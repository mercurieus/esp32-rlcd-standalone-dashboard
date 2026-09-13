#include "test_support.hpp"

#include "app_snapshot.hpp"
#include "history.hpp"

#include <vector>

namespace {

constexpr uint32_t kIntervalMinutes = 5;

// Builds a window whose charge falls linearly from `from_mv` to `to_mv`.
std::vector<app_core::HistorySample> ramp(int from_mv, int to_mv,
                                          std::size_t count) {
  std::vector<app_core::HistorySample> samples(count);
  for (std::size_t i = 0; i < count; ++i) {
    const double fraction =
        count <= 1 ? 0.0 : static_cast<double>(i) / (count - 1);
    samples[i].battery_millivolts = static_cast<uint16_t>(
        from_mv + (to_mv - from_mv) * fraction);
  }
  return samples;
}

}  // namespace

HOST_TEST(runtime_estimate_refuses_to_guess_without_enough_history) {
  // One reading is a measurement, not a trend. Four is an ADC excursion.
  for (std::size_t count = 0; count < app_core::kMinimumSamplesForEstimate;
       ++count) {
    const auto samples = ramp(4100, 3600, count);
    const auto estimate = app_core::estimate_runtime(
        samples.data(), samples.size(), kIntervalMinutes);
    EXPECT_TRUE(!estimate.known);
    EXPECT_TRUE(estimate.trend == app_core::PowerTrend::Unknown);
  }
}

HOST_TEST(runtime_estimate_projects_a_falling_cell_to_empty) {
  // 4100 -> 3600 mV across four hours of five-minute slots.
  const auto samples = ramp(4100, 3600, 48);
  const auto estimate = app_core::estimate_runtime(
      samples.data(), samples.size(), kIntervalMinutes);

  EXPECT_TRUE(estimate.trend == app_core::PowerTrend::Discharging);
  EXPECT_TRUE(estimate.known);
  EXPECT_TRUE(estimate.percent_per_hour < 0.0f);
  // All 48, not a fixed window: one uninterrupted discharge is one run, and
  // fitting the whole of it is what makes a long observation more precise
  // than a short one.
  EXPECT_EQ(static_cast<int>(estimate.samples_used), 48);

  // The projection has to be consistent with its own slope rather than with a
  // number pinned here: percent left divided by percent lost per hour.
  const int final_percent = app_core::battery_percent(3600);
  const double expected_minutes =
      final_percent / -static_cast<double>(estimate.percent_per_hour) * 60.0;
  const double error =
      static_cast<double>(estimate.minutes_remaining) - expected_minutes;
  EXPECT_TRUE(error < 1.0 && error > -1.0);
}

HOST_TEST(runtime_estimate_reports_charging_rather_than_a_negative_runtime) {
  // Rising voltage. Without a charge-detect line this is the only evidence
  // there is, and the wrong answer here is a runtime computed from a positive
  // slope - which projects backwards and would print a plausible number.
  const auto samples = ramp(3600, 4100, 48);
  const auto estimate = app_core::estimate_runtime(
      samples.data(), samples.size(), kIntervalMinutes);

  EXPECT_TRUE(estimate.trend == app_core::PowerTrend::Charging);
  EXPECT_TRUE(!estimate.known);
  EXPECT_EQ(static_cast<int>(estimate.minutes_remaining), 0);
}

HOST_TEST(a_charge_ends_the_run_so_the_discharge_before_it_is_not_fitted) {
  // Two days of discharge, then a charger. Fitting the whole ring kept
  // reporting Discharging for hours after the cable landed; the run boundary
  // is what closes that, and it has to close it from either side - here the
  // newest reading belongs to the charge, so the discharge before it is out.
  auto samples = ramp(4100, 3600, 200);
  const auto recent = ramp(3600, 3900, 24);
  for (std::size_t i = 0; i < recent.size(); ++i) {
    samples[samples.size() - recent.size() + i] = recent[i];
  }
  const auto estimate = app_core::estimate_runtime(
      samples.data(), samples.size(), kIntervalMinutes);

  EXPECT_TRUE(estimate.trend == app_core::PowerTrend::Charging);
  EXPECT_TRUE(estimate.percent_per_hour > 0.0f);
  // The rising tail and nothing like the 200 slots of discharge under it.
  // Asserted as a bound rather than an exact count on purpose: the boundary
  // lands wherever the margin trips, which is a property of the data, and
  // pinning it exactly would test the fixture instead of the rule.
  EXPECT_TRUE(estimate.samples_used >= 20 && estimate.samples_used < 60);
}

HOST_TEST(a_load_change_is_averaged_over_the_run_rather_than_ending_it) {
  // Flat all day, then a real discharge - a load starting, not a cable.
  //
  // This is the deliberate limit of a run-based window, written down as a
  // test so it is a decision rather than a surprise: nothing here is a
  // direction change, so it is all one run and the reported rate is the
  // average over the whole of it, not the last two hours. It converges as the
  // new load keeps running.
  //
  // The alternative - detecting a change in slope as well as in sign - is a
  // change-point detector, which this board does not need: its load is a
  // clock that draws the same current all day. If that stops being true, this
  // test is where to start.
  auto samples = ramp(3900, 3900, 300);
  const auto recent = ramp(3900, 3700, 24);
  for (std::size_t i = 0; i < recent.size(); ++i) {
    samples[samples.size() - recent.size() + i] = recent[i];
  }
  const auto estimate = app_core::estimate_runtime(
      samples.data(), samples.size(), kIntervalMinutes);

  EXPECT_TRUE(estimate.trend == app_core::PowerTrend::Discharging);
  EXPECT_TRUE(estimate.known);
  // Every slot handed in - the tail overwrites the last 24 of the 300 rather
  // than extending them - so the whole flat stretch is in the fit and the rate
  // is far gentler than the -18 %/h the last two hours alone would show.
  EXPECT_EQ(static_cast<int>(estimate.samples_used), 300);
  EXPECT_TRUE(estimate.percent_per_hour < 0.0f);
  EXPECT_TRUE(estimate.percent_per_hour > -5.0f);
}

HOST_TEST(the_same_slow_drain_becomes_callable_once_there_is_enough_history) {
  // The property the whole redesign exists for, and the failure it replaced:
  // a fixed 0.4 %/h threshold went blind the day power work took the board
  // under 0.4 %/h, and reported Steady forever.
  //
  // Same drain rate and same noise in both windows below - only the length
  // differs. The short one honestly cannot tell it from flat; the long one
  // can, because the standard error of a slope shrinks with both the sample
  // count and the span.
  const auto drain = [](std::size_t count) {
    std::vector<app_core::HistorySample> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
      // About -0.3 %/h near the top of the curve, where 14 mV is a point:
      // -0.35 mV per five-minute slot, plus the +/-1 point of quantisation
      // noise a real reading carries.
      const double mv = 4180.0 - 0.35 * static_cast<double>(i);
      const int wobble = (static_cast<int>(i) % 3 - 1) * 6;
      samples[i].battery_millivolts =
          static_cast<uint16_t>(mv + wobble);
    }
    return samples;
  };

  const auto brief = drain(24);  // two hours
  const auto brief_estimate = app_core::estimate_runtime(
      brief.data(), brief.size(), kIntervalMinutes);
  EXPECT_TRUE(brief_estimate.trend == app_core::PowerTrend::Steady);
  EXPECT_TRUE(!brief_estimate.known);

  const auto lengthy = drain(576);  // the whole ring, 48 hours
  const auto lengthy_estimate = app_core::estimate_runtime(
      lengthy.data(), lengthy.size(), kIntervalMinutes);
  EXPECT_TRUE(lengthy_estimate.trend == app_core::PowerTrend::Discharging);
  EXPECT_TRUE(lengthy_estimate.known);

  // And the reason it became callable is the error, not the rate: the slope
  // is the same drain either way, but it is pinned down far better.
  EXPECT_TRUE(lengthy_estimate.percent_per_hour_stderr <
              brief_estimate.percent_per_hour_stderr);
}

HOST_TEST(runtime_estimate_calls_a_flat_cell_steady_rather_than_eternal) {
  // A finished charger holds the cell at a plateau. Fitting that gives a slope
  // near zero, and dividing by it is how a battery page ends up claiming
  // several months of runtime.
  std::vector<app_core::HistorySample> samples(48);
  for (auto& sample : samples) sample.battery_millivolts = 4199;
  const auto estimate = app_core::estimate_runtime(
      samples.data(), samples.size(), kIntervalMinutes);

  EXPECT_TRUE(estimate.trend == app_core::PowerTrend::Steady);
  EXPECT_TRUE(!estimate.known);
}

HOST_TEST(runtime_estimate_skips_gaps_instead_of_interpolating_them) {
  auto samples = ramp(4100, 3600, 48);
  // A boot with no battery reading leaves holes. They must not count as
  // samples and must not contribute a zero-volt point, which would drag the
  // fit into claiming a catastrophic discharge.
  for (std::size_t i = 0; i < samples.size(); i += 3) {
    samples[i].battery_millivolts = app_core::HistorySample::kNoBattery;
  }
  const auto estimate = app_core::estimate_runtime(
      samples.data(), samples.size(), kIntervalMinutes);

  EXPECT_TRUE(estimate.trend == app_core::PowerTrend::Discharging);
  EXPECT_TRUE(estimate.samples_used < 48);
  EXPECT_TRUE(estimate.samples_used > 0);
  // Still a sane rate rather than the cliff a zero-filled gap would produce.
  EXPECT_TRUE(estimate.percent_per_hour > -100.0f);
}

HOST_TEST(runtime_estimate_is_total_on_degenerate_input) {
  EXPECT_TRUE(!app_core::estimate_runtime(nullptr, 10, 5).known);
  const auto samples = ramp(4100, 3600, 48);
  // A zero interval would divide by zero on the way to hours-per-slot.
  EXPECT_TRUE(!app_core::estimate_runtime(samples.data(), samples.size(), 0)
                   .known);
}

HOST_TEST(history_blob_rejects_what_a_power_cut_leaves_behind) {
  app_core::HistoryBlob blob;
  blob.count = 3;
  blob.samples[0].battery_millivolts = 4100;
  app_core::history_blob_seal(blob, 7);
  EXPECT_TRUE(app_core::history_blob_valid(blob));

  // A write interrupted partway leaves a sector whose contents disagree with
  // its checksum. It has to be skipped, not read back as measurements.
  app_core::HistoryBlob torn = blob;
  torn.samples[0].battery_millivolts = 3000;
  EXPECT_TRUE(!app_core::history_blob_valid(torn));

  // An erased sector is all 0xFF, which is not the magic.
  app_core::HistoryBlob erased;
  erased.magic = 0xFFFFFFFFu;
  EXPECT_TRUE(!app_core::history_blob_valid(erased));

  // A count past the array would walk off the end of the samples on read.
  app_core::HistoryBlob overlong;
  overlong.count = app_core::kHistorySlots + 1;
  app_core::history_blob_seal(overlong, 8);
  EXPECT_TRUE(overlong.count == app_core::kHistorySlots);
}

HOST_TEST(history_crc32_matches_the_known_ieee_vector) {
  // "123456789" -> 0xCBF43926 is the standard CRC-32 check value. Pinning it
  // means a rewrite of the loop cannot quietly change what old sectors mean.
  const char* input = "123456789";
  EXPECT_TRUE(app_core::history_crc32(
                  reinterpret_cast<const uint8_t*>(input), 9) == 0xCBF43926u);
}

HOST_TEST(history_append_keeps_the_newest_window_oldest_first) {
  app_core::HistoryBlob blob;
  for (std::size_t i = 0; i < app_core::kHistorySlots; ++i) {
    app_core::HistorySample sample;
    sample.battery_millivolts = static_cast<uint16_t>(3000 + i);
    app_core::history_append(blob, sample);
  }
  EXPECT_EQ(static_cast<int>(blob.count),
            static_cast<int>(app_core::kHistorySlots));
  EXPECT_EQ(static_cast<int>(blob.samples[0].battery_millivolts), 3000);

  // One past full: the oldest goes, everything shifts, the newest is last.
  app_core::HistorySample extra;
  extra.battery_millivolts = 4200;
  app_core::history_append(blob, extra);
  EXPECT_EQ(static_cast<int>(blob.count),
            static_cast<int>(app_core::kHistorySlots));
  EXPECT_EQ(static_cast<int>(blob.samples[0].battery_millivolts), 3001);
  EXPECT_EQ(
      static_cast<int>(blob.samples[app_core::kHistorySlots - 1]
                           .battery_millivolts),
      4200);
}

HOST_TEST(history_series_keeps_a_real_zero_humidity_and_drops_the_sentinel) {
  // kNoHumidity is 0xFF, so a genuine 0 %RH reading is a measurement and must
  // survive - the sentinel is the only value meaning "nothing was recorded".
  // Easy to get wrong: temperature's sentinel is INT16_MIN, an impossible
  // reading, while humidity's sits just past the top of the range its real
  // values use.
  app_core::HistoryBlob blob;
  app_core::HistorySample dry;
  dry.humidity_percent = 0;
  app_core::history_append(blob, dry);
  app_core::HistorySample absent;  // leaves kNoHumidity in place
  app_core::history_append(blob, absent);

  app_core::HistoryPoint points[2] = {};
  app_core::history_series(blob, points, 2, 1);
  EXPECT_TRUE(points[0].has_humidity);
  EXPECT_EQ(static_cast<int>(points[0].humidity_percent), 0);
  EXPECT_TRUE(!points[1].has_humidity);
}

HOST_TEST(pcf85063_encoding_round_trips_and_clears_the_stop_flag) {
  const app_core::RtcDateTime original{2026, 8, 16, 23, 41, 7};
  uint8_t registers[7] = {};
  EXPECT_TRUE(app_core::encode_pcf85063(original, registers,
                                        sizeof(registers)));

  // Bit 7 of seconds is the oscillator-stop flag. If encoding ever set it, the
  // chip would store the correct time and still report itself invalid on the
  // next read - which is the exact failure this whole path exists to end.
  EXPECT_EQ(static_cast<int>(registers[0] & 0x80), 0);

  app_core::RtcDateTime decoded{};
  EXPECT_TRUE(app_core::decode_pcf85063(registers, sizeof(registers),
                                        decoded));
  EXPECT_EQ(static_cast<int>(decoded.year), 2026);
  EXPECT_EQ(static_cast<int>(decoded.month), 8);
  EXPECT_EQ(static_cast<int>(decoded.day), 16);
  EXPECT_EQ(static_cast<int>(decoded.hour), 23);
  EXPECT_EQ(static_cast<int>(decoded.minute), 41);
  EXPECT_EQ(static_cast<int>(decoded.second), 7);

  // Out of range is refused rather than written as garbage the chip would
  // then hand back as a confident-looking wrong date.
  const app_core::RtcDateTime bad_day{2026, 2, 30, 0, 0, 0};
  EXPECT_TRUE(!app_core::encode_pcf85063(bad_day, registers,
                                         sizeof(registers)));
  const app_core::RtcDateTime bad_year{1999, 1, 1, 0, 0, 0};
  EXPECT_TRUE(!app_core::encode_pcf85063(bad_year, registers,
                                         sizeof(registers)));
  EXPECT_TRUE(!app_core::encode_pcf85063(original, registers, 3));
}

// --- history_series: the positional walk a time axis needs ----------------

HOST_TEST(history_series_samples_by_slot_position_at_the_stride) {
  app_core::HistoryBlob blob;
  for (int i = 0; i < 20; ++i) {
    app_core::HistorySample sample;
    sample.temperature_decic = static_cast<int16_t>(200 + i);
    sample.humidity_percent = static_cast<uint8_t>(40 + i);
    app_core::history_append(blob, sample);
  }

  app_core::HistoryPoint points[4] = {};
  app_core::history_series(blob, points, 4, 3);

  // Newest slot is 19; stepping back 3 at a time gives 10, 13, 16, 19 -
  // oldest-first.
  EXPECT_EQ(static_cast<int>(points[0].temperature_decic), 210);
  EXPECT_EQ(static_cast<int>(points[1].temperature_decic), 213);
  EXPECT_EQ(static_cast<int>(points[2].temperature_decic), 216);
  EXPECT_EQ(static_cast<int>(points[3].temperature_decic), 219);
  for (const auto& point : points) {
    EXPECT_TRUE(point.has_temperature);
    EXPECT_TRUE(point.has_humidity);
  }
  EXPECT_EQ(static_cast<int>(points[3].humidity_percent), 59);
}

HOST_TEST(history_series_leaves_a_gap_where_a_slot_has_no_reading) {
  // This is the whole reason the positional walk exists.
  // The accessor this replaced walked the newest N slots that carried a
  // reading, which skips an empty slot and pulls its neighbours together -
  // on a chart spaced by index that claims an interval nobody measured.
  // Here the hole keeps its place.
  app_core::HistoryBlob blob;
  for (int i = 0; i < 4; ++i) {
    app_core::HistorySample sample;
    // Slot 2 is a boot with the sensor unreadable; it still records, because
    // the gap is information.
    if (i != 2) {
      sample.temperature_decic = static_cast<int16_t>(200 + i);
      sample.humidity_percent = static_cast<uint8_t>(50 + i);
    }
    app_core::history_append(blob, sample);
  }

  app_core::HistoryPoint points[4] = {};
  app_core::history_series(blob, points, 4, 1);

  EXPECT_TRUE(points[0].has_temperature);
  EXPECT_TRUE(points[1].has_temperature);
  EXPECT_TRUE(!points[2].has_temperature);
  EXPECT_TRUE(!points[2].has_humidity);
  EXPECT_TRUE(points[3].has_temperature);
  EXPECT_EQ(static_cast<int>(points[3].temperature_decic), 203);
}

HOST_TEST(history_series_reaching_past_recorded_history_stays_absent) {
  // A board an hour into its first boot cannot answer for eight hours ago.
  // Those points come back absent rather than clamped to the oldest slot,
  // which would stack several points on one reading and draw a flat run that
  // was never measured.
  app_core::HistoryBlob blob;
  for (int i = 0; i < 2; ++i) {
    app_core::HistorySample sample;
    sample.temperature_decic = static_cast<int16_t>(230 + i);
    app_core::history_append(blob, sample);
  }

  app_core::HistoryPoint points[5] = {};
  app_core::history_series(blob, points, 5, 1);

  for (int i = 0; i < 3; ++i) EXPECT_TRUE(!points[i].has_temperature);
  EXPECT_TRUE(points[3].has_temperature);
  EXPECT_EQ(static_cast<int>(points[3].temperature_decic), 230);
  EXPECT_EQ(static_cast<int>(points[4].temperature_decic), 231);
}

HOST_TEST(history_series_on_an_empty_ring_reports_nothing_at_all) {
  app_core::HistoryBlob blob;
  app_core::HistoryPoint points[3] = {};
  app_core::history_series(blob, points, 3, 6);
  for (const auto& point : points) {
    EXPECT_TRUE(!point.has_temperature);
    EXPECT_TRUE(!point.has_humidity);
  }
  // A zero stride would walk the same slot forever; it is clamped to 1.
  app_core::history_series(blob, points, 3, 0);
  EXPECT_TRUE(!points[0].has_temperature);
  app_core::history_series(blob, nullptr, 3, 6);  // must not crash
}
