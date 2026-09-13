#pragma once

#include <ctime>

#include "app_snapshot.hpp"

// Pure epoch -> local-time conversion, no ESP dependency. Split out from
// net_time.hpp (which pulls in esp_err.h) so host tests can link this half
// without the ESP-IDF headers the SNTP half needs.
namespace net_time {

// POSIX TZ rule for the device's displayed local time: Ukraine (EET/EEST),
// UTC+2 standard / UTC+3 daylight, EU DST rules (last Sunday of March to
// last Sunday of October). No zoneinfo database lookup required.
// Device side: start() below calls setenv("TZ", kTimeZone, 1) + tzset().
// Host tests: do the same before calling epoch_to_local().
inline constexpr const char* kTimeZone = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// Breaks a UTC epoch down into the device's displayed local time (kTimeZone,
// via the active TZ).
void epoch_to_local(std::time_t epoch, app_core::RtcDateTime& out);

}  // namespace net_time
