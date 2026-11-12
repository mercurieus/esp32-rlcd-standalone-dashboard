#include "net_time_convert.hpp"

namespace net_time {

void epoch_to_local(std::time_t epoch, app_core::RtcDateTime& out) {
  std::tm local{};
  localtime_r(&epoch, &local);
  out.year = static_cast<uint16_t>(local.tm_year + 1900);
  out.month = static_cast<uint8_t>(local.tm_mon + 1);
  out.day = static_cast<uint8_t>(local.tm_mday);
  out.hour = static_cast<uint8_t>(local.tm_hour);
  out.minute = static_cast<uint8_t>(local.tm_min);
  out.second = static_cast<uint8_t>(local.tm_sec);
}

uint32_t ms_until_next_minute(int64_t epoch_ms) {
  const int64_t into_minute = ((epoch_ms % 60'000) + 60'000) % 60'000;
  return static_cast<uint32_t>(60'000 - into_minute) + kMinuteMarginMs;
}

}  // namespace net_time
