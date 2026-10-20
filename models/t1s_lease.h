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
// Nothing else is ever written. In particular register 0 (power / DHW) is
// never read-modify-written by this code.
//
// Every write is a read-modify-write that starts from a *fresh* read of the
// register (the wired controller shares the bus), touches only the masked
// bits, is skipped if the bits already have the desired value (EEPROM
// endurance), and is verified by a read-back with bounded retries.
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

static const int T1S_MIN = 20;               // site floor for supervised heating targets
static const int T1S_MAX = 40;               // UFH screed cap (matches the zwwg number max_value)
static const uint32_t LEASE_MIN_S = 10;
static const uint32_t LEASE_MAX_S = 2 * 3600;  // hard cap: 2 h
static const uint32_t RESPONSE_TIMEOUT_MS = 5000;
static const uint32_t VERIFY_DELAY_MS = 3000;  // let the unit apply the write before reading back
static const uint8_t MAX_ATTEMPTS = 3;          // per register operation
static const uint32_t RESTORE_BACKOFF_MS = 60000;
static const uint32_t BOOT_RESTORE_DELAY_MS = 30000;
static const uint32_t SNAPSHOT_MAGIC = 0x7153A001;

// Persisted only on supervision start and cleared after a verified restore:
// two flash writes per supervision session, none on renewals.
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
  uint16_t bits;  // desired value of the masked bits
};

enum class Phase : uint8_t { IDLE, SNAPSHOT_READ, OP_READ, OP_VERIFY_WAIT, OP_VERIFY_READ, BACKOFF };

class T1sLease {
 public:
  void setup(ModbusController *dev, uint16_t *shadow_reg2, uint16_t *shadow_reg5) {
    dev_ = dev;
    shadow_reg2_ = shadow_reg2;
    shadow_reg5_ = shadow_reg5;
    pref_ = esphome::global_preferences->make_preference<Snapshot>(0x7153A5E1u, true);
    Snapshot s{};
    if (pref_.load(&s) && s.magic == SNAPSHOT_MAGIC && s.pending) {
      snap_ = s;
      ESP_LOGW(TAG, "Boot: pending snapshot found (WC z1 %s, T1S z1 %u) -> restore in %us",
               s.wc_z1_was_on ? "on" : "off", s.t1s_z1, BOOT_RESTORE_DELAY_MS / 1000);
      want_restore_ = true;
      phase_ = Phase::BACKOFF;
      phase_ts_ = esphome::millis();
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
    if (want_restore_) return error_("rejected: restore in progress");

    target_ = (uint8_t) t1s;
    start_lease_(lease_s);
    if (!active_) {
      // First supervised call: snapshot from a fresh read, then take over.
      active_ = true;
      have_snapshot_ = false;
      ESP_LOGI(TAG, "Supervision requested: T1S z1=%d lease=%ds", t1s, lease_s);
      begin_snapshot_();
    } else if (t1s != last_requested_) {
      ESP_LOGI(TAG, "Supervised T1S change -> %d", t1s);
      enqueue_({REG_T1S, MASK_T1S_Z1, (uint16_t) t1s});
    }
    // Same value while active: lease renewal only, no Modbus traffic.
    last_requested_ = t1s;
  }

  void renew(int lease_s) {
    if (!active_ || want_restore_) return error_("renew ignored: not supervising");
    if (lease_s < (int) LEASE_MIN_S) return error_("rejected lease_s < 10");
    start_lease_(lease_s);  // no Modbus, no flash
  }

  void release() {
    if (!active_) return;
    ESP_LOGI(TAG, "Supervision released by caller");
    request_restore_();
  }

  // --- called every second from an interval ---------------------------------

  void loop() {
    uint32_t now = esphome::millis();
    if (active_ && !want_restore_ && lease_remaining_s() == 0) {
      ESP_LOGW(TAG, "Lease expired -> restoring");
      request_restore_();
    }
    switch (phase_) {
      case Phase::SNAPSHOT_READ:
      case Phase::OP_READ:
      case Phase::OP_VERIFY_READ:
        if (now - phase_ts_ > RESPONSE_TIMEOUT_MS) {
          token_++;  // ignore a late response
          attempt_failed_("no Modbus response");
        }
        break;
      case Phase::OP_VERIFY_WAIT:
        if (now - phase_ts_ >= VERIFY_DELAY_MS) read_(Phase::OP_VERIFY_READ, ops_.front().reg, 1);
        break;
      case Phase::BACKOFF:
        if (now - phase_ts_ >= backoff_ms_) {
          phase_ = Phase::IDLE;
          if (want_restore_) start_restore_ops_();
        }
        break;
      case Phase::IDLE:
        break;
    }
  }

  // --- status ---------------------------------------------------------------

  bool active() const { return active_ || want_restore_; }
  uint32_t lease_remaining_s() const {
    if (!active_) return 0;
    uint32_t elapsed = esphome::millis() - lease_start_ms_;  // unsigned: wrap-safe
    return elapsed >= lease_ms_ ? 0 : (lease_ms_ - elapsed + 999) / 1000;
  }
  const std::string &last_error() const { return last_error_; }
  uint32_t restore_count() const { return restore_count_; }

 protected:
  void error_(const std::string &msg) {
    ESP_LOGE(TAG, "%s", msg.c_str());
    last_error_ = msg;
  }

  void start_lease_(int lease_s) {
    uint32_t s = (uint32_t) lease_s;
    if (s > LEASE_MAX_S) s = LEASE_MAX_S;
    lease_ms_ = s * 1000;
    lease_start_ms_ = esphome::millis();
  }

  void request_restore_() {
    active_ = false;
    lease_ms_ = 0;
    if (!have_snapshot_) {
      // Nothing was changed yet (snapshot never succeeded): nothing to undo.
      ops_.clear();
      if (phase_ == Phase::SNAPSHOT_READ) token_++;
      phase_ = Phase::IDLE;
      return;
    }
    want_restore_ = true;
    if (phase_ == Phase::IDLE) start_restore_ops_();
    // Otherwise: finish the in-flight op, then restore (see next_op_()).
  }

  void start_restore_ops_() {
    ops_.clear();
    // T1S first, then re-enable the curve.
    ops_.push_back({REG_T1S, MASK_T1S_Z1, snap_.t1s_z1});
    if (snap_.wc_z1_was_on) ops_.push_back({REG_OPTIONS, MASK_WC_Z1, MASK_WC_Z1});
    restoring_ = true;
    next_op_();
  }

  void begin_snapshot_() { read_(Phase::SNAPSHOT_READ, REG_T1S, REG_OPTIONS - REG_T1S + 1); }

  void enqueue_(Op op) {
    // Replace a not-yet-started op on the same register (latest target wins).
    for (size_t i = (phase_ == Phase::IDLE ? 0 : 1); i < ops_.size(); i++) {
      if (ops_[i].reg == op.reg && ops_[i].mask == op.mask) {
        ops_[i] = op;
        return;
      }
    }
    ops_.push_back(op);
    if (phase_ == Phase::IDLE) next_op_();
  }

  void next_op_() {
    attempts_ = 0;
    if (want_restore_ && !restoring_) {
      // Lease expired / released mid-takeover: drop remaining takeover ops.
      start_restore_ops_();
      return;
    }
    if (!ops_.empty()) {
      read_(Phase::OP_READ, ops_.front().reg, 1);
      return;
    }
    phase_ = Phase::IDLE;
    if (restoring_) {
      restoring_ = false;
      want_restore_ = false;
      have_snapshot_ = false;
      restore_count_++;
      snap_.pending = 0;
      pref_.save(&snap_);
      esphome::global_preferences->sync();
      ESP_LOGI(TAG, "Restore verified (WC z1 %s, T1S z1 %u)", snap_.wc_z1_was_on ? "on" : "off", snap_.t1s_z1);
    } else if (want_restore_) {
      start_restore_ops_();
    }
  }

  void read_(Phase phase, uint16_t reg, uint16_t count) {
    phase_ = phase;
    phase_ts_ = esphome::millis();
    uint32_t tok = ++token_;
    auto cmd = ModbusCommandItem::create_read_command(
        dev_, ModbusRegisterType::HOLDING, reg, count,
        [this, tok, reg](ModbusRegisterType, uint16_t, const std::vector<uint8_t> &data) {
          if (tok != token_) return;
          std::vector<uint16_t> regs;
          for (size_t i = 0; i + 1 < data.size(); i += 2) regs.push_back((uint16_t(data[i]) << 8) | data[i + 1]);
          on_read_(reg, regs);
        });
    dev_->queue_command(cmd);
  }

  void on_read_(uint16_t reg, const std::vector<uint16_t> &regs) {
    if (regs.empty()) return attempt_failed_("empty Modbus response");
    if (phase_ == Phase::SNAPSHOT_READ) {
      if (regs.size() < 4) return attempt_failed_("short snapshot read");
      uint16_t v2 = regs[0], v5 = regs[3];
      snap_.magic = SNAPSHOT_MAGIC;
      snap_.wc_z1_was_on = (v5 & MASK_WC_Z1) ? 1 : 0;
      snap_.t1s_z1 = v2 & MASK_T1S_Z1;
      snap_.pending = 1;
      snap_.reserved = 0;
      have_snapshot_ = true;
      pref_.save(&snap_);
      esphome::global_preferences->sync();  // must survive an immediate reboot
      ESP_LOGI(TAG, "Snapshot: WC z1 %s, T1S z1 %u", snap_.wc_z1_was_on ? "on" : "off", snap_.t1s_z1);
      phase_ = Phase::IDLE;
      if (want_restore_) {
        start_restore_ops_();
        return;
      }
      if (snap_.wc_z1_was_on) ops_.push_back({REG_OPTIONS, MASK_WC_Z1, 0});
      ops_.push_back({REG_T1S, MASK_T1S_Z1, target_});
      next_op_();
      return;
    }
    const Op &op = ops_.front();
    uint16_t v = regs[0];
    update_shadow_(reg, v);
    if ((v & op.mask) == op.bits) {  // already as desired (or write verified)
      ops_.pop_front();
      next_op_();
      return;
    }
    if (phase_ == Phase::OP_VERIFY_READ) return attempt_failed_("read-back mismatch reg " + std::to_string(reg));
    uint16_t nv = (v & ~op.mask) | op.bits;
    ESP_LOGI(TAG, "Write reg %u: 0x%04X -> 0x%04X", reg, v, nv);
    dev_->queue_command(ModbusCommandItem::create_write_single_command(dev_, reg, nv));
    update_shadow_(reg, nv);
    phase_ = Phase::OP_VERIFY_WAIT;
    phase_ts_ = esphome::millis();
  }

  void attempt_failed_(const std::string &why) {
    attempts_++;
    if (phase_ == Phase::SNAPSHOT_READ) {
      if (attempts_ < MAX_ATTEMPTS && !want_restore_) return begin_snapshot_();
      // Nothing written yet: abort supervision without touching anything.
      active_ = false;
      want_restore_ = false;
      lease_ms_ = 0;
      phase_ = Phase::IDLE;
      return error_("snapshot read failed (" + why + "), supervision not started");
    }
    if (attempts_ < MAX_ATTEMPTS) return read_(Phase::OP_READ, ops_.front().reg, 1);
    error_("reg " + std::to_string(ops_.front().reg) + " op failed after retries: " + why);
    ops_.clear();
    if (restoring_ || !active_) {
      // Restore failed: keep the persisted snapshot and try again later.
      restoring_ = false;
      want_restore_ = true;
      phase_ = Phase::BACKOFF;
      phase_ts_ = esphome::millis();
      backoff_ms_ = RESTORE_BACKOFF_MS;
    } else {
      // Takeover failed half-way: undo whatever did get applied.
      phase_ = Phase::IDLE;
      request_restore_();
    }
  }

  void update_shadow_(uint16_t reg, uint16_t v) {
    if (reg == REG_T1S && shadow_reg2_) *shadow_reg2_ = v;
    if (reg == REG_OPTIONS && shadow_reg5_) *shadow_reg5_ = v;
  }

  ModbusController *dev_{nullptr};
  uint16_t *shadow_reg2_{nullptr};
  uint16_t *shadow_reg5_{nullptr};
  esphome::ESPPreferenceObject pref_;
  Snapshot snap_{};
  bool have_snapshot_{false};
  bool active_{false};
  bool want_restore_{false};
  bool restoring_{false};
  uint8_t target_{0};
  int last_requested_{-1};
  uint32_t lease_start_ms_{0};
  uint32_t lease_ms_{0};
  std::deque<Op> ops_;
  Phase phase_{Phase::IDLE};
  uint32_t phase_ts_{0};
  uint32_t backoff_ms_{0};
  uint32_t token_{0};
  uint8_t attempts_{0};
  uint32_t restore_count_{0};
  std::string last_error_;
};

inline T1sLease &instance() {
  static T1sLease lease;
  return lease;
}

}  // namespace t1s_lease
