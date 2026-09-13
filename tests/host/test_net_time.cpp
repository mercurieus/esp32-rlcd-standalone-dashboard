#include "net_time_convert.hpp"

#include <cstdlib>
#include <ctime>

#include "test_support.hpp"

namespace {
// net_time::epoch_to_local() reads the active TZ via localtime_r(); the
// device sets it once via net_time::start(), so tests do the same here.
struct TzGuard {
  TzGuard() {
    setenv("TZ", net_time::kTimeZone, 1);
    tzset();
  }
};
}  // namespace

HOST_TEST(epoch_to_local_converts_the_unix_epoch_to_kyiv_winter_time) {
  TzGuard tz;
  // 1970-01-01 00:00:00 UTC -> 1970-01-01 02:00:00 local (UTC+2, winter/EET).
  app_core::RtcDateTime out{};
  net_time::epoch_to_local(0, out);
  EXPECT_EQ(out.year, 1970);
  EXPECT_EQ(out.month, 1);
  EXPECT_EQ(out.day, 1);
  EXPECT_EQ(out.hour, 2);
  EXPECT_EQ(out.minute, 0);
  EXPECT_EQ(out.second, 0);
}

HOST_TEST(epoch_to_local_applies_the_utc_plus_3_offset_in_summer_dst) {
  TzGuard tz;
  // 2024-06-15 12:34:56 UTC -> 2024-06-15 15:34:56 local (UTC+3, EEST).
  app_core::RtcDateTime out{};
  net_time::epoch_to_local(1718454896, out);
  EXPECT_EQ(out.year, 2024);
  EXPECT_EQ(out.month, 6);
  EXPECT_EQ(out.day, 15);
  EXPECT_EQ(out.hour, 15);
  EXPECT_EQ(out.minute, 34);
  EXPECT_EQ(out.second, 56);
}

HOST_TEST(epoch_to_local_rolls_the_date_over_at_kyiv_winter_midnight) {
  TzGuard tz;
  // 2024-01-01 21:59:59 UTC -> 2024-01-01 23:59:59 local (just before).
  app_core::RtcDateTime before{};
  net_time::epoch_to_local(1704146399, before);
  EXPECT_EQ(before.year, 2024);
  EXPECT_EQ(before.month, 1);
  EXPECT_EQ(before.day, 1);
  EXPECT_EQ(before.hour, 23);
  EXPECT_EQ(before.minute, 59);
  EXPECT_EQ(before.second, 59);

  // 2024-01-01 22:00:00 UTC -> 2024-01-02 00:00:00 local: date rolls over.
  app_core::RtcDateTime after{};
  net_time::epoch_to_local(1704146400, after);
  EXPECT_EQ(after.year, 2024);
  EXPECT_EQ(after.month, 1);
  EXPECT_EQ(after.day, 2);
  EXPECT_EQ(after.hour, 0);
  EXPECT_EQ(after.minute, 0);
  EXPECT_EQ(after.second, 0);
}
