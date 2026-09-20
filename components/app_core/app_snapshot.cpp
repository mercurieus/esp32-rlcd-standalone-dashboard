#include "app_snapshot.hpp"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// Real dividers run several percent off nominal. To calibrate: read the
// reported millivolts (setup tray / log), measure the actual cell voltage
// with a multimeter across the battery terminals, then set
// CONFIG_BATTERY_CALIBRATION_PERMILLE = 1000 * multimeter_mV / reported_mV
// via `idf.py menuconfig` (Battery sensing). Defaults to 1000 (no trim); the
// host build always uses the untrimmed default so this stays pure/testable.
#ifndef CONFIG_BATTERY_CALIBRATION_PERMILLE
#define CONFIG_BATTERY_CALIBRATION_PERMILLE 1000
#endif

namespace app_core {
namespace {

struct BatteryBreakpoint {
  int millivolts;
  uint8_t percent;
};

// Single-cell Li-ion discharge curve, highest voltage first.
constexpr BatteryBreakpoint kBatteryCurve[] = {
    {4200, 100}, {4060, 90}, {3980, 80}, {3920, 70}, {3870, 60},
    {3820, 50},  {3790, 40}, {3700, 30}, {3620, 20}, {3500, 10},
    {3300, 5},   {3000, 0},
};

bool decode_bcd(uint8_t value, uint8_t mask, uint8_t maximum,
                uint8_t& decoded) {
  value &= mask;
  const uint8_t low = value & 0x0f;
  const uint8_t high = static_cast<uint8_t>((value >> 4) & 0x0f);
  if (low > 9 || high > 9) return false;
  decoded = static_cast<uint8_t>(high * 10 + low);
  return decoded <= maximum;
}

uint8_t days_in_month_impl(uint16_t year, uint8_t month) {
  static constexpr uint8_t days[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  if (month == 2 &&
      (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
    return 29;
  }
  return days[month - 1];
}

// The extremes are the widest text the chart's markers and the header can be
// asked to draw, not a flattering sample: -10.5 C and 100 %RH. A layout that
// only fits pleasant numbers should fail here rather than on the panel in
// January. The gap at index 3 is deliberate too - a slot the sensor did not
// answer for is the normal case early in a boot, and the chart has to draw a
// break there rather than bridge it.
IndoorData indoor() {
  IndoorData data;
  data.temperature_c = 24.8;
  data.humidity_percent = 57;
  constexpr double kTemperatures[kIndoorHistoryPoints] = {
      -10.5, 24.1, 24.2, 0.0,  24.4, 24.5, 24.6, 24.7,
      24.9,  25.0, 24.9, 24.8, 24.7, 24.8, 24.8, 24.8};
  constexpr uint8_t kHumidity[kIndoorHistoryPoints] = {
      100, 61, 60, 0, 59, 58, 58, 57, 57, 56, 56, 57, 57, 57, 57, 57};
  for (std::size_t i = 0; i < kIndoorHistoryPoints; ++i) {
    const bool present = i != 3;
    data.history[i].has_temperature = present;
    data.history[i].has_humidity = present;
    data.history[i].temperature_c = kTemperatures[i];
    data.history[i].humidity_percent = kHumidity[i];
  }
  data.history_interval_minutes = 30;
  data.history_time_known = true;
  data.history_newest_hour = 9;
  data.history_newest_minute = 41;
  return data;
}

MarketData ua_fx_market() {
  MarketData market;
  market.display_name = "UA Exchange Rate";
  market.primary_label = "USD/UAH";
  market.primary_value = 4'455;  // 44.55, hundredths - see value_has_decimals.
  market.primary_change_percent = 0.18;
  market.secondary_label = "EUR/UAH";
  market.secondary_value = 5'168;  // 51.68.
  market.secondary_change_percent = -0.09;
  market.value_has_decimals = true;
  return market;
}

MarketData us_market() {
  MarketData market;
  market.display_name = "US Market";
  market.primary_label = "S&P 500";
  market.primary_value = 5'432;
  market.primary_change_percent = -0.18;
  market.secondary_label = "NASDAQ";
  market.secondary_value = 17'667;
  market.secondary_change_percent = 0.21;
  market.intraday_samples = {5'410, 5'425, 5'420, 5'438,
                             5'430, 5'440, 5'426, 5'432};
  return market;
}

WeatherData taipei_weather() {
  WeatherData weather;
  weather.current = {.location = "Taipei",
                     .condition = "Cloudy",
                     .temperature_c = 29.0,
                     .has_feels_like = true,
                     .feels_like_c = 31.0,
                     .rain_probability_percent = 40};
  weather.alert = true;
  weather.seven_day = {{{.day = "Sat",
                         .day_number = 21,
                         .condition = "Cloudy",
                         .high_c = 30.0,
                         .low_c = 25.0,
                         .rain_probability_percent = 25},
                        {.day = "Sun",
                         .day_number = 22,
                         .condition = "Rain",
                         .high_c = 28.0,
                         .low_c = 24.0,
                         .rain_probability_percent = 70},
                        {.day = "Mon",
                         .day_number = 23,
                         .condition = "Rain",
                         .high_c = 27.0,
                         .low_c = 23.0,
                         .rain_probability_percent = 65},
                        {.day = "Tue",
                         .day_number = 24,
                         .condition = "Cloudy",
                         .high_c = 29.0,
                         .low_c = 24.0,
                         .rain_probability_percent = 35},
                        {.day = "Wed",
                         .day_number = 25,
                         .condition = "Sunny",
                         .high_c = 31.0,
                         .low_c = 25.0,
                         .rain_probability_percent = 10},
                        {.day = "Thu",
                         .day_number = 26,
                         .condition = "Cloudy",
                         .high_c = 30.0,
                         .low_c = 25.0,
                         .rain_probability_percent = 30},
                        {.day = "Fri",
                         .day_number = 27,
                         .condition = "Sunny",
                         .high_c = 32.0,
                         .low_c = 26.0,
                         .rain_probability_percent = 5}}};
  return weather;
}

WeatherData new_york_weather() {
  WeatherData weather;
  weather.current = {.location = "New York",
                     .condition = "Sunny",
                     .temperature_c = 22.0,
                     .has_feels_like = true,
                     .feels_like_c = 20.0,
                     .rain_probability_percent = 15};
  weather.alert = false;
  weather.seven_day = {{{.day = "Sat",
                         .day_number = 21,
                         .condition = "Sunny",
                         .high_c = 24.0,
                         .low_c = 18.0,
                         .rain_probability_percent = 10},
                        {.day = "Sun",
                         .day_number = 22,
                         .condition = "Cloudy",
                         .high_c = 23.0,
                         .low_c = 17.0,
                         .rain_probability_percent = 25},
                        {.day = "Mon",
                         .day_number = 23,
                         .condition = "Rain",
                         .high_c = 21.0,
                         .low_c = 16.0,
                         .rain_probability_percent = 60},
                        {.day = "Tue",
                         .day_number = 24,
                         .condition = "Cloudy",
                         .high_c = 22.0,
                         .low_c = 17.0,
                         .rain_probability_percent = 35},
                        {.day = "Wed",
                         .day_number = 25,
                         .condition = "Sunny",
                         .high_c = 25.0,
                         .low_c = 18.0,
                         .rain_probability_percent = 10},
                        {.day = "Thu",
                         .day_number = 26,
                         .condition = "Sunny",
                         .high_c = 26.0,
                         .low_c = 19.0,
                         .rain_probability_percent = 5},
                        {.day = "Fri",
                         .day_number = 27,
                         .condition = "Cloudy",
                         .high_c = 24.0,
                         .low_c = 18.0,
                         .rain_probability_percent = 20}}};
  return weather;
}

}  // namespace

uint8_t days_in_month(uint16_t year, uint8_t month) {
  if (month == 0 || month > 12) return 0;
  return days_in_month_impl(year, month);
}

const char* ota_phase_label(OtaPhase phase) {
  switch (phase) {
    case OtaPhase::Idle:
      return "";
    case OtaPhase::AwaitingConfirm:
      return "UPDATE OFFERED";
    case OtaPhase::Receiving:
      return "UPDATING";
    // Distinct from UPDATING because this is the phase where a power cut is
    // least survivable, and a user watching the panel deserves to know the
    // difference between "still downloading" and "committing".
    case OtaPhase::Writing:
      return "FINISHING UPDATE";
    case OtaPhase::Verifying:
      return "VERIFYING UPDATE";
    case OtaPhase::RolledBack:
      return "UPDATE ROLLED BACK";
    case OtaPhase::Failed:
      return "UPDATE FAILED";
  }
  return "";
}

RtcDateTime advance_rtc_datetime(RtcDateTime clock,
                                 uint64_t elapsed_seconds) {
  const uint64_t total_seconds = clock.second + elapsed_seconds;
  const uint64_t total_minutes =
      static_cast<uint64_t>(clock.hour) * 60 + clock.minute + total_seconds / 60;
  clock.hour = static_cast<uint8_t>((total_minutes / 60) % 24);
  clock.minute = static_cast<uint8_t>(total_minutes % 60);
  clock.second = static_cast<uint8_t>(total_seconds % 60);
  uint64_t days = total_minutes / (24 * 60);
  while (days-- > 0) {
    if (++clock.day > days_in_month_impl(clock.year, clock.month)) {
      clock.day = 1;
      if (++clock.month > 12) {
        clock.month = 1;
        ++clock.year;
      }
    }
  }
  return clock;
}

bool decode_pcf85063(const uint8_t* registers, std::size_t length,
                     RtcDateTime& decoded) {
  if (registers == nullptr || length < 7) return false;

  uint8_t second = 0;
  uint8_t minute = 0;
  uint8_t hour = 0;
  uint8_t day = 0;
  uint8_t weekday = 0;
  uint8_t month = 0;
  uint8_t year = 0;
  if (!decode_bcd(registers[0], 0x7f, 59, second) ||
      !decode_bcd(registers[1], 0x7f, 59, minute) ||
      !decode_bcd(registers[2], 0x3f, 23, hour) ||
      !decode_bcd(registers[3], 0x3f, 31, day) || day == 0 ||
      !decode_bcd(registers[4], 0x07, 6, weekday) ||
      !decode_bcd(registers[5], 0x1f, 12, month) || month == 0 ||
      !decode_bcd(registers[6], 0xff, 99, year)) {
    return false;
  }
  (void)weekday;
  const uint16_t full_year = static_cast<uint16_t>(2000 + year);
  if (day > days_in_month_impl(full_year, month)) return false;
  decoded = {full_year, month, day,
             hour, minute, second};
  return true;
}

bool encode_pcf85063(const RtcDateTime& clock, uint8_t* registers,
                     std::size_t length) {
  if (registers == nullptr || length < 7) return false;
  if (clock.year < 2000 || clock.year > 2099) return false;
  if (clock.month < 1 || clock.month > 12) return false;
  if (clock.day < 1 || clock.day > days_in_month_impl(clock.year, clock.month)) {
    return false;
  }
  if (clock.hour > 23 || clock.minute > 59 || clock.second > 59) return false;

  const auto to_bcd = [](uint8_t value) -> uint8_t {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
  };
  // Bit 7 of the seconds register is the oscillator-stop flag. Writing it as 0
  // is what tells the chip its time is trustworthy again; leaving it set would
  // store the right time and still report it as invalid on the next read.
  registers[0] = static_cast<uint8_t>(to_bcd(clock.second) & 0x7F);
  registers[1] = to_bcd(clock.minute);
  registers[2] = to_bcd(clock.hour);
  registers[3] = to_bcd(clock.day);
  // Weekday is not carried in RtcDateTime and nothing reads it back - decode
  // discards it too. Zero rather than a computed value nobody checks.
  registers[4] = 0;
  registers[5] = to_bcd(clock.month);
  registers[6] = to_bcd(static_cast<uint8_t>(clock.year - 2000));
  return true;
}

int battery_millivolts_scaled(int adc_millivolts, int calibration_permille) {
  return adc_millivolts * 3 * calibration_permille / 1000;
}

int battery_millivolts(int adc_millivolts) {
  return battery_millivolts_scaled(adc_millivolts,
                                   CONFIG_BATTERY_CALIBRATION_PERMILLE);
}

bool battery_reading_valid(int millivolts) {
  return millivolts >= kBatteryValidThresholdMillivolts;
}

uint8_t battery_percent(int millivolts) {
  constexpr std::size_t last =
      sizeof(kBatteryCurve) / sizeof(kBatteryCurve[0]) - 1;
  if (millivolts >= kBatteryCurve[0].millivolts) return kBatteryCurve[0].percent;
  if (millivolts <= kBatteryCurve[last].millivolts) {
    return kBatteryCurve[last].percent;
  }
  for (std::size_t index = 0; index < last; ++index) {
    const BatteryBreakpoint& hi = kBatteryCurve[index];
    const BatteryBreakpoint& lo = kBatteryCurve[index + 1];
    if (millivolts > hi.millivolts || millivolts < lo.millivolts) continue;
    const double span = hi.millivolts - lo.millivolts;
    const double frac = (millivolts - lo.millivolts) / span;
    return static_cast<uint8_t>(
        lo.percent + frac * (hi.percent - lo.percent) + 0.5);
  }
  return 0;  // unreachable: breakpoints cover [3000, 4200] contiguously.
}

int smoothed_battery_millivolts(const int* recent_millivolts, int count) {
  if (recent_millivolts == nullptr || count <= 0) return 0;
  long sum = 0;
  for (int i = 0; i < count; ++i) sum += recent_millivolts[i];
  return static_cast<int>(sum / count);
}

bool voltage_suggests_charging(const int* recent_millivolts, int count) {
  if (recent_millivolts == nullptr || count <= 0) return false;
  for (int i = 0; i < count; ++i) {
    if (recent_millivolts[i] < kChargingVoltageThresholdMillivolts) return false;
  }
  return true;
}

bool battery_voltage_slope(const int* ordered_millivolts, int count,
                           int seconds_per_sample, float* out) {
  if (out == nullptr) return false;
  if (ordered_millivolts == nullptr || seconds_per_sample <= 0) return false;
  if (count < 2) return false;
  if ((count - 1) * seconds_per_sample < kChargingSlopeMinSpanSeconds) {
    return false;
  }

  // Least squares on millivolts against hours. Time origin is arbitrary for a
  // slope, so index units are used and scaled once at the end - it keeps the
  // sums small and avoids a per-sample multiply.
  double sum_i = 0.0, sum_v = 0.0, sum_iv = 0.0, sum_ii = 0.0;
  for (int i = 0; i < count; ++i) {
    const double v = static_cast<double>(ordered_millivolts[i]);
    sum_i += i;
    sum_v += v;
    sum_iv += i * v;
    sum_ii += static_cast<double>(i) * i;
  }
  const double n = static_cast<double>(count);
  const double denominator = n * sum_ii - sum_i * sum_i;
  if (denominator == 0.0) return false;

  const double per_sample = (n * sum_iv - sum_i * sum_v) / denominator;
  const double samples_per_hour = 3600.0 / seconds_per_sample;
  *out = static_cast<float>(per_sample * samples_per_hour);
  return true;
}

bool voltage_is_falling(const int* ordered_millivolts, int count,
                        int seconds_per_sample) {
  float per_hour = 0.0f;
  if (!battery_voltage_slope(ordered_millivolts, count, seconds_per_sample,
                             &per_hour)) {
    return false;
  }
  return per_hour < kDischargeSlopeMillivoltsPerHour;
}

bool voltage_is_rising(const int* ordered_millivolts, int count,
                       int seconds_per_sample) {
  float per_hour = 0.0f;
  if (!battery_voltage_slope(ordered_millivolts, count, seconds_per_sample,
                             &per_hour)) {
    return false;
  }
  return per_hour > kChargingRiseMillivoltsPerHour;
}

bool battery_overvoltage_warning(int millivolts) {
  return millivolts >= kBatteryOvervoltageWarningMillivolts;
}

bool battery_overvoltage_danger(int millivolts) {
  return millivolts >= kBatteryOvervoltageDangerMillivolts;
}

AppSnapshot make_mock_snapshot(DemoScenario scenario) {
  AppSnapshot snapshot;
  snapshot.clock = {"09:41", "Sat, 15 Aug 2026", "Clock Hero"};
  snapshot.ua_fx = ua_fx_market();
  snapshot.us_market = us_market();
  snapshot.weather = taipei_weather();
  snapshot.new_york_weather = new_york_weather();
  // valid stays false here and in every other builder: these figures are
  // layout fixtures, not readings, and the UI must show a NO DATA placeholder
  // until a real provider fills them in. Nothing on this snapshot may reach
  // the panel as though it were measured.
  snapshot.indoor = indoor();
  snapshot.availability = {};
#ifdef APP_CORE_DEMO_MISSING_PAGE
  snapshot.availability.weather = false;
#endif
  snapshot.scenario = scenario;
  return snapshot;
}

}  // namespace app_core
