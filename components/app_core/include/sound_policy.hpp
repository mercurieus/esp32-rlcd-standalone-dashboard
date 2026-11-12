#pragma once

#include <cstdint>
#include <optional>

// Which locally generated sound may play, and when - spec section 6, "Rules".
// Header-only and free of ESP-IDF, LVGL and modules/audio on purpose: this is
// the part where a wrong answer becomes a chime at 3 am or a beep that nags,
// so it has to be host-tested, and app_core must not reach into modules/
// (modules/README.md rule 4). main/sound_events.cpp owns the melodies and
// calls modules/audio; it asks this header first, every time.
namespace app_core {

enum class SoundEvent : uint8_t {
  Boot,             // a real start: power-on or a firmware-initiated restart
  Farewell,         // immediately before a firmware-initiated restart
  PowerLost,        // the cable-step detector saw the cable come out
  PowerBack,        // ...and go back in
  BatteryLow,       // crossed below kBatteryLowPercent, not charging
  BatteryCritical,  // crossed below kBatteryCriticalPercent - about to die
  Button,           // a press the UI accepted
  VolumeConfirm,    // the Volume row's own confirmation tone
};

// volume_off: the Volume preset is Off. quiet_now: the local hour is inside
// the Quiet hours preset. clock_trusted: the clock source is SNTP or a valid
// RTC read (ui::clock_is_set()).
//
// Volume Off silences everything, the confirmation tone included - Off means
// off. The confirmation tone alone ignores quiet hours and trust: it is the
// operator, holding the board, asking to hear the level they just picked.
// Everything else needs a trusted clock outside quiet hours: a board that
// cannot know it is daytime plays nothing rather than risk the night.
constexpr bool sound_allowed(SoundEvent event, bool volume_off, bool quiet_now,
                             bool clock_trusted) {
  if (volume_off) return false;
  if (event == SoundEvent::VolumeConfirm) return true;
  return !quiet_now && clock_trusted;
}

// Why the chip last reset, reduced to the one question the boot melody asks.
// main/sound_events.cpp maps esp_reset_reason() onto this so the header
// carries no mirrored ESP_RST_* numbers: ESP_RST_POWERON -> PowerOn,
// ESP_RST_SW -> Software, everything else -> Other.
enum class ResetKind : uint8_t { PowerOn, Software, Other };

// Power-on (the PWR button, or a cable into a dead board) and the firmware's
// own restarts (OTA, /restart, a settings reboot) are real starts. Panic,
// watchdog and brownout are not - a crash loop must not chime every cycle -
// and neither is ESP_RST_USB: opening this board's serial port resets it.
constexpr bool boot_sound_for_reset(ResetKind kind) {
  return kind == ResetKind::PowerOn || kind == ResetKind::Software;
}

// 15% is the builder's call from spec section 6: low enough not to nag on an
// ordinary afternoon off the charger, high enough to leave hours of standby.
// 3% is "about to die": battery_percent()'s curve is near its knee there and
// the cell is minutes, not hours, from the brownout. 20% is the hysteresis
// band - percent readings wobble a point or two between publishes, and
// re-arming at the threshold itself would beep on every wobble across it.
inline constexpr uint8_t kBatteryLowPercent = 15;
inline constexpr uint8_t kBatteryCriticalPercent = 3;
inline constexpr uint8_t kBatteryRearmPercent = 20;

// Edge detector over the published battery level: each alarm fires once when
// the level crosses below its threshold while not charging, then stays silent
// until it is re-armed by the level recovering to kBatteryRearmPercent or by
// a charger reading. Critical outranks low on the same reading (a publish
// that drops straight past both plays the critical sound only) and spends
// the low alarm with it.
//
// The first valid reading after boot only arms: a board booting at 10% has
// not crossed anything, and a crash loop at 10% must not beep every cycle.
// An invalid reading (no cell, or a divider reading nobody trusts) changes
// nothing and never fires.
class BatteryAlarm {
 public:
  std::optional<SoundEvent> update(bool valid, bool charging, uint8_t percent) {
    if (!valid) return std::nullopt;
    if (!seen_valid_) {
      seen_valid_ = true;
      low_armed_ = charging || percent >= kBatteryLowPercent;
      critical_armed_ = charging || percent >= kBatteryCriticalPercent;
      return std::nullopt;
    }
    if (charging || percent >= kBatteryRearmPercent) {
      low_armed_ = true;
      critical_armed_ = true;
    }
    if (charging) return std::nullopt;
    if (percent < kBatteryCriticalPercent && critical_armed_) {
      critical_armed_ = false;
      low_armed_ = false;
      return SoundEvent::BatteryCritical;
    }
    if (percent < kBatteryLowPercent && low_armed_) {
      low_armed_ = false;
      return SoundEvent::BatteryLow;
    }
    return std::nullopt;
  }

 private:
  bool seen_valid_ = false;
  bool low_armed_ = false;
  bool critical_armed_ = false;
};

}  // namespace app_core
