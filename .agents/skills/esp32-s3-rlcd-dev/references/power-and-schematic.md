# Power path, buttons and schematic facts

Read off Waveshare's own schematic on 2026-11-13. Everything here is what the
drawing shows; where the drawing does not settle something, it says so.

| | |
| --- | --- |
| Source | <https://files.waveshare.com/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-schematic.pdf> (linked from Waveshare's *Resources and Documents* page) |
| SHA-256 at retrieval | `63e679d106253ac6ce8c4eaa69610f34f879660f372953f15e259fc2a7dac1aa` |
| Size | 1,093,838 bytes, one A4-landscape sheet |

The PDF is not vendored into this public repository: Waveshare publishes it
without a stated licence. Fetch it from the URL above and compare the hash; a
different hash means the drawing was revised and the facts below need
re-checking against it.

The PDF is vector, so text extraction gives the net list (`NL<net>` followed by
the `PI<ref><pin>` pins on it), which is how the connections below were
traced, and rendering a region at 4-6x zoom gives a readable picture of it.

## The PWR button is invisible to firmware

**PWR (Key3) is wired only to U3, an ECJ23001-4FCBD6 push-button on/off
controller.** It is not on any ESP32-S3 pin. The `KEYS` block on the schematic
holds just the two application buttons:

| Ref | Button | Net | Pull-up | Note |
| --- | --- | --- | --- | --- |
| Key1 | BOOT | GPIO0 | R8 10k | C14 100 nF to GND; download strap |
| Key4 | KEY | `KEY` = GPIO18 | R17 10k | |
| Key3 | PWR | U3 pin 3 (`KEY`) | inside U3 | nothing else on that net |

Also in that block: R9 10k + C15 100 nF on `CHIP_PU` - an RC power-on reset
with no reset button.

### What U3 switches

```
VBUS --D1 (MBR230LSFT1G)--+
                          +-- VDD --[Q1 AO3401, gate = OUTH]-- VSYS --> TPS63020 --> VCC3V3
B+ --Q3 AO3401 (Q4/Q6)----+                                      \--> RT9193-33 --> A3V3 (audio analog)

U3 OUT (pin 1) --R10 1k-- OUTH ; R12 100k OUTH->VDD holds Q1 off by default
```

- USB and the cell are merged into `VDD` (D1 from VBUS, Q3 as an ideal diode
  from the cell) **before** Q1. So a PWR long-press cuts `VSYS` **even on USB
  power** - USB does not keep the board alive.
- When U3 releases Q1, `VSYS` and then `VCC3V3` collapse with only the bulk
  capacitance behind them: milliseconds, not the hundreds of milliseconds a
  sound or an NVS commit would need.

### Consequences for firmware

- **A PWR press cannot be observed, timed or anticipated in software.** A
  short PWR press on a running board does nothing visible (it is "power on");
  a long press removes power with no warning to the application.
- There is **no shutdown hook**: no interrupt, no GPIO edge, nothing to save
  state or play a sound on. Anything that must survive a power-off has to be
  written ahead of time, not at shutdown.
- The brown-out detector may fire as the rail falls, but it leaves no usable
  time. Do not design around it.
- U3's long-press hold time is **not known** - no datasheet for the
  ECJ23001-4FCBD6 was found. Measure it with a stopwatch if it ever matters.

### If firmware ever needs to see PWR (hardware modification, not done)

Run a wire from U3's `KEY` net (Key3's non-ground side) to a free GPIO on the
P1 header through a series resistor, configured as input with no internal
pull, active-low. **First measure the idle voltage on U3 `KEY` with a
multimeter:** there is no external pull-up on that net, so U3 must pull it up
internally - most likely towards its own `VDD`, which is the merged
battery/USB rail (up to ~5 V on USB). If it idles above 3.3 V the GPIO needs a
divider or a diode-OR instead of a plain resistor. Then the firmware would see the press
begin and have U3's hold time to act in.

## Other facts from the drawing

- **P1 (2x8, 2.54 mm header)** brings out GPIO0, GPIO1, GPIO2, GPIO3, GPIO17,
  GPIO18, U0TXD (43), U0RXD (44), the I2C pair (SDA 13 / SCL 14), the second
  USB pair (`USB'_N` / `USB'_P`, GPIO19/20), VBUS, VCC3V3 and GND. GPIO1-3 and
  GPIO17 are free on this board.
- **GPIO15 is `RTC_INT`** - the PCF85063's interrupt output. Unused by this
  firmware today; it is the way to wake on an RTC alarm.
- **Battery sense:** VBAT -> R21 200k / R23 100k -> `BAT_ADC` = GPIO4, with
  C21 and C33 100 nF. That is the 3x divider; the per-unit calibration rule in
  `SKILL.md` still applies.
- **Charging:** U7 ETA6098 switching charger from VBUS (ISET R11 82k), cell
  protection U11 S8261 with M1 8205 dual MOSFET. No charge-status line reaches
  the ESP32 - charging is only inferable from the battery voltage, which is
  why this firmware derives `charging` from voltage steps and slope.
- **Audio:** ES8311 (U9) DAC -> NS4150B (U12) amplifier, `CTRL` = `PA_CTRL` =
  GPIO46; ES7210 (U10) mic ADC at I2C 0x40 with two microphones. The speaker
  connector **H2 is labelled PH2.0 2-pin** on the schematic (the audio module
  README calls it MX1.25 - check the physical part before buying a lead).
- **RTC backup cell:** J7 (SH1.0 2-pin), charged from VCC3V3 through a B5819WS
  diode. Rechargeable ML1220 only, never CR1220.
- The pin table has a **QMI8658C** (IMU) column, but no IMU answers on this
  board's I2C bus (the scan finds 0x18, 0x40, 0x51, 0x70), so treat it as not
  fitted.
- **microSD** is on GPIO21 / 38 / 39 (`MOSI`/`SCK`/`MISO` on the drawing,
  used as SDMMC CMD / CLK / D0).
