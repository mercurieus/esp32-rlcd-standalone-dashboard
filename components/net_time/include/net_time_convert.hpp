#pragma once

#include <cstdint>
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

// Slack added past the minute boundary before the clock is read. The wait is
// a FreeRTOS tick count (10 ms here) and the wake-up can land a hair early;
// waking at :59.999 would publish the old minute and the panel would show it
// for a whole extra minute. 50 ms later is imperceptible and never early.
inline constexpr uint32_t kMinuteMarginMs = 50;

// Milliseconds from epoch_ms (Unix ms, UTC) to the next :00 plus
// kMinuteMarginMs. Never 0 (exactly on a boundary waits a full minute, the
// current minute having just been published) and never above 60'050.
uint32_t ms_until_next_minute(int64_t epoch_ms);

}  // namespace net_time
