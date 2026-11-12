#include "sound_events.hpp"

#include "audio.hpp"
#include "net_time.hpp"
#include "settings_menu.hpp"
#include "ui_data.hpp"

#include <atomic>
#include <cstdint>
#include <iterator>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace sound_events {
namespace {

constexpr char kTag[] = "sound";

using app_core::SoundEvent;

// The melodies. Spec section 6 leaves pitch and length to the builder within
// "short and soft": event tones under 400 ms, melodies under 1 s, 1-3 kHz
// (the band the audio README found audible on this speaker). Every pitch here
// is an equal-tempered note rounded to the hertz, so the arpeggios are in tune
// with each other rather than four arbitrary frequencies. No rests: each note
// already has its own few-millisecond fade (audio_play_notes()), which is
// enough articulation, and a rest would only lengthen the amplifier window.
//
// Boot: a C-major arpeggio rising C6 E6 G6 C7, the last note held - "awake".
constexpr audio::Note kBootMelody[] = {
    {1047, 90}, {1319, 90}, {1568, 90}, {2093, 240}};
static_assert(audio::notes_duration_ms(kBootMelody, std::size(kBootMelody)) <
                  1000,
              "boot melody must stay under 1 s");

// Farewell: the boot melody's mirror, falling C7 G6 E6 C6, the last note held.
// Also the battery-critical sound - spec: "the same falling melody" - because
// a board about to die of a flat cell is, from the listener's side, a board
// about to switch off.
constexpr audio::Note kFarewellMelody[] = {
    {2093, 90}, {1568, 90}, {1319, 90}, {1047, 240}};
static_assert(audio::notes_duration_ms(kFarewellMelody,
                                       std::size(kFarewellMelody)) < 1000,
              "farewell melody must stay under 1 s");

// Power lost: two falling notes, G6 then C6 - the tail of the farewell, so it
// reads as "going away" without being the full shutdown sound.
constexpr audio::Note kPowerLost[] = {{1568, 120}, {1047, 180}};
static_assert(audio::notes_duration_ms(kPowerLost, std::size(kPowerLost)) < 400,
              "power-lost tone must stay under 400 ms");

// Power back: one short note at the top of the arpeggio, C7 - the pair to
// power lost, ending high where that one ends low.
constexpr audio::Note kPowerBack[] = {{2093, 120}};
static_assert(audio::notes_duration_ms(kPowerBack, std::size(kPowerBack)) < 400,
              "power-back tone must stay under 400 ms");

// Battery low: one tone, low in the band and a little longer than the others
// so it is not mistaken for a button tick or the cable note. Plays once per
// discharge (app_core::BatteryAlarm), so it may be noticeable.
constexpr audio::Note kBatteryLow[] = {{1047, 250}};
static_assert(audio::notes_duration_ms(kBatteryLow, std::size(kBatteryLow)) <
                  400,
              "battery-low tone must stay under 400 ms");

// Button: the shortest note the audio module plays (50 ms - its kMinDurationMs;
// with the fades the audible part is shorter still), at E6. A tick, not a beep:
// it confirms the press and is over before the panel has finished repainting.
constexpr audio::Note kButtonTick[] = {{1319, 50}};
static_assert(audio::notes_duration_ms(kButtonTick, std::size(kButtonTick)) <
                  400,
              "button tick must stay under 400 ms");

// Written once from app_main before any task that plays a sound exists, read
// from the battery task and the LVGL thread afterwards. g_rtc_valid is the
// publish flag: atomic, stored last, so a reader that sees it true also sees
// the clock it describes.
app_core::RtcDateTime g_boot_clock{};
int64_t g_boot_clock_us = 0;
std::atomic<bool> g_rtc_valid{false};

// Only ever touched from battery_monitor_task (on_battery), so no lock.
app_core::BatteryAlarm g_battery_alarm;

const char* event_name(SoundEvent event) {
  switch (event) {
    case SoundEvent::Boot: return "boot";
    case SoundEvent::Farewell: return "farewell";
    case SoundEvent::PowerLost: return "power-lost";
    case SoundEvent::PowerBack: return "power-back";
    case SoundEvent::BatteryLow: return "battery-low";
    case SoundEvent::BatteryCritical: return "battery-critical";
    case SoundEvent::Button: return "button";
    case SoundEvent::VolumeConfirm: return "volume-confirm";
  }
  return "?";
}

app_core::ResetKind reset_kind(int reset_reason) {
  switch (static_cast<esp_reset_reason_t>(reset_reason)) {
    case ESP_RST_POWERON: return app_core::ResetKind::PowerOn;
    case ESP_RST_SW: return app_core::ResetKind::Software;
    // Panic, watchdogs, brownout, ESP_RST_USB (opening the serial port resets
    // this board), external pin, deep sleep: none of them is somebody
    // starting the clock on purpose.
    default: return app_core::ResetKind::Other;
  }
}

// The same three-way source string the panel's clock carries (see
// ui_app.cpp's update_visible_clock and net_time_monitor_task), so "trusted"
// here means exactly what the clock warning on screen means.
std::string clock_source() {
  if (net_time::synced()) return "SNTP";
  return g_rtc_valid.load() ? "PCF85063" : "RTC fallback";
}

// The local wall-clock hour by whichever clock is trusted. Once SNTP has
// synced, net_time::now() is local time (TZ is set by net_time::start()).
// Before that, the system clock knows nothing - the RTC is never written into
// it - so the hour comes from the RTC read at boot advanced by uptime, which is
// how the panel itself shows time until the first sync.
uint8_t local_hour() {
  app_core::RtcDateTime now{};
  if (net_time::now(now)) return now.hour;
  const int64_t elapsed_us = esp_timer_get_time() - g_boot_clock_us;
  const uint64_t elapsed_s =
      elapsed_us > 0 ? static_cast<uint64_t>(elapsed_us / 1'000'000) : 0;
  return app_core::advance_rtc_datetime(g_boot_clock, elapsed_s).hour;
}

// Starts a sound the policy has already allowed. Refused-if-busy is the
// audio module's contract and an expected outcome (a tick during a chime is
// simply lost), so it is a log line, not an error.
void play_async(SoundEvent event, const audio::Note* notes, size_t count) {
  const esp_err_t result = audio::audio_play_notes_async(notes, count);
  if (result != ESP_OK) {
    ESP_LOGI(kTag, "%s sound did not play: %s", event_name(event),
             esp_err_to_name(result));
  }
}

}  // namespace

bool allowed(SoundEvent event) {
  const bool volume_off = ui::volume_preset() == ui::VolumePreset::Off;
  const std::string source = clock_source();
  const bool trusted = ui::clock_is_set(source);
  // An untrusted clock counts as quiet anyway (sound_allowed() says so), and
  // its "hour" would be the compile time's - so it is not even asked.
  const bool quiet =
      trusted && ui::in_quiet_hours(ui::quiet_hours_preset(), local_hour());
  const bool verdict =
      app_core::sound_allowed(event, volume_off, quiet, trusted);
  if (!verdict) {
    // Every silenced sound says why, so "it didn't beep" has an answer in the
    // log. Button ticks would be one line per press; those go to debug.
    const esp_log_level_t level =
        event == SoundEvent::Button ? ESP_LOG_DEBUG : ESP_LOG_INFO;
    ESP_LOG_LEVEL_LOCAL(level, kTag,
                        "%s sound silenced (volume %s, quiet hours %s, clock %s)",
                        event_name(event), volume_off ? "off" : "on",
                        quiet ? "now" : "not now", source.c_str());
  }
  return verdict;
}

void set_boot_clock(const app_core::RtcDateTime& clock, bool rtc_valid) {
  g_boot_clock = clock;
  g_boot_clock_us = esp_timer_get_time();
  g_rtc_valid.store(rtc_valid);
}

void init(int reset_reason) {
  if (!app_core::boot_sound_for_reset(reset_kind(reset_reason))) {
    ESP_LOGI(kTag, "boot sound skipped: reset reason %d is not a real start",
             reset_reason);
    return;
  }
  if (!allowed(SoundEvent::Boot)) return;
  play_async(SoundEvent::Boot, kBootMelody, std::size(kBootMelody));
}

void on_cable_step(bool now_charging, int step_mv) {
  const SoundEvent event =
      now_charging ? SoundEvent::PowerBack : SoundEvent::PowerLost;
  // Logged before the policy check, so the step behind a silenced sound is in
  // the log too - spec: "Log every power sound with the step that caused it."
  ESP_LOGI(kTag, "%s: cable step %+d mV", event_name(event), step_mv);
  if (!allowed(event)) return;
  if (now_charging) {
    play_async(event, kPowerBack, std::size(kPowerBack));
  } else {
    play_async(event, kPowerLost, std::size(kPowerLost));
  }
}

void on_battery(const app_core::BatteryData& battery) {
  const auto event =
      g_battery_alarm.update(battery.valid, battery.charging, battery.percent);
  if (!event.has_value()) return;
  ESP_LOGI(kTag, "%s: battery at %u%% (%d mV), not charging",
           event_name(*event), static_cast<unsigned>(battery.percent),
           battery.millivolts);
  if (!allowed(*event)) return;
  if (*event == SoundEvent::BatteryCritical) {
    play_async(*event, kFarewellMelody, std::size(kFarewellMelody));
  } else {
    play_async(*event, kBatteryLow, std::size(kBatteryLow));
  }
}

void on_button() {
  if (!allowed(SoundEvent::Button)) return;
  // Not play_async(): a refused tick (a chime is still playing, or the
  // operator pressed twice inside one tick's window) is routine and would
  // otherwise log on every quick double press.
  (void)audio::audio_play_notes_async(kButtonTick, std::size(kButtonTick));
}

void play_farewell_blocking() {
  if (!allowed(SoundEvent::Farewell)) return;
  const esp_err_t result = audio::audio_try_play_notes_blocking(
      kFarewellMelody, std::size(kFarewellMelody));
  if (result != ESP_OK) {
    ESP_LOGI(kTag, "farewell sound did not play: %s", esp_err_to_name(result));
  }
}

}  // namespace sound_events
