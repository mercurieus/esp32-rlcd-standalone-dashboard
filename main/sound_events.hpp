#pragma once

#include "app_snapshot.hpp"
#include "sound_policy.hpp"

// The board's event sounds (spec section 6): which melody each event plays,
// and the one place every one of them asks app_core::sound_allowed() first,
// with the live Volume preset, Quiet hours preset and clock trust. Lives in
// main/ because it is the only layer allowed to include modules/audio's
// audio.hpp (modules/README.md rule 4); with CONFIG_AUDIO_ENABLE=n every play
// below lands on the module's no-op stubs and nothing here needs an #ifdef.
//
// Nothing here blocks except play_farewell_blocking(), and that one never
// waits for anything but its own melody: every other sound is
// audio_play_notes_async(), refuse-if-busy. A sound requested while another
// plays is dropped, not queued.
namespace sound_events {

// The clock the board booted with, and whether it came from a valid RTC read
// (otherwise it is the compile-time fallback and is not trusted). Until SNTP
// syncs, quiet hours are judged against this clock advanced by uptime - the
// same clock the panel shows. Call before init(), after the RTC read.
void set_boot_clock(const app_core::RtcDateTime& clock, bool rtc_valid);

// Plays the boot melody when reset_reason (an esp_reset_reason_t) is a real
// start - ESP_RST_POWERON or ESP_RST_SW - and the policy allows it. Call once,
// after audio_init() and set_boot_clock().
void init(int reset_reason);

// The battery monitor's cable-step detector fired: PowerBack when the step was
// up (now_charging), PowerLost when it was down. step_mv is only for the log
// line - every power sound is logged with the step that caused it.
void on_cable_step(bool now_charging, int step_mv = 0);

// Every battery publish. Feeds app_core::BatteryAlarm; plays the low tone or
// the critical (farewell) melody on the reading that crosses the threshold.
void on_battery(const app_core::BatteryData& battery);

// Registered as ui::set_button_feedback_handler: one soft tick. Runs on the
// LVGL thread, so it only ever starts a sound, never waits for one.
void on_button();

// The policy verdict for an event right now, logged when it says no. For
// callers that play their own sound - apply_volume_preset_change() and its
// confirmation tone.
bool allowed(app_core::SoundEvent event);

// The falling farewell melody, played to completion on the calling task, for
// right before a firmware-initiated esp_restart(). Never waits for the audio
// module: if anything is already playing it returns at once, silent - a
// restart must not be held up by a sound. Gated by the policy like the rest.
void play_farewell_blocking();

}  // namespace sound_events
