# Faster Time, Trust Line, Legible Values, Wider Chart and Sounds - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Network time on the panel within seconds of boot and on the real minute, an always-on clock-trust line on Home, a three-state Wi-Fi icon, readings at 20 px, a wider indoor plot, and soft once-only sounds gated by an on-device quiet-hours setting.

**Architecture:** Every decision that can be pure (minute timing, trust-line text, icon state, quiet-hours window, sound policy, layout geometry) goes into a header-only or host-compiled function with a host test; ESP-IDF/LVGL code only wires those functions to tasks, widgets and the codec. Module rule from `modules/README.md` holds: no core component includes `audio.hpp`; core exposes registration hooks and `main/` connects them to the audio module.

**Tech Stack:** C++17, ESP-IDF 5.5.x (built locally with the eim-managed v5.5.5), LVGL 9.3, host tests via the project's own `HOST_TEST` harness in `tests/host`.

**Spec:** `docs/superpowers/specs/2026-11-13-sync-legibility-sounds-design.md` - read it first; this plan argues from it and does not repeat its reasoning.

## Global Constraints

- Nothing reaches the panel as a number unless it was measured or fetched. Stale is shown with its marker, not hidden.
- Every text box is sized from the compiled font's glyph table for the **widest value the slot can hold**, in both languages (English, Traditional Chinese). Use `scripts/measure-text.py` (Task 0). Placeholder/mock text is the worst case.
- 1-bit panel: no grey text, strokes >= 2 px, never dither behind text.
- Fonts available: `font_small()` Montserrat 14 + CJK 14 (line height 16), `font_medium()` Montserrat 20 + CJK 20 (line height 22). No new font sizes.
- Every new Traditional Chinese string must pass `python scripts/check-cjk-font.py`; if it fails, regenerate the subset with `scripts/build-fonts.sh`.
- Modules: no `components/*` file includes `modules/*/include/*`. With `CONFIG_AUDIO_ENABLE=n` the build still links and every sound call is a no-op stub.
- Amplifier (GPIO46) high only inside a tone's play window.
- Sounds: each plays once per event, never loops; event tones < 400 ms, melodies < 1000 ms; 1-3 kHz; at the current Volume preset.
- Volume `Off` silences every sound. Quiet hours silence every sound except the Volume row's confirmation tone. An untrusted clock (source neither `"SNTP"` nor `"PCF85063"`) counts as quiet hours.
- Quiet-hours presets: `Off`, `22-07`, `23-07`, `00-07`; default `23-07`; NVS key `quiet_preset` in namespace `ui_prefs`.
- Trust line: `< 60 min` -> minutes ago; `>= 60 min` -> sync time of day; `>= 24 h` -> old-sync warning.
- NTP servers: `pool.ntp.org`, `ua.pool.ntp.org`; `CONFIG_LWIP_SNTP_MAX_SERVERS=2`.
- `sdkconfig.defaults` only seeds a fresh `sdkconfig` (and `sdkconfig` is git-ignored): set the option in both, then `grep LWIP_SNTP_MAX_SERVERS sdkconfig` must print `=2`.
- **Two RLCD boards exist on this machine's history.** This project's board is MAC `a4:cb:8f:df:88:d0`; the weather-clock board on COM8 is a different unit. Confirm the port by `python -m esptool --chip esp32s3 -p COMn --after no_reset read_mac` before any flash; never flash a port chosen by inference. Use `app-flash`, never `erase-flash`.
- Firmware build on this Windows machine (PowerShell): `. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1; idf.py build`.
- Stage commits by explicit path; never `git add -A`.

## Review Focus

1. **Clock jumps at first sync** (RTC hours wrong, or set backwards): "minutes since last sync" must never go negative or huge - clamp; a sync that just landed reads `SYNCED JUST NOW`. Test in Task 2.
2. **Quiet hours across midnight and on the boundary hour**: `23-07` is quiet at 23:00 and 06:59, loud at 07:00 and 22:59; `00-07` does not wrap. Test in Task 8.
3. **No battery / invalid reading**: no power, low or critical sound when `battery.valid` is false or the reading is the "no battery" case; low/critical fire once per discharge with hysteresis. Test in Task 9.
4. **Restart while audio is busy** (AirPlay stream open, or a tone already playing): the farewell is skipped, not waited on forever; the restart still happens within its existing timing + 1 s. Test in Task 10.
5. **Traditional Chinese**: every new string has glyphs and fits its box at the widest value. Checked in Tasks 2, 3, 8 via `check-cjk-font.py` and `measure-text.py`.

---

### Task 0: Host toolchain on Windows and a text-measuring script

**Files:**
- Modify: `tests/host/CMakeLists.txt` (cJSON lookup)
- Create: `scripts/measure-text.py`

**Interfaces:**
- Produces: `python scripts/measure-text.py --font montserrat_14|montserrat_20 "TEXT" ["TEXT" ...]` printing `<px with one decimal>  <text>` per argument, from `adv_w/16` of each glyph in `managed_components/lvgl__lvgl/src/font/lv_font_montserrat_<N>.c` (resolve codepoints through `cmaps[]`, including sparse `unicode_list` offsets from `range_start`; no kerning). Unknown glyph -> non-zero exit naming it. `--cjk 14|20` measures with `components/ui/fonts/rlcd_cjk_<N>.c` as fallback for codepoints Montserrat lacks.

- [ ] **Step 1: Ask the operator before installing anything.** No host C/C++ compiler is installed (no `cl`, `g++`, `clang++` on PATH). Propose `winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT` (MinGW-w64 GCC) and use CMake from `C:\Espressif\tools\cmake\4.0.3\bin`. Wait for approval.
- [ ] **Step 2: Make cJSON resolvable here.** In `tests/host/CMakeLists.txt`, accept a pre-set `CJSON_DIR` cache variable before the `.tools/esp-idf-v5.5.2` lookup (only fail if neither exists). Local value: `C:/esp/v5.5.5/esp-idf/components/json/cJSON`.
- [ ] **Step 3: Build and run the existing suite unchanged.**
  Run: `cmake -S tests/host -B build-host -G Ninja -DCJSON_DIR=C:/esp/v5.5.5/esp-idf/components/json/cJSON && cmake --build build-host && ./build-host/host_tests`
  Expected: last line `N cases, 0 failures`. Record N. If anything fails before any change, stop and report - do not fix unrelated code.
- [ ] **Step 4: Write `scripts/measure-text.py`** to the interface above.
- [ ] **Step 5: Check it against numbers already in the code comments.**
  Run: `python scripts/measure-text.py --font montserrat_14 "-24.4" "100" "FEELS -24.1°C RAIN 100%"` and `--font montserrat_20 "-10.4°C" "100%"`
  Expected (+/-0.5 px): `35.4`, `23.8`, `173` (spec), `69.1`, `50.9`.
- [ ] **Step 6: Commit** `tests/host/CMakeLists.txt scripts/measure-text.py` - `build(host): run the host suite on Windows, and measure text from the font tables`.

---

### Task 1: Network time reaches the panel at once, and minutes flip on the real minute

**Files:**
- Modify: `components/net_time/include/net_time.hpp`, `components/net_time/net_time.cpp`
- Modify: `components/net_time/include/net_time_convert.hpp`, `components/net_time/net_time_convert.cpp`
- Modify: `main/app_main.cpp` (`net_time_monitor_task`, `kNetTimeCheckPeriodMs`)
- Modify: `components/ui/include/ui_data.hpp`, `components/ui/ui_app.cpp` (`update_clock`)
- Modify: `sdkconfig.defaults` (+ local `sdkconfig`)
- Test: `tests/host/test_net_time.cpp`, `tests/host/test_ui_data.cpp`

**Interfaces:**
- Produces:
  - `uint32_t net_time::ms_until_next_minute(int64_t epoch_ms)` - ms to the next `:00` plus `kMinuteMarginMs = 50`; never 0, never more than `60'050`.
  - `void net_time::set_sync_observer(void (*observer)())` - called from `on_sync` on **every** sync (not only the first).
  - `int64_t net_time::last_sync_epoch()` - Unix seconds of the most recent sync, `0` before the first.
  - `uint64_t ui::clock_minute_index(uint64_t elapsed_ms, uint8_t start_second)` - `(elapsed_ms + start_second * 1000) / 60000`.

- [ ] **Step 1: Failing tests.**
```cpp
// 1'699'999'980'000 is an exact minute boundary (60000 * 28'333'333).
constexpr int64_t kBoundaryMs = 1'699'999'980'000;
HOST_TEST(ms_until_next_minute_counts_to_the_next_boundary_plus_margin) {
  EXPECT_EQ(net_time::ms_until_next_minute(kBoundaryMs + 40'000), 20'050u);
  EXPECT_EQ(net_time::ms_until_next_minute(kBoundaryMs + 59'999), 51u);
}
HOST_TEST(ms_until_next_minute_on_an_exact_boundary_waits_a_whole_minute) {
  EXPECT_EQ(net_time::ms_until_next_minute(kBoundaryMs), 60'050u);
}
HOST_TEST(clock_minute_index_flips_when_the_rtc_minute_does) {
  EXPECT_EQ(ui::clock_minute_index(14'999, 45), 0u);   // 59.999 s on the RTC
  EXPECT_EQ(ui::clock_minute_index(15'000, 45), 1u);   // :00 on the RTC
}
```
(Write the boundary epoch as a literal that is a multiple of 60000.)
- [ ] **Step 2: Run** host tests. Expected: the three new cases FAIL (undefined).
- [ ] **Step 3: Implement** `ms_until_next_minute` in `net_time_convert.cpp` and `clock_minute_index` inline in `ui_data.hpp`.
- [ ] **Step 4: Run** host tests. Expected: all pass.
- [ ] **Step 5: Wire it.**
  - `net_time.cpp`: store `last_sync_epoch` (atomic) and call the observer in `on_sync` every time; keep the first-sync log line as is. Use `ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "ua.pool.ntp.org"))`.
  - `app_main.cpp`: monitor task keeps its own `TaskHandle_t`; the observer does `xTaskNotifyGive(handle)`. The loop waits `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(synced ? ms_until_next_minute(now_ms) : 60'000))`. Replace `since_rtc_write_ms` accumulation with elapsed real time (the wait is no longer fixed). Delete `kNetTimeCheckPeriodMs`.
  - `ui_app.cpp` `update_clock`: compute the minute with `clock_minute_index(now_ms - started_ms, initial_clock.second)`.
  - `CONFIG_LWIP_SNTP_MAX_SERVERS=2` in `sdkconfig.defaults` and local `sdkconfig`.
- [ ] **Step 6: Build firmware.** Expected: success; `grep LWIP_SNTP_MAX_SERVERS sdkconfig` -> `=2`.
- [ ] **Step 7: Commit** - `fix(net_time): publish the first sync at once and every minute on the minute`.

---

### Task 2: Home always says whether the clock can be trusted

**Files:**
- Modify: `components/app_core/include/app_snapshot.hpp` (`ClockData`)
- Modify: `main/app_main.cpp` (`format_clock` call site in the monitor task)
- Modify: `components/ui/include/ui_data.hpp` (replace use of `clock_warning_text` on Home)
- Modify: `components/ui/include/ui_strings.hpp`, `components/ui/ui_strings.cpp`
- Modify: `components/ui/render_home.cpp`, `components/ui/ui_app.cpp` (re-render the line each minute)
- Test: `tests/host/test_ui_data.cpp`

**Interfaces:**
- Consumes: `net_time::last_sync_epoch()` (Task 1).
- Produces:
  - `ClockData::last_sync_epoch` (`int64_t`, `0` = never).
  - `std::string ui::clock_trust_line(const app_core::ClockData& clock, int64_t now_epoch)` - in `ui_data.hpp`, built from `Text` entries (so it follows the language).
  - New `Text` entries: `ClockSyncedJustNow`, `ClockSyncedMinutesAgo` (format with one `%u`), `ClockSyncedAt` (format `%02u:%02u`), `ClockSyncOld` (format `%02u:%02u`). EN copy: `SYNCED JUST NOW`, `SYNCED %u MIN AGO`, `SYNCED %02u:%02u`, `SYNC OLD %02u:%02u`.

- [ ] **Step 1: Failing tests** (English; set the language explicitly; use a fixed TZ like `test_net_time.cpp` does):
```cpp
HOST_TEST(trust_line_reads_just_now_for_a_sync_under_a_minute_old)       // now - last = 30 -> "SYNCED JUST NOW"
HOST_TEST(trust_line_counts_minutes_until_an_hour)                       // 3*60 -> "SYNCED 3 MIN AGO"; 59*60+59 -> "SYNCED 59 MIN AGO"
HOST_TEST(trust_line_shows_the_local_sync_time_from_an_hour)             // 60*60 -> "SYNCED hh:mm" of last_sync in local time
HOST_TEST(trust_line_warns_once_the_sync_is_a_day_old)                   // 24*3600 -> "SYNC OLD hh:mm"
HOST_TEST(trust_line_treats_a_sync_in_the_future_as_just_now)            // last > now (clock jumped back) -> "SYNCED JUST NOW"
HOST_TEST(trust_line_keeps_the_rtc_and_unsynced_warnings)                // source "PCF85063" -> text(ClockFromRtc); "" -> text(ClockNotSynced)
```
- [ ] **Step 2: Run.** Expected: FAIL (undefined).
- [ ] **Step 3: Implement** `clock_trust_line`, the field, and EN + TC strings (TC wording: follow the existing `ClockFromRtc` translation's style).
- [ ] **Step 4: Run.** Expected: pass.
- [ ] **Step 5: Wire.** Monitor task sets `data.last_sync_epoch = net_time::last_sync_epoch()`. `render_home.cpp` always draws `clock_trust_line(snapshot.clock, now)` in `layout.sync` with `row_font()`; the UI's per-minute path refreshes it (no extra repaints between minutes).
- [ ] **Step 6: Measure and check glyphs.** `measure-text.py --font montserrat_20` on `SYNCED 59 MIN AGO`, `SYNC OLD 23:59` and the widest of the existing warnings, plus the TC strings with `--cjk 20`; all must fit `layout.sync` width minus `2 * kTextInset`. `python scripts/check-cjk-font.py` passes (regenerate fonts if not).
- [ ] **Step 7: Build firmware.** Expected: success.
- [ ] **Step 8: Commit** - `feat(home): always say whether the clock can be trusted`.

---

### Task 3: Three-state Wi-Fi icon

**Files:**
- Modify: `components/app_core/include/app_snapshot.hpp` (`SetupData`)
- Modify: `components/wifi_provision/wifi_provision.cpp` (`apply_state_and_publish`)
- Modify: `components/ui/include/ui_data.hpp`, `components/ui/include/ui_theme.hpp`, `components/ui/ui_theme.cpp`, `components/ui/render_shared.cpp`, `components/ui/ui_app.cpp`, `components/ui/include/ui_app.hpp` (`UiContext`)
- Test: `tests/host/test_ui_data.cpp`

**Interfaces:**
- Produces:
  - `SetupData::connecting` (`bool`) - true iff the state machine is `Connecting`.
  - `enum class ui::WifiIconState : uint8_t { Connected, Connecting, Off };`
  - `ui::WifiIconState ui::wifi_icon_state(const app_core::SetupData&)`.
  - `bool ui::wifi_icon_visible(ui::WifiIconState, uint64_t now_ms)` - same phase as `clock_blink_visible` (`kClockBlinkHalfPeriodMs`); always true unless `Connecting`.
  - `void set_wifi_icon_state(const WifiIconParts&, WifiIconState, bool blink_visible)` replaces the `bool connected` overload.

- [ ] **Step 1: Failing tests:** `wifi_icon_is_solid_when_connected`, `wifi_icon_blinks_while_connecting` (visible at 0 ms, hidden at 500 ms, visible at 1000 ms), `wifi_icon_is_slashed_in_setup_or_unconfigured` (`active=true` -> `Off`; all false -> `Off`), `connected_wins_over_a_stale_connecting_flag`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** the pure functions and the snapshot field.
- [ ] **Step 4: Run.** Expected: pass.
- [ ] **Step 5: Wire.** `Connecting` shows `wifi-high` toggled by opacity (as `set_wifi_icon_state` already does), `Off` shows `wifi-slash`. A per-tick `update_wifi_icon_blink(UiContext&, const AppSnapshot&, uint64_t now_ms)` beside `update_clock_blink` touches the widget only on a phase change. Home has no tray: unaffected.
- [ ] **Step 6: Build firmware.** Expected: success.
- [ ] **Step 7: Commit** - `feat(tray): the Wi-Fi icon blinks while it is connecting`.

---

### Task 4: Tile detail readings at 20 px

**Files:**
- Modify: `components/ui/include/ui_theme.hpp` (`kTileDetailHeight`, `kTileContentHeight`, `tile_text_layout`)
- Modify: `components/ui/include/ui_data.hpp` (detail parts + fit rule)
- Modify: `components/ui/render_shared.cpp` (tile detail rendering for Battery, Weather, Indoor, Market)
- Test: `tests/host/test_ui_theme.cpp`, `tests/host/test_ui_data.cpp`

**Interfaces:**
- Produces:
  - `struct ui::DetailParts { std::string prefix; std::string caption; std::string value; std::string suffix; };` - prefix = `ALERT  ` or empty, caption = 14 px words (condition, `RH`), value = 20 px reading (`~-24°C`, `55%`, `+0.12%`), suffix = ` OLD` or empty.
  - `enum class ui::CaptionFit : uint8_t { Full, Ellipsised, Dropped };`
  - `ui::CaptionFit ui::fit_detail_caption(int prefix_px, int caption_px, int value_px, int suffix_px, int box_px, int min_caption_px)` - `Full` if all fit; else `Ellipsised` if `box - prefix - value - suffix >= min_caption_px`; else `Dropped`. Value, prefix and suffix are never reduced.

- [ ] **Step 1: Failing tests:** `caption_is_kept_when_everything_fits` (172 of 176 -> `Full`), `caption_gives_way_to_old_marker` (widths summing to ~205 -> `Ellipsised`), `caption_is_dropped_before_the_value_with_alert_and_old` (~260 -> `Dropped`), and a geometry case `tile_detail_row_fits_a_20px_reading` asserting `tile_text_layout(cell).detail.height >= safe_text_box_height(18, kSetupMediumFontLineHeight)` and that the row stays within the tile cell and below `tile_leading_visual_rect(cell, true)`.
  Use real widths from `measure-text.py` in the tests' comments.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Grow `kTileDetailHeight` to `safe_text_box_height(18, 22)` and `kTileContentHeight` by the same amount, taking height from below the tile (Home tile row / page-dot gap), never from `kTileValueHeight` or `kTileDetailDrop` (those size the icon box). If `home_layout_fits` or any existing geometry `static_assert` fails, stop and report the numbers - do not shrink the icon.
- [ ] **Step 4: Run.** Expected: pass, including all pre-existing tile/home geometry tests.
- [ ] **Step 5: Wire** `render_shared.cpp`: draw caption (14) and value (20) as two labels on one baseline (14 px box at `y + 5` meets a 20 px box at `y`), centred as a group; measure with `lv_text_get_size` at runtime and apply `fit_detail_caption`. Battery tile word (`OVERVOLTAGE`) stays 14 on the same baseline. Mock/placeholder text: `ALERT  Snow Showers ~-24°C OLD`.
- [ ] **Step 6: Build firmware.** Expected: success.
- [ ] **Step 7: Commit** - `feat(home): tile readings at 20px, and the condition word gives way first`.

---

### Task 5: Weather page qualifier row at 20 px

**Files:**
- Modify: `components/ui/render_weather.cpp` (current-conditions rows)
- Modify: `components/ui/include/ui_data.hpp` (`weather_forecast_rect` / header geometry if it moves)
- Test: `tests/host/test_ui_data.cpp`

**Interfaces:**
- Consumes: `ui::DetailParts`-style splitting is not reused; this row has two pairs (`FEELS` + feels value, `RAIN` + percent).

- [ ] **Step 1: Measure** with `measure-text.py` the worst cases: captions `FEELS`, `RAIN` at 14; values `-24.1°C`, `100%` at 20; the ` OLD` suffix; city `Chernivtsi 02:02` at 14.
- [ ] **Step 2: Choose the first arrangement that fits every worst case**, in this order, and write the measured numbers into the code comment that replaces the current one at the `current_line` label:
  1. One line, captions 14 / values 20, right-aligned in the existing box (`x + 198`, width `bounds.width - 202`).
  2. Same, with the feels-like value at whole degrees (`temperature_text(..., 0)`).
  3. Two stacked pairs (caption above value), moving the divider (`y + 70`) down by the minimum needed; the forecast's seven rows must still pass their existing geometry checks.
  If none fits, stop and report the measurements.
- [ ] **Step 3: Failing test** for the geometry chosen: `weather_qualifier_row_fits_its_widest_reading` asserting the chosen boxes' widths >= measured worst case + `2 * kTextInset` and that nothing overlaps the city label or the forecast rect.
- [ ] **Step 4: Run.** Expected: FAIL. **Step 5: Implement. Step 6: Run.** Expected: pass.
- [ ] **Step 7: Build firmware.** Expected: success.
- [ ] **Step 8: Commit** - `feat(weather): feels-like and rain chance at 20px`.

---

### Task 6: Wider indoor plot

**Files:**
- Modify: `components/ui/include/ui_data.hpp` (`indoor_layout`, its `static_assert`s)
- Test: `tests/host/test_ui_data.cpp`

- [ ] **Step 1: Failing test** `indoor_plot_reclaims_the_page_inset`: for the content bounds the page really uses, `plot.width == content.width - kIndoorTemperatureScaleWidth - kIndoorHumidityScaleWidth` and the temperature gutter starts at `content.x` (no 8 px inset), the humidity gutter ends at `content.right()`; everything stays `within_safe_canvas`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement**: the plot, gutters and hour axis use `bounds.x` / `bounds.width` directly; the title and header row keep `inset = 8`. Gutter widths stay sized for `-24.4` and `100` (do not shrink them).
- [ ] **Step 4: Run.** Expected: pass, and the existing gutter-meets-plot `static_assert`s still hold (update their expressions, not their meaning).
- [ ] **Step 5: Build firmware.** Expected: success.
- [ ] **Step 6: Commit** - `feat(indoor): the plot takes the page inset it was not using`.

---

### Task 7: Note sequences in the audio module

**Files:**
- Modify: `modules/audio/include/audio.hpp`, `modules/audio/audio.cpp`, `modules/audio/README.md`
- Test: `tests/host/test_audio_drain.cpp` (pure helper only)

**Interfaces:**
- Produces (with no-op stubs returning `ESP_ERR_NOT_SUPPORTED` when `CONFIG_AUDIO_ENABLE=n`):
  - `struct audio::Note { int frequency_hz; int duration_ms; };` - `frequency_hz == 0` is a rest.
  - `esp_err_t audio::audio_play_notes(const Note* notes, size_t count);` - blocking; one codec open and one amplifier window for the whole sequence (lead silence, notes each with `kFadeMs` fades, trail silence, `kDrainMs`).
  - `esp_err_t audio::audio_play_notes_async(const Note* notes, size_t count);` - copies the notes, same refuse-if-busy slot as `audio_play_tone_async`.
  - `uint32_t audio::notes_duration_ms(const Note* notes, size_t count)` - header-inline, sum of durations, for `static_assert`/tests.

- [ ] **Step 1: Failing test** `notes_duration_sums_notes_and_rests` (`{{1047,120},{0,30},{1319,120}}` -> 270).
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement** `notes_duration_ms`. **Step 4: Run.** Expected: pass.
- [ ] **Step 5: Implement** `audio_play_notes` by generalising `write_tone_step`'s window so tones and the sequence share one pop-free sequence (`audio_play_tone` becomes a one-note sequence). Reject `count == 0`, any note outside `kMinFrequencyHz..kMaxFrequencyHz` (except rests) or `kMinDurationMs..kMaxDurationMs` (rests may be shorter, >= 10 ms), with `ESP_ERR_INVALID_ARG`. Refuse while a stream is open, like tones.
- [ ] **Step 6: Build firmware** with audio on and with `CONFIG_AUDIO_ENABLE=n` (`idf.py -DSDKCONFIG=... ` or temporarily toggle in `sdkconfig`, then restore). Expected: both succeed.
- [ ] **Step 7: Commit** - `feat(audio): play a short note sequence in one amplifier window`.

---

### Task 8: Quiet-hours setting

**Files:**
- Modify: `components/ui/include/settings_menu.hpp`, `components/ui/settings_menu.cpp`
- Modify: `components/ui/include/ui_strings.hpp`, `components/ui/ui_strings.cpp`
- Modify: `components/ui/render_settings.cpp` (row value text)
- Modify: `main/app_main.cpp` (NVS load/store, registered like `vol_preset`)
- Test: `tests/host/test_settings_menu.cpp`

**Interfaces:**
- Produces:
  - `enum class ui::QuietHoursPreset : uint8_t { Off, From22To07, From23To07, From00To07, Count };` default `From23To07`.
  - `ui::QuietHoursPreset ui::quiet_hours_preset();` `void ui::set_quiet_hours_preset(QuietHoursPreset);` `void ui::set_quiet_hours_store_handler(void (*)(QuietHoursPreset));` `const char* ui::quiet_hours_preset_name(QuietHoursPreset);`
  - `bool ui::in_quiet_hours(QuietHoursPreset preset, uint8_t local_hour);`
  - `SettingsItem::QuietHours` directly after `Volume`; `SettingsAction::QuietHoursChanged`.
  - `Text` entries: `SettingsQuietHours`, `QuietHoursOff`, `QuietHours22To07`, `QuietHours23To07`, `QuietHours00To07` (EN `Quiet hours`, `Off`, `22-07`, `23-07`, `00-07`; TC translations).

- [ ] **Step 1: Failing tests:**
```cpp
HOST_TEST(quiet_hours_23_to_07_wraps_midnight) {
  using ui::QuietHoursPreset; using ui::in_quiet_hours;
  EXPECT_TRUE(in_quiet_hours(QuietHoursPreset::From23To07, 23));
  EXPECT_TRUE(in_quiet_hours(QuietHoursPreset::From23To07, 6));
  EXPECT_TRUE(!in_quiet_hours(QuietHoursPreset::From23To07, 7));
  EXPECT_TRUE(!in_quiet_hours(QuietHoursPreset::From23To07, 22));
}
HOST_TEST(quiet_hours_00_to_07_does_not_wrap)    // 0 and 6 quiet; 7 and 23 not
HOST_TEST(quiet_hours_off_is_never_quiet)         // all 24 hours false
HOST_TEST(quiet_hours_row_cycles_after_volume)    // focus order Language, Volume, QuietHours, ...; activate() cycles Off->22->23->00->Off and returns QuietHoursChanged
```
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement. Step 4: Run.** Expected: pass (update any existing test that hard-codes the item order or count).
- [ ] **Step 5: Wire** NVS key `quiet_preset` in `ui_prefs`, loaded before the first render with fallback `From23To07` on any error, stored by the handler. Measure the row's widest value in both languages; `check-cjk-font.py` passes.
- [ ] **Step 6: Build firmware.** Expected: success.
- [ ] **Step 7: Commit** - `feat(settings): a quiet-hours row`.

---

### Task 9: Sound policy and event sounds

**Files:**
- Create: `components/app_core/include/sound_policy.hpp` (header-only, no ESP/LVGL/audio includes)
- Create: `main/sound_events.hpp`, `main/sound_events.cpp`; Modify: `main/CMakeLists.txt`
- Modify: `main/app_main.cpp` (boot, cable step, battery publish, volume confirm)
- Modify: `components/ui/include/ui_app.hpp`, `components/ui/ui_app.cpp` (button feedback hook)
- Test: create `tests/host/test_sound_policy.cpp`; add it to `tests/host/CMakeLists.txt`

**Interfaces:**
- Produces in `sound_policy.hpp` (namespace `app_core`):
  - `enum class SoundEvent : uint8_t { Boot, Farewell, PowerLost, PowerBack, BatteryLow, BatteryCritical, Button, VolumeConfirm };`
  - `bool sound_allowed(SoundEvent e, bool volume_off, bool quiet_now, bool clock_trusted);` - false if `volume_off`; `VolumeConfirm` ignores quiet hours and trust; every other event false if `quiet_now || !clock_trusted`.
  - `enum class ResetKind : uint8_t { PowerOn, Software, Other };` and `bool boot_sound_for_reset(ResetKind);` - true for `PowerOn` and `Software` only. `sound_events.cpp` maps `esp_reset_reason()` to it: `ESP_RST_POWERON` -> `PowerOn`, `ESP_RST_SW` -> `Software`, everything else (panic, watchdogs, brownout, `ESP_RST_USB`, external) -> `Other`. Keeps the header ESP-free with no mirrored magic numbers.
  - `class BatteryAlarm` with `std::optional<SoundEvent> update(bool valid, bool charging, uint8_t percent);` - fires `BatteryLow` once on crossing below 15, re-armed at >= 20 or when charging; `BatteryCritical` once below 3, re-armed the same way; nothing when `!valid`; the first valid reading after boot does not fire if already below (it only arms).
- Produces in `main/sound_events.hpp` (namespace `sound_events`): `void init(int reset_reason);` `void on_cable_step(bool now_charging);` `void on_battery(const app_core::BatteryData&);` `void on_button();` `void play_farewell_blocking();` (Task 10 uses it). Melodies as `constexpr audio::Note` arrays with `static_assert(audio::notes_duration_ms(...) < 1000)` / `< 400`.
- Produces in UI: `void ui::set_button_feedback_handler(void (*)());` - called for every accepted `ButtonEvent`, **except** the press whose `activate()` returns `SettingsAction::VolumeChanged`.

- [ ] **Step 1: Failing tests** in `test_sound_policy.cpp`: `volume_off_silences_everything` (all 8 events), `quiet_hours_silence_everything_but_the_volume_confirm`, `an_untrusted_clock_counts_as_quiet`, `boot_sound_only_on_power_on_and_software_restart` (`PowerOn`, `Software` true; `Other` false), `battery_low_fires_once_per_discharge`, `battery_alarm_rearms_after_charging_or_recovery`, `battery_alarm_is_silent_without_a_battery` (`valid=false` at 1% -> nothing), `battery_alarm_does_not_fire_on_the_first_reading_after_boot`.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement** `sound_policy.hpp`. **Step 4: Run.** Expected: pass.
- [ ] **Step 5: Wire** `sound_events.cpp`: gate every play through `sound_allowed(...)` with `volume_off = volume_preset() == Off`, `quiet_now = in_quiet_hours(preset, local hour)`, `clock_trusted = clock_is_set(source)`; play with `audio_play_notes_async`. Hook points: `init` after `audio_init()` (boot melody if `boot_sound_for_reset(<mapped esp_reset_reason()>)` - a rising 4-5 note arpeggio); `on_cable_step` where the cable step sets `g_cable_step_charging` (`PowerLost` falling two notes / `PowerBack` one note), log each with the step in mV; `on_battery` from the battery publish path; `on_button` registered as the UI feedback handler (one ~50 ms soft tick); `apply_volume_preset_change` routes through `VolumeConfirm`.
- [ ] **Step 6: Build firmware** with audio on and off. Expected: both succeed; `grep -rn "audio.hpp" components/` prints nothing.
- [ ] **Step 7: Commit** - `feat(sound): soft once-only sounds for boot, power, battery and buttons`.

---

### Task 10: Farewell melody before firmware-initiated restarts

**Files:**
- Create: `components/app_core/include/restart_hook.hpp`, `components/app_core/restart_hook.cpp` (+ `CMakeLists.txt` SRCS)
- Modify: `components/ota/ota_pull.cpp:224`, `components/wifi_provision/portal.cpp:138,151` (and any other `esp_restart()` the grep finds)
- Modify: `main/app_main.cpp` / `main/sound_events.cpp` (register)
- Test: `tests/host/test_sound_policy.cpp`

**Interfaces:**
- Produces: `void app_core::set_before_restart_hook(void (*hook)());` `void app_core::run_before_restart();` - runs the hook once (a second call is a no-op), safe when none is registered.
- Consumes: `sound_events::play_farewell_blocking()` (Task 9) - gated by `sound_allowed(Farewell, ...)`; uses blocking `audio_play_notes`; if the playback slot is busy or a stream is open it returns immediately (no waiting).

- [ ] **Step 1: Failing tests:** `restart_hook_runs_once`, `restart_hook_without_registration_is_harmless`.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement. Step 4: Run.** Expected: pass.
- [ ] **Step 5: Wire:** every firmware `esp_restart()` call site calls `app_core::run_before_restart()` immediately before it. In `restart_timer_fired` (esp_timer context) the blocking melody holds the timer task < 1 s right before a restart - acceptable; note it in a comment. The OTA quiesce has already closed any stream, so the farewell can still play after an OTA.
- [ ] **Step 6: Build firmware.** Expected: success.
- [ ] **Step 7: Commit** - `feat(sound): a farewell melody before the firmware restarts itself`.

---

### Task 11: Debug route to play any event's sound

**Files:**
- Modify: `components/wifi_provision/portal.cpp` (debug-only route next to `/beep`)
- Modify: `components/wifi_provision/include/wifi_provision.hpp` (handler registration)
- Modify: `main/sound_events.cpp` (register the handler)

**Interfaces:**
- Produces: `void wifi_provision::set_sound_test_handler(bool (*handler)(const char* event, bool force));` - core never sees `SoundEvent` names as audio; `main` maps the string.
- Route: `POST /sound?event=boot|farewell|power_lost|power_back|battery_low|battery_critical|button[&force=1]`, gated exactly like `/beep` (debug builds). Without `force` it applies the real policy; `force=1` bypasses quiet hours and trust (never Volume `Off`). 200 + `played`/`suppressed: <reason>`, 400 on an unknown event.

- [ ] **Step 1: Implement** the route and handler.
- [ ] **Step 2: Build** a debug firmware. Expected: success; release build has no `/sound` route (`grep` the handler registration is inside the same guard as `kBeepPost`).
- [ ] **Step 3: Commit** - `feat(portal): a debug route that plays any event's sound`.

---

### Task 12: On the panel - flash and hand the checklist to the operator

- [ ] **Step 1:** Full host suite green; firmware builds (audio on).
- [ ] **Step 2:** Confirm the port by MAC (`a4:cb:8f:df:88:d0`), then `idf.py -p COMn app-flash`. If the MAC does not match, stop.
- [ ] **Step 3:** Hand the operator this checklist and record each answer:
  1. Power on: rising melody (outside quiet hours); Wi-Fi icon blinks, then holds; clock switches to network time within seconds of the icon going solid.
  2. Minute flips in step with a reference clock (watch one rollover).
  3. Home trust line: `SYNCED JUST NOW` -> `SYNCED N MIN AGO`; (unplug the router) stays honest.
  4. Weather row and Home tile at 20 px; read at your usual distance. Worst cases via a debug/mock snapshot: `-24°C`, `100%`, `Snow Showers`, `OLD`, `ALERT`, and Traditional Chinese.
  5. Indoor chart: wider; scale numbers not clipped at the edges.
  6. Pull the cable: one falling chime. Plug back: one note. Buttons: soft tick, not annoying at Medium.
  7. Quiet hours set to cover now: nothing plays (try `POST /sound?event=power_lost`, then with `force=1`).
  8. `POST /restart`: farewell plays, board restarts, boot melody plays.
- [ ] **Step 4:** Commit nothing here unless the checklist forces a fix; any fix goes back through its owning task's test.
