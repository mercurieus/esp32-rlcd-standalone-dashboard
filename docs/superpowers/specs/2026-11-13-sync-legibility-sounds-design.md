# Faster time, a trust line, legible values, a wider chart, and sounds

2026-11-13. A phase of seven small changes to `esp32-rlcd-standalone`, agreed
with the operator in one conversation. Each section says what the operator
decided, what the builder decided on their behalf (marked **builder's call**),
and what is left open.

The project's standing rule applies to every section here: **nothing reaches
the panel as a number unless it was measured or fetched**, and layouts are
sized from the compiled font's glyph table for the widest value a slot can
hold - never from a sample string.

---

## 1. Network time appears on the panel within seconds of boot

### Why

After power-on the panel keeps showing the RTC (or blinking) time for up to a
minute after SNTP has actually landed. The operator noticed this as "NTP
refresh is slow after power-on".

### What is actually slow

The SNTP request itself was fixed in `90f44bf` (restart on `IP_EVENT_STA_GOT_IP`)
and now lands a few seconds after the station gets an address. The delay is
downstream of it:

- `net_time_monitor_task` (`main/app_main.cpp`, ~line 1408) polls
  `net_time::synced()` every `kNetTimeCheckPeriodMs` = 60 s. Its first poll is
  at boot, before Wi-Fi is up, so the first publish of network time waits for
  the second poll - up to 60 s after the sync landed.
- A second defect found while reading this: once the source is `"SNTP"`,
  `update_clock()` in `components/ui/ui_app.cpp` (~line 236) stops computing
  the minute itself and relies entirely on that 60 s poll to publish each new
  minute. The poll is not phase-aligned to the minute, so **the displayed
  minute can change up to ~59 s late, all day**, not just at boot.

### Decided

1. **The sync callback wakes the monitor task immediately** (operator). The
   `on_sync` callback in `components/net_time/net_time.cpp` signals the task
   (task notification or equivalent) so the first network time is published
   the moment it exists. `net_time` is a core component and the monitor task
   lives in `main`, so the wake-up goes through a registration hook exposed by
   `net_time` (e.g. a callback setter), not an include running the other way.
2. **After that, the task wakes on the real minute boundary**, computed from
   `time()` each iteration (sleep until the next `:00`, plus a small margin),
   instead of a fixed 60 s delay. The displayed minute then changes within a
   fraction of a second of the real one. The daily RTC re-write keeps its
   existing cadence.
3. **A second NTP server, `ua.pool.ntp.org`** (operator), alongside
   `pool.ntp.org`. This needs `CONFIG_LWIP_SNTP_MAX_SERVERS` raised from 1 to
   2. Per the config trap below, set it in `sdkconfig.defaults` **and** confirm
   it landed in the generated `sdkconfig`.

4. **The RTC-only clock gets the same fix** (**builder's call**, found by
   the builder check). Before any sync, `update_clock()` counts minutes from
   *boot*, not from the RTC's own seconds, so an RTC-only clock also flips up
   to ~59 s late. Derive the minute boundary from the RTC reading's seconds
   so it flips on the real minute too.

### Acceptance

- Boot log shows `NTP sync landed` and the panel switches to network time in
  the same second (or the next tick), not up to a minute later.
- Watching the Home clock against a reference: the minute flips within ~1 s
  of the real minute, at an arbitrary time of day (not only after boot).

---

## 2. Home always says whether the clock can be trusted

### Why

The operator wants to glance at Home and know **whether the time can be
trusted right now**. Today Home says `CLOCK FROM RTC` or `CLOCK NOT SYNCED`
when the time is not from the network, and nothing at all when it is.

### Decided

The row `home_layout()` already reserves for that warning (`layout.sync`,
`font_medium()`) is used **always**, not only when something is wrong:

| State | Text |
|---|---|
| Network-synced, last sync under 60 min ago | `SYNCED 3 MIN AGO` (`SYNCED <1 MIN AGO` / `JUST NOW` for 0 - builder's call on the exact wording) |
| Network-synced, last sync 60 min or older | `SYNCED 14:02` (the local time of the last sync) |
| Network-synced, but last sync older than 24 h | a warning form, e.g. `SYNC OLD 14:02` - the time is still probably right but no longer verified (**builder's call** on threshold and wording; the 24 h figure is a proposal, not the operator's) |
| RTC only | `CLOCK FROM RTC` (unchanged) |
| Neither | `CLOCK NOT SYNCED` (unchanged) |

- The "minutes ago" vs "time of day" switch at 60 min is **builder's call**,
  following the operator's "depending on how long it was".
- "Last sync" means the last time SNTP actually delivered a time
  (`on_sync`, which esp_netif fires on every resync - currently only the first
  is logged; the timestamp must be recorded on every one). Hourly resyncs
  (`CONFIG_LWIP_SNTP_UPDATE_DELAY` = 1 h) keep it fresh on a healthy network.
- The line repaints once a minute with the clock, not more often.
- New strings need **Traditional Chinese** translations in `ui_strings.cpp`.
  `rlcd_cjk_20` is a subset font - any CJK glyph not already in it must be
  added by regenerating that font, or the TC line renders with holes.
- Size the row for its widest value in both languages, measured from the
  glyph tables.

---

## 3. The Wi-Fi icon shows three states

### Why

On power-up the tray Wi-Fi icon looks the same the whole time, so the
operator cannot tell "trying to connect" apart from anything else.

### Current behaviour

`set_wifi_icon_state()` (`components/ui/ui_theme.cpp`) is two-state, driven by
`snapshot.setup.connected` (true only in `State::Connected`, i.e. the station
has an IP). Connecting, setup AP and "never configured" all show the slash.

### Decided

| Wi-Fi state (`wifi_config::StateMachine`) | Icon |
|---|---|
| `Connected` (has IP) | solid `wifi-high` |
| `Connecting` (including reconnect after a drop) | `wifi-high` **blinking at 1 Hz**, same rhythm and phase source as the unset-clock blink (`kClockBlinkHalfPeriodMs`) |
| `SetupAp`, no credentials, radio off | `wifi-slash` |

- The snapshot needs the connecting state, not just `connected` - add it to
  `AppSnapshot`'s setup data (e.g. a `connecting` bool or the state enum) in
  `wifi_provision.cpp`'s `apply_state_and_publish()`.
- The blink toggles visibility in place, like the clock blink - no page
  rebuild. It only runs while connecting, so the extra repaints last seconds
  on a healthy network. If the AP is down, the station keeps retrying until
  its retry budget sends it to `SetupAp`; the icon blinks for that whole
  period, which is correct (it *is* trying).
- Applies wherever the tray draws the network icon (every page with a tray).
  Home has no tray; Home is unaffected.

---

## 4. Readings in small rows get bigger

### Why

On the Weather page the `FEELS 0.3°C RAIN 60%` row, and on Home the weather
tile's `Rain ~0°C` line, are set in `font_small()` (Montserrat 14) and are
hard to read from a distance. The operator asked for **at least the values
(digits and unit) to be bigger**.

### Decided

- **Values go to `font_medium()` (Montserrat 20); captions (`FEELS`, `RAIN`,
  the condition word) stay at 14.** Mixed sizes on one row align on the
  **baseline**, as the indoor header already does (montserrat_20 is 22/4,
  montserrat_14 is 16/3: small text at `y + 5` meets a 20 box at `y`).
- No new font size is introduced (a 16/17 px size would need a new Montserrat
  *and* a new CJK subset font, costing flash).
- **Measure before changing the layout.** The Weather row's current widest
  string, `FEELS -24.1°C RAIN 100%`, is 173 px in a 186 px box at 14 px. With
  values at 20 px it will not fit on one line. If the measurement confirms
  that, **stack** each caption above its value (two small pairs, caption 14
  over value 20) rather than shrinking anything - there is vertical room to
  find between the headline (`y + 4 .. y + 39`) and the divider (`y + 70`),
  and moving the divider down a few pixels at the forecast's expense is
  acceptable if it is needed. The final arrangement is **builder's call**,
  made against measured widths for the worst case of every field.
- The Home tile line (`render_shared.cpp`, the `"%s%s ~%s%s"` detail, ~line
  308) gets the same treatment: the `~0°C` value at 20, the condition word at
  14, sized for the widest condition word plus the widest temperature.
- **When the Home tile line cannot fit, the condition word gives way, never
  the value** (**builder's call**). Measured by the builder check against the
  176 px detail row of a half-width tile: `Snow Showers ~-24°C` (condition 14,
  value 20) is ~172 px and fits; with ` OLD` it is ~205 px and with the
  `ALERT  ` prefix ~260 px (that one already overflows at all-14 today). In
  those cases the condition word ellipsises or is dropped first; the value,
  `OLD` and `ALERT` always render. The tile's 18 px detail-row height is not
  changed.
- **All tiles' detail-row readings get the same treatment** (**builder's
  call**): the Indoor tile (`RH 55%`) and Market tile (`+0.12%`) share the row
  with the weather tile when two tiles show, so their values also go to 20 on
  the same baseline, captions at 14. Same worst-case measurement rule.
- The battery tile's status word (`OVERVOLTAGE` in the screenshot) is a word,
  not a reading - it stays at 14, aligned to the same baseline.
- The `OLD` stale marker must still render when present.
- Placeholder/mock text for these rows should be the worst case, so the next
  reader sees the constraint.

---

## 5. The indoor chart uses the blank space beside its scales

### Why

On the Indoor page there is visibly blank space left of the temperature scale
and right of the humidity scale. The operator wants the plot wider into it.

### What the space actually is

Two things, measured from `indoor_layout()` in `components/ui/include/ui_data.hpp`:

- An 8 px page `inset` on each side (16 px total).
- The temperature gutter (43 px) is sized for `-24.4`, 35.4 px; real indoor
  readings like `22.9` are ~10 px narrower and are right-aligned, so the
  left of the gutter is usually empty. The humidity gutter (31 px) is sized
  for `100`, 23.8 px; the screenshot shows `68` with blank to its right.

- Outside both of those, every page sits inside `safe_canvas()`'s 6 px
  `kSafeMargin` (`components/ui/include/ui_theme.hpp`).

So the reclaimable width is **roughly 12-16 px, not the ~40 px first
estimated in conversation**. The plot is 310 px today.

### Decided

- **Remove the chart's own 8 px side inset; keep the 6 px safe-canvas margin**
  (**builder's call**). The scale text then sits at the same 6 px from the
  panel edge as every other page's content, and the indoor page stays inside
  the safe-canvas contract and its `within_safe_canvas` checks. The header row
  above keeps its inset.
- Keep the widest-value sizing rule for the gutters. Do **not** shrink the
  temperature gutter on the assumption that indoor readings are never
  negative. **Builder's call** whether anything else is honestly reclaimable
  (e.g. the min/max end labels currently carry a decimal - `22.9`, `19.3` -
  while the round labels do not; whether that decimal is worth its width).
- The existing `static_assert`s on gutter/plot geometry stay and are updated,
  including the minimum plot width one.
- Chart rules unchanged: no bridging of gaps, hour labels only with a synced
  clock, labels never overlaid on the plot (that was tried and rejected - see
  the comment in `indoor_layout()`).

---

## 6. Sounds

### Why

The operator wants soft, short sounds - "not annoying and not loud" - for
start-up and for important events, power loss being the example given, plus
button presses.

### Events and tones

All tones are clean sines through the existing `modules/audio` path (fades,
pop-free amplifier sequence, GPIO46 high only while playing), at the current
Volume preset. Frequencies and durations are **builder's call** within
"short and soft": event tones under ~400 ms total, melodies under ~1 s, in
the 1-3 kHz range the README found audible on this speaker.

| Event | Sound | Trigger |
|---|---|---|
| Power on | **a short melody** (operator), e.g. a 4-5 note rising arpeggio, under ~1 s | once, after audio init, on `ESP_RST_POWERON` and `ESP_RST_SW` only (see below) |
| Power lost | falling two notes | the cable-step detector's **unplug** event (see below) |
| Power back | one short note | the cable-step detector's **plug-in** event - **builder's call**, proposed as the pair to power lost |
| Battery low | one tone, once | `battery.percent` crosses below 15% while not charging; not repeated until it has recovered above the threshold (hysteresis, e.g. re-arm at 20%) - the 15% figure is **builder's call** |
| Button | a very short soft tick | each KEY / BOOT press the UI accepts |
| Shutdown: restart | **a short falling melody** (operator), the power-on melody's mirror, under ~1 s | immediately before every restart the firmware itself initiates (OTA success, `/restart`, a settings reboot): play it **to completion**, then call `esp_restart()` |
| Shutdown: battery out | the same falling melody, once | the cell reaches a critical level while not charging (proposed ~3%, or the voltage equivalent - **builder's call**), i.e. the board is about to die; not repeated on that discharge |

**There is no shutdown sound on the PWR button, by design.** PWR is wired only
to a hardware power controller (U3) that cuts the whole supply, USB included;
the firmware never sees the press and gets no warning (traced from the
schematic - see `.agents/skills/esp32-s3-rlcd-dev/references/power-and-schematic.md`).
The operator chose to keep this phase software-only rather than add the wire
that would make PWR readable.

Each plays **once per event, never looped or repeated**.

### Rules

- **Volume `Off` silences everything.**
- **Quiet hours silence everything**, including power lost and button ticks
  (operator: "silent at night"). The power-on chime is silent in quiet hours
  too (**builder's call** - a 3am reboot should not wake anyone).
- **Unsynced clock counts as quiet hours** (**builder's call**): if the clock
  is neither SNTP nor a valid RTC read, the board cannot know it is daytime,
  so it plays nothing.
- **Power sounds hook the cable-step event, not `battery.charging`**
  (**builder's call**, found by the builder check). `battery.charging` also
  changes with nobody touching the cable: the step verdict expires after
  `kCableHoldTicks` (~11 min) and a just-unplugged full cell can read
  "charging" again; a half-charged cell on the charger flips false -> true
  once the slope window fills after boot; a full cell flips when the charger
  finishes. Chiming on those would be exactly the nagging this design must
  not do. So the sound fires only where `battery_monitor_task` detects a step
  of `kCableStepMillivolts` and sets `g_cable_hold_ticks` - one sound per
  detected step, its direction from `g_cable_step_charging`. The detector is
  biased towards missing a step; a missed step means no sound, which is the
  acceptable failure. Log every power sound with the step that caused it.
  Never sound when no battery is connected.
- **Power-on melody only on a real start** (**builder's call**): plays when
  `esp_reset_reason()` is `ESP_RST_POWERON` (PWR button / cable to a dead
  board) or `ESP_RST_SW` (OTA restart, `/restart`, settings reboot). Not on
  panic, watchdog, brownout or `ESP_RST_USB` - a crash loop must not chime on
  every cycle, and opening the serial port resets this board.
- **Volume row:** the press that changes Volume plays the existing
  confirmation tone (`apply_volume_preset_change()`, 2 kHz / 150 ms) and **no
  button tick** - otherwise the tick takes the audio module and the
  confirmation is refused as busy. The confirmation tone **ignores quiet
  hours** (it is the operator explicitly asking to hear the level), but still
  respects Volume `Off` (**builder's call**).
- Tones must never block the display tick or a button handler -
  `audio_play_tone_async()` exists for this, but it is **single tone and
  refuse-if-busy**. Two-note chimes need a small sequence API in the audio
  module (e.g. `audio_play_notes_async(const Note*, n)`), playing all notes
  inside one amplifier-on window. A request while busy is dropped, not queued
  (a button tick during a chime is simply lost - fine).
- While AirPlay is streaming, tones are already refused by the audio module;
  that stays.

### Module boundaries

`modules/README.md` rule 4 holds: no core component includes `audio.hpp`.
The event-to-sound policy (which event, quiet-hours check, volume) lives in
`main/` (a new `main/sound_events.cpp` or inside `app_main.cpp` - builder's
call). The button tick needs `ui_app.cpp`'s input handling to report presses
outward through a registered callback, the same pattern as
`set_volume_preset_store_handler`. With `CONFIG_AUDIO_ENABLE=n` everything
here compiles to the existing no-op stubs.

---

## 7. Quiet hours setting, and testing the sounds

### Decided

- **A new on-device settings row, `Quiet hours`**, directly after `Volume` in
  `SettingsItem`, cycling presets on OK like Volume does:
  `Off` / `22-07` / `23-07` / `00-07`. Persisted in NVS next to
  `vol_preset` (namespace `kUiNamespace`, new key e.g. `quiet_preset`),
  loaded before the first tone can play, falling back to a default on any NVS
  error. Default: **`23-07`** (**builder's call**).
- A window wrapping midnight is the normal case; `00-07` does not wrap.
  Evaluated against local time.
- TC translations for the row label and values, same CJK-subset caveat as §2.

### Testing (the operator asked for this to be easy)

- On the device: set `Quiet hours` to `Off` to hear everything, or pick a
  window covering the current time to confirm silence.
- **Debug builds only**, gated exactly like `POST /beep`: a route such as
  `POST /sound?event=power_lost` (and `boot`, `power_back`, `battery_low`,
  `button`) that plays the event's sound **through the same policy**
  (volume, quiet hours) so it tests what the board would really do; an
  optional `force=1` bypasses quiet hours for hearing the tone itself.
- A general web settings page is **not** in this phase. After setup the
  portal deliberately keeps its forms shut (only the firmware upload
  answers); opening it for settings is the backlog "web interface" project.

---

## Non-goals

- A web settings page (see §7).
- Alarms, chimes on the hour, notification sounds beyond the five events.
- New font sizes or regenerating Montserrat.
- Changing chart semantics, history, or the trend arrows.

## Anti-goals (would make this a failure even if it works)

- Any sound at night, or one that repeats or nags.
- A clock that says `SYNCED` when it is not, or a stale sync that reads as
  fresh.
- A layout that fits the screenshot's values but overflows on a real worst
  case (`-24.1°C`, `100%`, the longest WMO condition, TC strings).
- The amplifier on outside a tone (battery drain, and it would pollute the
  drain history).

## Config trap

`sdkconfig.defaults` only seeds a fresh `sdkconfig`. After changing
`CONFIG_LWIP_SNTP_MAX_SERVERS`, grep the generated `sdkconfig` to confirm the
value landed; otherwise the second server is silently ignored.

## Verification on the panel

A clean build proves nothing about layout or sound on this device. The
operator judges:

- Boot: time switches to network time within seconds; the Wi-Fi icon blinks
  until connected, then holds; boot chime plays (outside quiet hours).
- Home trust line in each state (fresh, >1 h, RTC-only via no network).
- Weather row and Home tile at worst-case values (use a debug/mock snapshot
  for `-24.1°C`, `100%`, the longest condition, and TC).
- Indoor chart edges: scale text not clipped at the panel edge.
- Pull the cable: one falling chime; plug back: one note; with quiet hours
  covering now: nothing.
- Button ticks are soft enough not to annoy at the Medium preset.
