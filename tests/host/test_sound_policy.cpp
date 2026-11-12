#include "restart_hook.hpp"
#include "sound_policy.hpp"

#include "test_support.hpp"

#include <optional>

// Spec section 6, "Rules": which event may make a sound, and when. The
// playing itself is main/sound_events.cpp (ESP-only); what is decided here is
// everything a wrong answer would turn into a sound at night or a nag.

namespace {

using app_core::BatteryAlarm;
using app_core::ResetKind;
using app_core::SoundEvent;
using app_core::boot_sound_for_reset;
using app_core::sound_allowed;

constexpr SoundEvent kAllEvents[] = {
    SoundEvent::Boot,       SoundEvent::Farewell,   SoundEvent::PowerLost,
    SoundEvent::PowerBack,  SoundEvent::BatteryLow, SoundEvent::BatteryCritical,
    SoundEvent::Button,     SoundEvent::VolumeConfirm,
};

bool fired(const std::optional<SoundEvent>& got, SoundEvent want) {
  return got.has_value() && *got == want;
}

}  // namespace

HOST_TEST(volume_off_silences_everything) {
  for (SoundEvent event : kAllEvents) {
    // Daytime, trusted clock: Off still wins, the confirmation tone included.
    EXPECT_TRUE(!sound_allowed(event, /*volume_off=*/true, /*quiet_now=*/false,
                               /*clock_trusted=*/true));
    EXPECT_TRUE(!sound_allowed(event, true, true, false));
  }
}

HOST_TEST(quiet_hours_silence_everything_but_the_volume_confirm) {
  for (SoundEvent event : kAllEvents) {
    const bool allowed = sound_allowed(event, false, /*quiet_now=*/true, true);
    EXPECT_EQ(allowed, event == SoundEvent::VolumeConfirm);
    // And outside quiet hours everything plays.
    EXPECT_TRUE(sound_allowed(event, false, false, true));
  }
}

HOST_TEST(an_untrusted_clock_counts_as_quiet) {
  for (SoundEvent event : kAllEvents) {
    const bool allowed =
        sound_allowed(event, false, /*quiet_now=*/false, /*clock_trusted=*/false);
    EXPECT_EQ(allowed, event == SoundEvent::VolumeConfirm);
  }
}

HOST_TEST(boot_sound_only_on_power_on_and_software_restart) {
  EXPECT_TRUE(boot_sound_for_reset(ResetKind::PowerOn));
  EXPECT_TRUE(boot_sound_for_reset(ResetKind::Software));
  EXPECT_TRUE(!boot_sound_for_reset(ResetKind::Other));
}

HOST_TEST(battery_low_fires_once_per_discharge) {
  BatteryAlarm alarm;
  EXPECT_TRUE(!alarm.update(true, false, 40).has_value());  // arms
  EXPECT_TRUE(!alarm.update(true, false, 16).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 15).has_value());  // 15 is not below
  EXPECT_TRUE(fired(alarm.update(true, false, 14), SoundEvent::BatteryLow));
  // Readings wobble around the threshold; the 20% re-arm is what keeps
  // 14/15/14 from beeping twice.
  EXPECT_TRUE(!alarm.update(true, false, 14).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 16).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 14).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 5).has_value());
  EXPECT_TRUE(fired(alarm.update(true, false, 2), SoundEvent::BatteryCritical));
  EXPECT_TRUE(!alarm.update(true, false, 2).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 3).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 1).has_value());
}

HOST_TEST(battery_alarm_critical_outranks_low_on_a_big_drop) {
  // One publish can skip straight past both thresholds (e.g. a sag under
  // load). The board is about to die: the critical sound, not the low one,
  // and no low one afterwards either.
  BatteryAlarm alarm;
  EXPECT_TRUE(!alarm.update(true, false, 30).has_value());
  EXPECT_TRUE(fired(alarm.update(true, false, 2), SoundEvent::BatteryCritical));
  EXPECT_TRUE(!alarm.update(true, false, 4).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 2).has_value());
}

HOST_TEST(battery_alarm_rearms_after_charging_or_recovery) {
  BatteryAlarm alarm;
  EXPECT_TRUE(!alarm.update(true, false, 50).has_value());
  EXPECT_TRUE(fired(alarm.update(true, false, 14), SoundEvent::BatteryLow));
  // Recovery to 19 is not enough...
  EXPECT_TRUE(!alarm.update(true, false, 19).has_value());
  EXPECT_TRUE(!alarm.update(true, false, 14).has_value());
  // ...20 is.
  EXPECT_TRUE(!alarm.update(true, false, 20).has_value());
  EXPECT_TRUE(fired(alarm.update(true, false, 14), SoundEvent::BatteryLow));

  // On the charger nothing fires, however low it reads, and it re-arms.
  EXPECT_TRUE(!alarm.update(true, true, 13).has_value());
  EXPECT_TRUE(!alarm.update(true, true, 2).has_value());
  EXPECT_TRUE(fired(alarm.update(true, false, 12), SoundEvent::BatteryLow));
  EXPECT_TRUE(fired(alarm.update(true, false, 2), SoundEvent::BatteryCritical));
  EXPECT_TRUE(!alarm.update(true, true, 2).has_value());
  EXPECT_TRUE(fired(alarm.update(true, false, 2), SoundEvent::BatteryCritical));
}

HOST_TEST(battery_alarm_rearm_gives_back_a_refused_alarm) {
  BatteryAlarm alarm;
  EXPECT_TRUE(!alarm.update(true, false, 50).has_value());
  EXPECT_TRUE(fired(alarm.update(true, false, 14), SoundEvent::BatteryLow));
  EXPECT_TRUE(!alarm.update(true, false, 14).has_value());  // used up
  alarm.rearm(SoundEvent::BatteryLow);                      // play was busy
  EXPECT_TRUE(fired(alarm.update(true, false, 14), SoundEvent::BatteryLow));
  EXPECT_TRUE(!alarm.update(true, false, 14).has_value());

  // Critical spent the low alarm too; rearming critical gives both back.
  EXPECT_TRUE(fired(alarm.update(true, false, 2), SoundEvent::BatteryCritical));
  EXPECT_TRUE(!alarm.update(true, false, 2).has_value());
  alarm.rearm(SoundEvent::BatteryCritical);
  EXPECT_TRUE(fired(alarm.update(true, false, 2), SoundEvent::BatteryCritical));
  // Without a rearm (a policy suppression) it stays used up.
  EXPECT_TRUE(!alarm.update(true, false, 2).has_value());
}

HOST_TEST(battery_alarm_is_silent_without_a_battery) {
  BatteryAlarm alarm;
  EXPECT_TRUE(!alarm.update(true, false, 50).has_value());
  // No cell: the percentage is meaningless, whatever it says.
  EXPECT_TRUE(!alarm.update(false, false, 1).has_value());
  EXPECT_TRUE(!alarm.update(false, false, 0).has_value());
  // And a board that has never seen a battery stays quiet too.
  BatteryAlarm never;
  EXPECT_TRUE(!never.update(false, false, 1).has_value());
  EXPECT_TRUE(!never.update(false, false, 1).has_value());
}

HOST_TEST(battery_alarm_does_not_fire_on_the_first_reading_after_boot) {
  // A board that boots already at 10% has not "crossed" anything - and a
  // crash loop at 10% must not beep every cycle.
  BatteryAlarm low;
  EXPECT_TRUE(!low.update(true, false, 10).has_value());
  EXPECT_TRUE(!low.update(true, false, 9).has_value());
  // Critical still fires when it is crossed later on this boot.
  EXPECT_TRUE(fired(low.update(true, false, 2), SoundEvent::BatteryCritical));

  BatteryAlarm critical;
  EXPECT_TRUE(!critical.update(true, false, 2).has_value());
  EXPECT_TRUE(!critical.update(true, false, 1).has_value());

  // Invalid readings before the first valid one do not count as "first".
  BatteryAlarm late;
  EXPECT_TRUE(!late.update(false, false, 0).has_value());
  EXPECT_TRUE(!late.update(true, false, 10).has_value());
  EXPECT_TRUE(!late.update(true, false, 9).has_value());
}

// The restart hook (spec section 6, "Farewell"): every firmware-initiated
// esp_restart() runs app_core::run_before_restart() first, and that is where
// the farewell melody is registered. What is decided here is only the hook's
// own contract - once, and harmless when nothing is registered.
namespace {
int g_hook_calls = 0;
void counting_hook() { ++g_hook_calls; }
}  // namespace

HOST_TEST(restart_hook_runs_once) {
  g_hook_calls = 0;
  app_core::set_before_restart_hook(&counting_hook);
  app_core::run_before_restart();
  EXPECT_EQ(g_hook_calls, 1);
  // The OTA task and the portal's timer can both reach a restart; the second
  // must not play the melody again.
  app_core::run_before_restart();
  EXPECT_EQ(g_hook_calls, 1);
}

HOST_TEST(restart_hook_without_registration_is_harmless) {
  app_core::set_before_restart_hook(nullptr);
  app_core::run_before_restart();
  app_core::run_before_restart();
  EXPECT_TRUE(true);
}
