// t1s_lease.h — supervised Zone 1 supply-temperature target (T1S) with an
// on-device lease.
//
// An external supervisor (Homage) sets T1S zone 1 directly while weather
// compensation for zone 1 is temporarily disabled. The supervisor must keep
// renewing a lease; if it stops (crash, network loss, ESP reboot), the ESP
// restores the original settings by itself.
//
// Register contract (Modbus holding registers, see README):
//   reg 2  T1S, packed: zone 1 = low byte, zone 2 = high byte  -> mask 0x00FF
//   reg 5  options, bit 0x1000 = weather compensation zone 1    -> mask 0x1000
// Nothing else is ever written. Registers 0/6/7/8/212 are never written.
// Register 0 is only *observed* (via the stock poll shadow) to log DHW-bit
// drops that follow one of our writes.
//
// Every write is a read-modify-write that starts from a *fresh* read of the
// register (the wired controller shares the bus), touches only the masked
// bits, is skipped if the bits already have the desired value (EEPROM
// endurance), and is verified by a read-back with bounded retries.
//
// Heat-pump EEPROM budget:
//   * mid-lease T1S changes are rate limited to one write per
//     min_write_interval (default 900 s) and always carry the *latest*
//     target; a target equal to the applied value writes nothing.
//   * restore retries back off exponentially (60 s doubling, capped at 1 h),
//     retry only the failed ops, and stop writing after MAX_RESTORE_CYCLES.
#pragma once

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/modbus_controller/modbus_controller.h"

#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace t1s_lease {

using esphome::modbus_controller::ModbusCommandItem;
using esphome::modbus_controller::ModbusController;
using esphome::modbus_controller::ModbusRegisterType;

static const char *const TAG = "t1s_lease";

static const uint16_t REG_T1S = 2;
static const uint16_t REG_OPTIONS = 5;
static const uint16_t MASK_T1S_Z1 = 0x00FF;
static const uint16_t MASK_WC_Z1 = 0x1000;
static const uint16_t MASK_REG0_DHW = 0x0004;  // observed only, never written

static const int T1S_MIN = 20;               // site floor for supervised heating targets
static const int T1S_MAX = 40;               // UFH screed cap (matches the zwwg number max_value)
static const uint8_t SNAP_T1S_MIN = 5;       // plausible range of a snapshot T1S value
static const uint8_t SNAP_T1S_MAX = 65;
static const uint32_t LEASE_MIN_S = 10;
static const uint32_t LEASE_MAX_S = 2 * 3600;  // hard cap: 2 h
static const uint32_t RESPONSE_TIMEOUT_MS = 15000;
static const uint32_t VERIFY_DELAY_MS = 3000;   // let the unit apply the write before reading back
static const uint8_t MAX_ATTEMPTS = 3;          // per register operation and cycle
static const uint32_t RESTORE_BACKOFF_MIN_MS = 60000;
static const uint32_t RESTORE_BACKOFF_MAX_MS = 3600000;
static const uint8_t MAX_RESTORE_CYCLES = 8;    // 1+2+4+...+60 min ≈ 2.1 h of retries, then stop writing
static const uint32_t BOOT_RESTORE_DELAY_MS = 30000;
static const uint32_t DRIFT_GRACE_MS = 30000;   // > stock poll interval (10 s) after our last I/O
static const uint32_t DHW_WATCH_MS = 30000;
static const uint32_t SNAPSHOT_MAGIC = 0x7153A001;

// Persisted only on supervision start and cleared after a verified restore
// (or when a user change is adopted): two flash writes per session, none on
// renewals.
struct Snapshot {
  uint32_t magic;
  uint8_t wc_z1_was_on;
  uint8_t t1s_z1;
  uint8_t pending;  // 1 = a restore is owed
  uint8_t reserved;
};

// One masked read-modify-write with read-back verification.
struct Op {
  uint16_t reg;
  uint16_t mask;
  uint16_t bits;        // desired value of the masked bits
  bool follow_target;   // T1S op whose bits track the latest target_
};

// Single lifecycle state (replaces the former active_/want_restore_/restoring_ flags).
enum class Mode : uint8_t {
  IDLE,
  TAKEOVER,         // snapshot + WC off + T1S target
  SUPERVISING,      // lease running; rate-limited T1S changes
  RESTORING,        // restore ops in flight
  RESTORE_BACKOFF,  // waiting to retry failed restore ops (or boot delay)
  RESTORE_HALTED,   // gave up writing; snapshot stays pending
};

enum class Io : uint8_t { NONE, SNAP_READ, OP_READ, VERIFY_WAIT, VERIFY_READ };

class T1sLease {
 public:
  void setup(ModbusController *dev, uint16_t *shadow_reg0, uint16_t *shadow_reg2, uint16_t *shadow_reg5,
             uint32_t min_write_interval_s) {
    dev_ = dev;
    shadow_reg0_ = shadow_reg0;
    shadow_reg2_ = shadow_reg2;
    shadow_reg5_ = shadow_reg5;
    min_write_interval_ms_ = min_write_interval_s * 1000;
    pref_ = esphome::global_preferences->make_preference<Snapshot>(0x7153A5E1u, true);
    Snapshot s{};
    if (pref_.load(&s) && s.magic == SNAPSHOT_MAGIC && s.pending) {
      snap_ = s;
      have_snapshot_ = true;
      ESP_LOGW(TAG, "Boot: pending snapshot found (WC z1 %s, T1S z1 %u) -> restore in %us",
               s.wc_z1_was_on ? "on" : "off", s.t1s_z1, BOOT_RESTORE_DELAY_MS / 1000);
      build_restore_ops_();
      restore_cycles_ = 0;
      set_mode_(Mode::RESTORE_BACKOFF);
      backoff_ts_ = esphome::millis();
      backoff_ms_ = BOOT_RESTORE_DELAY_MS;
    }
  }

  // --- API entry points -----------------------------------------------------

  void set_supervised(int t1s, int lease_s, float unit_min, float unit_max) {
    if (dev_ == nullptr) return;
    if (t1s < T1S_MIN || t1s > T1S_MAX) {
      return error_("rejected t1s=" + std::to_string(t1s) + " (allowed " + std::to_string(T1S_MIN) + ".." +
                    std::to_string(T1S_MAX) + ")");
    }
    // Unit's own T1S heating limits (regs 203/204), when they have been read.
    if (!std::isnan(unit_min) && unit_min > 0 && t1s < unit_min) return error_("rejected t1s below unit lower limit");
    if (!std::isnan(unit_max) && unit_max > 0 && t1s > unit_max) return error_("rejected t1s above unit upper limit");
    if (lease_s < (int) LEASE_MIN_S) return error_("rejected lease_s < 10");
    if (restore_owed_()) return error_("rejected: restore pending");

    target_ = (uint8_t) t1s;  // latest target always wins; written lazily
    start_lease_(lease_s);
    if (mode_ == Mode::IDLE) {
      ESP_LOGI(TAG, "Supervision requested: T1S z1=%d lease=%ds", t1s, lease_s);
      release_pending_ = false;
      have_snapshot_ = false;
      set_mode_(Mode::TAKEOVER);
      attempts_ = 0;
      begin_snapshot_();
    }
    // TAKEOVER: the T1S op picks up target_ when it starts (after WC off).
    // SUPERVISING: loop() writes target_ when the rate limit allows.
  }

  void renew(int lease_s) {
    if (mode_ != Mode::TAKEOVER && mode_ != Mode::SUPERVISING) return error_("renew ignored: not supervising");
    if (lease_s < (int) LEASE_MIN_S) return error_("rejected lease_s < 10");
    start_lease_(lease_s);  // no Modbus, no flash
  }

  void release() {
    if (mode_ == Mode::RESTORE_HALTED) {
      ESP_LOGW(TAG, "Release while halted: re-arming restore (one more backoff series)");
      restore_cycles_ = 0;
      start_restore_cycle_();
      return;
    }
    if (mode_ != Mode::TAKEOVER && mode_ != Mode::SUPERVISING) return;
    ESP_LOGI(TAG, "Supervision released by caller");
    end_supervision_();
  }

  // --- called every second from an interval ---------------------------------

  void loop() {
    uint32_t now = esphome::millis();
    observe_dhw_(now);

    if ((mode_ == Mode::TAKEOVER || mode_ == Mode::SUPERVISING) && !release_pending_ && lease_remaining_s() == 0) {
      ESP_LOGW(TAG, "Lease expired -> restoring");
      end_supervision_();
    }

    switch (io_) {
      case Io::SNAP_READ:
      case Io::OP_READ:
      case Io::VERIFY_READ:
        if (now - io_ts_ > RESPONSE_TIMEOUT_MS) attempt_failed_("no Modbus response");
        break;
      case Io::VERIFY_WAIT:
        if (now - io_ts_ >= VERIFY_DELAY_MS) {
          accept_seq_ = seq_ + 1;  // only reads issued after the write count as read-back
          read_(Io::VERIFY_READ, ops_.front().reg, 1);
        }
        break;
      case Io::NONE:
        break;
    }

    if (mode_ == Mode::SUPERVISING && io_ == Io::NONE && ops_.empty()) {
      if (check_drift_(now)) return;
      if (target_ != applied_t1s_ && (!t1s_written_ || now - last_t1s_write_ms_ >= min_write_interval_ms_)) {
        ESP_LOGI(TAG, "Supervised T1S change %u -> %u", applied_t1s_, target_);
        ops_.push_back({REG_T1S, MASK_T1S_Z1, target_, true});
        next_op_();
      }
    }

    if (mode_ == Mode::RESTORE_BACKOFF && now - backoff_ts_ >= backoff_ms_) start_restore_cycle_();
  }

  // --- status ---------------------------------------------------------------

  bool active() const { return mode_ != Mode::IDLE; }
  const char *mode_name() const {
    switch (mode_) {
      case Mode::IDLE: return "idle";
      case Mode::TAKEOVER: return "takeover";
      case Mode::SUPERVISING: return "supervising";
      case Mode::RESTORING: return "restoring";
      case Mode::RESTORE_BACKOFF: return "restore_backoff";
      case Mode::RESTORE_HALTED: return "restore_halted";
    }
    return "?";
  }
  uint32_t lease_remaining_s() const {
    if (mode_ != Mode::TAKEOVER && mode_ != Mode::SUPERVISING) return 0;
    uint32_t elapsed = esphome::millis() - lease_start_ms_;  // unsigned: wrap-safe
    return elapsed >= lease_ms_ ? 0 : (lease_ms_ - elapsed + 999) / 1000;
  }
  const std::string &last_error() const { return last_error_; }
  uint32_t restore_count() const { return restore_count_; }
  uint32_t write_count() const { return write_count_; }
  uint32_t dhw_drop_count() const { return dhw_drops_; }
  uint32_t drift_count() const { return drift_count_; }
  // True once after any status transition; used to publish sensors promptly.
  bool consume_changed() {
    bool c = changed_;
    changed_ = false;
    return c;
  }

 protected:
  bool restore_owed_() const {
    return mode_ == Mode::RESTORING || mode_ == Mode::RESTORE_BACKOFF || mode_ == Mode::RESTORE_HALTED;
  }

  void set_mode_(Mode m) {
    if (m != mode_) {
      mode_ = m;
      changed_ = true;
      ESP_LOGD(TAG, "mode -> %s", mode_name());
    }
  }

  void error_(const std::string &msg) {
    ESP_LOGE(TAG, "%s", msg.c_str());
    // Keep the persistent "halted" error visible; rejections then only go to the log.
    if (mode_ == Mode::RESTORE_HALTED && msg.rfind("rejected", 0) == 0) return;
    last_error_ = msg;
    changed_ = true;
  }

  void start_lease_(int lease_s) {
    uint32_t s = (uint32_t) lease_s;
    if (s > LEASE_MAX_S) s = LEASE_MAX_S;
    lease_ms_ = s * 1000;
    lease_start_ms_ = esphome::millis();
  }

  void persist_(uint8_t pending) {
    snap_.pending = pending;
    pref_.save(&snap_);
    esphome::global_preferences->sync();
  }

  // Lease expired or released (TAKEOVER or SUPERVISING).
  void end_supervision_() {
    lease_ms_ = 0;
    if (mode_ == Mode::TAKEOVER && !have_snapshot_) {
      // Nothing was changed yet (snapshot not taken): nothing to undo.
      accept_seq_ = ++seq_;  // ignore the in-flight snapshot response
      io_ = Io::NONE;
      ops_.clear();
      set_mode_(Mode::IDLE);
      return;
    }
    if (io_ != Io::NONE) {
      release_pending_ = true;  // finish the in-flight op, then restore (see next_op_)
      return;
    }
    ops_.clear();
    begin_restore_();
  }

  void build_restore_ops_() {
    restore_ops_.clear();
    // WC first: it is the actual safety net and must not wait on T1S.
    if (snap_.wc_z1_was_on) restore_ops_.push_back({REG_OPTIONS, MASK_WC_Z1, MASK_WC_Z1, false});
    if (snap_.t1s_z1 >= SNAP_T1S_MIN && snap_.t1s_z1 <= SNAP_T1S_MAX) {
      restore_ops_.push_back({REG_T1S, MASK_T1S_Z1, snap_.t1s_z1, false});
    } else {
      error_("snapshot T1S " + std::to_string(snap_.t1s_z1) + " out of range 5..65, T1S not restored");
    }
  }

  void begin_restore_() {
    release_pending_ = false;
    build_restore_ops_();
    restore_cycles_ = 0;
    start_restore_cycle_();
  }

  void start_restore_cycle_() {
    ops_.assign(restore_ops_.begin(), restore_ops_.end());
    failed_.clear();
    set_mode_(Mode::RESTORING);
    next_op_();
  }

  void begin_snapshot_() {
    accept_seq_ = seq_ + 1;
    read_(Io::SNAP_READ, REG_T1S, REG_OPTIONS - REG_T1S + 1);
  }

  void next_op_() {
    attempts_ = 0;
    if (release_pending_ && (mode_ == Mode::TAKEOVER || mode_ == Mode::SUPERVISING)) {
      ops_.clear();
      io_ = Io::NONE;
      begin_restore_();
      return;
    }
    if (!ops_.empty()) {
      Op &op = ops_.front();
      if (op.follow_target) op.bits = target_;
      accept_seq_ = seq_ + 1;
      read_(Io::OP_READ, op.reg, 1);
      return;
    }
    io_ = Io::NONE;
    last_io_end_ms_ = esphome::millis();
    switch (mode_) {
      case Mode::TAKEOVER:
        set_mode_(Mode::SUPERVISING);
        ESP_LOGI(TAG, "Supervising: WC z1 off, T1S z1 %u", applied_t1s_);
        break;
      case Mode::RESTORING:
        if (failed_.empty()) {
          restore_count_++;
          have_snapshot_ = false;
          persist_(0);
          set_mode_(Mode::IDLE);
          ESP_LOGI(TAG, "Restore verified (WC z1 %s, T1S z1 %u)", snap_.wc_z1_was_on ? "on" : "off", snap_.t1s_z1);
        } else {
          restore_ops_ = failed_;  // retry only what failed
          restore_cycles_++;
          if (restore_cycles_ >= MAX_RESTORE_CYCLES) {
            set_mode_(Mode::RESTORE_HALTED);
            error_("RESTORE HALTED after " + std::to_string(restore_cycles_) +
                   " cycles: no more heat-pump writes until reboot or release_supervision; snapshot kept");
          } else {
            uint32_t b = RESTORE_BACKOFF_MIN_MS << (restore_cycles_ - 1);
            backoff_ms_ = b > RESTORE_BACKOFF_MAX_MS ? RESTORE_BACKOFF_MAX_MS : b;
            backoff_ts_ = esphome::millis();
            set_mode_(Mode::RESTORE_BACKOFF);
            ESP_LOGW(TAG, "Restore incomplete (%u ops failed), retry in %us", (unsigned) failed_.size(),
                     (unsigned) (backoff_ms_ / 1000));
          }
        }
        break;
      default:
        break;
    }
  }

  void read_(Io io, uint16_t reg, uint16_t count) {
    io_ = io;
    io_ts_ = esphome::millis();
    uint32_t seq = ++seq_;
    auto cmd = ModbusCommandItem::create_read_command(
        dev_, ModbusRegisterType::HOLDING, reg, count,
        [this, seq, reg, count](ModbusRegisterType, uint16_t, const std::vector<uint8_t> &data) {
          // A retry may be merged by queue_command() into a still-queued
          // earlier read of the same reg/count, which keeps that earlier
          // callback. Accept any response issued since the current step began
          // (accept_seq_ is kept across retries), so a merge still completes.
          if (seq < accept_seq_) return;
          if (io_ != Io::SNAP_READ && io_ != Io::OP_READ && io_ != Io::VERIFY_READ) return;
          bool snap = io_ == Io::SNAP_READ;
          uint16_t want_reg = snap ? REG_T1S : (ops_.empty() ? 0xFFFF : ops_.front().reg);
          uint16_t want_cnt = snap ? (REG_OPTIONS - REG_T1S + 1) : 1;
          if (reg != want_reg || count != want_cnt) return;
          std::vector<uint16_t> regs;
          for (size_t i = 0; i + 1 < data.size(); i += 2) regs.push_back((uint16_t(data[i]) << 8) | data[i + 1]);
          on_read_(reg, regs);
        });
    dev_->queue_command(cmd);
  }

  void on_read_(uint16_t reg, const std::vector<uint16_t> &regs) {
    if (regs.empty()) return attempt_failed_("empty Modbus response");
    if (io_ == Io::SNAP_READ) {
      if (regs.size() < 4) return attempt_failed_("short snapshot read");
      uint16_t v2 = regs[0], v5 = regs[3];
      update_shadow_(REG_T1S, v2);
      update_shadow_(REG_OPTIONS, v5);
      snap_.magic = SNAPSHOT_MAGIC;
      snap_.wc_z1_was_on = (v5 & MASK_WC_Z1) ? 1 : 0;
      snap_.t1s_z1 = v2 & MASK_T1S_Z1;
      snap_.reserved = 0;
      have_snapshot_ = true;
      persist_(1);  // must survive an immediate reboot
      applied_t1s_ = snap_.t1s_z1;
      ESP_LOGI(TAG, "Snapshot: WC z1 %s, T1S z1 %u", snap_.wc_z1_was_on ? "on" : "off", snap_.t1s_z1);
      io_ = Io::NONE;
      // WC off strictly before any T1S write.
      ops_.clear();
      if (snap_.wc_z1_was_on) ops_.push_back({REG_OPTIONS, MASK_WC_Z1, 0, false});
      ops_.push_back({REG_T1S, MASK_T1S_Z1, target_, true});
      next_op_();
      return;
    }
    Op &op = ops_.front();
    uint16_t v = regs[0];
    update_shadow_(reg, v);  // real read: the shadow follows the unit, never our intent
    if (reg == REG_T1S) applied_t1s_ = v & MASK_T1S_Z1;
    if ((v & op.mask) == op.bits) return op_done_(true, "");  // already as desired, or verified
    if (io_ == Io::VERIFY_READ) return attempt_failed_("read-back mismatch reg " + std::to_string(reg));
    uint16_t nv = (v & ~op.mask) | op.bits;
    ESP_LOGI(TAG, "Write reg %u: 0x%04X -> 0x%04X (heat-pump write #%u since boot)", reg, v, nv,
             (unsigned) (write_count_ + 1));
    dev_->queue_command(ModbusCommandItem::create_write_single_command(dev_, reg, nv));
    uint32_t now = esphome::millis();
    write_count_++;
    changed_ = true;
    last_write_ms_ = now;
    any_write_ = true;
    if (reg == REG_T1S) {
      last_t1s_write_ms_ = now;
      t1s_written_ = true;
    }
    io_ = Io::VERIFY_WAIT;
    io_ts_ = now;
  }

  void attempt_failed_(const std::string &why) {
    attempts_++;
    if (io_ == Io::SNAP_READ) {
      if (attempts_ < MAX_ATTEMPTS) return read_(Io::SNAP_READ, REG_T1S, REG_OPTIONS - REG_T1S + 1);
      // Nothing written yet: abort supervision without touching anything.
      io_ = Io::NONE;
      lease_ms_ = 0;
      release_pending_ = false;
      set_mode_(Mode::IDLE);
      return error_("snapshot read failed (" + why + "), supervision not started");
    }
    // Retry from a fresh read; accept_seq_ is kept so a late reply to a
    // merged earlier read still counts.
    if (attempts_ < MAX_ATTEMPTS) return read_(Io::OP_READ, ops_.front().reg, 1);
    op_done_(false, why);
  }

  void op_done_(bool ok, const std::string &why) {
    Op op = ops_.front();
    ops_.pop_front();
    last_io_end_ms_ = esphome::millis();
    if (!ok) {
      error_("reg " + std::to_string(op.reg) + " op failed after retries: " + why);
      switch (mode_) {
        case Mode::RESTORING:
          failed_.push_back(op);  // independent ops: continue with the rest
          break;
        case Mode::TAKEOVER:
          // Takeover failed half-way: undo whatever did get applied.
          ops_.clear();
          io_ = Io::NONE;
          begin_restore_();
          return;
        case Mode::SUPERVISING:
          // Keep the lease; retry at the next rate-limit slot.
          last_t1s_write_ms_ = esphome::millis();
          t1s_written_ = true;
          break;
        default:
          break;
      }
    }
    next_op_();
  }

  // Someone else (HA stock entities, wired controller) changed reg 2/5 during
  // the lease: adopt their change and end supervision; never fight it.
  bool check_drift_(uint32_t now) {
    if (now - last_io_end_ms_ < DRIFT_GRACE_MS) return false;
    if (shadow_reg2_ == nullptr || shadow_reg5_ == nullptr) return false;
    uint16_t r2 = *shadow_reg2_, r5 = *shadow_reg5_;
    std::string what;
    if (r5 & MASK_WC_Z1) {
      what = "WC z1 switched on";
    } else if ((r2 & MASK_T1S_Z1) != applied_t1s_) {
      what = "T1S z1 " + std::to_string(applied_t1s_) + " -> " + std::to_string(r2 & MASK_T1S_Z1);
    }
    if (what.empty()) return false;
    drift_count_++;
    have_snapshot_ = false;
    lease_ms_ = 0;
    persist_(0);  // adopt the user's setting: no restore owed
    set_mode_(Mode::IDLE);
    error_("external change during lease (" + what + "): supervision ended, user setting adopted, no restore");
    return true;
  }

  void observe_dhw_(uint32_t now) {
    if (shadow_reg0_ == nullptr) return;
    bool on = (*shadow_reg0_ & MASK_REG0_DHW) != 0;
    if (dhw_known_ && dhw_prev_ && !on) {
      if (any_write_ && now - last_write_ms_ <= DHW_WATCH_MS) {
        dhw_drops_++;
        changed_ = true;
        ESP_LOGW(TAG, "DHW bit (reg 0 0x0004) dropped %us after a lease write (count %u)",
                 (unsigned) ((now - last_write_ms_) / 1000), (unsigned) dhw_drops_);
      } else {
        ESP_LOGI(TAG, "DHW bit (reg 0 0x0004) dropped, not within 30 s of a lease write");
      }
    }
    dhw_prev_ = on;
    dhw_known_ = true;
  }

  void update_shadow_(uint16_t reg, uint16_t v) {
    if (reg == REG_T1S && shadow_reg2_) *shadow_reg2_ = v;
    if (reg == REG_OPTIONS && shadow_reg5_) *shadow_reg5_ = v;
  }

  ModbusController *dev_{nullptr};
  uint16_t *shadow_reg0_{nullptr};
  uint16_t *shadow_reg2_{nullptr};
  uint16_t *shadow_reg5_{nullptr};
  esphome::ESPPreferenceObject pref_;
  Snapshot snap_{};
  bool have_snapshot_{false};
  Mode mode_{Mode::IDLE};
  Io io_{Io::NONE};
  bool release_pending_{false};
  uint8_t target_{0};
  uint8_t applied_t1s_{0};
  uint32_t min_write_interval_ms_{900000};
  uint32_t lease_start_ms_{0};
  uint32_t lease_ms_{0};
  std::deque<Op> ops_;
  std::vector<Op> restore_ops_;
  std::vector<Op> failed_;
  uint8_t restore_cycles_{0};
  uint32_t io_ts_{0};
  uint32_t backoff_ts_{0};
  uint32_t backoff_ms_{0};
  uint32_t seq_{0};
  uint32_t accept_seq_{0};
  uint8_t attempts_{0};
  uint32_t last_write_ms_{0};
  uint32_t last_t1s_write_ms_{0};
  uint32_t last_io_end_ms_{0};
  bool any_write_{false};
  bool t1s_written_{false};
  bool dhw_known_{false};
  bool dhw_prev_{false};
  bool changed_{false};
  uint32_t restore_count_{0};
  uint32_t write_count_{0};
  uint32_t dhw_drops_{0};
  uint32_t drift_count_{0};
  std::string last_error_;
};

inline T1sLease &instance() {
  static T1sLease lease;
  return lease;
}

}  // namespace t1s_lease
