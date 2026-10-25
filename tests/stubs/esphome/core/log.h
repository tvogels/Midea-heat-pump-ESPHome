// Host stub: ESP_LOGx append formatted lines to log_lines() for the tests.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

inline std::vector<std::string> &log_lines() {
  static std::vector<std::string> lines;
  return lines;
}

__attribute__((format(printf, 3, 4))) inline void log_line(char level, const char *tag, const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  log_lines().push_back(std::string(1, level) + " " + tag + ": " + buf);
}

#define ESP_LOGD(tag, ...) log_line('D', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) log_line('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) log_line('W', tag, __VA_ARGS__)
