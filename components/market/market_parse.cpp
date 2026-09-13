#include "market_parse.hpp"

#include "cJSON.h"

#include <cmath>

namespace market {
namespace {

// Days-to-civil, Howard Hinnant's algorithm. UTC rather than the exchange's
// own zone: the response gives seconds since the epoch and the timezone name
// separately, and quietly applying one to the other would turn a reported fact
// into a computed guess. A close is dated by its UTC day here, which is the
// same calendar day as New York's for any regular session.
void civil_from_unix(long long seconds, uint16_t& year, uint8_t& month,
                     uint8_t& day) {
  long long z = seconds / 86400 + 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned long long doe = static_cast<unsigned long long>(z - era * 146097);
  const unsigned long long yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long y = static_cast<long long>(yoe) + era * 400;
  const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned long long mp = (5 * doy + 2) / 153;
  const unsigned long long d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned long long m = mp + (mp < 10 ? 3 : -9);
  year = static_cast<uint16_t>(y + (m <= 2 ? 1 : 0));
  month = static_cast<uint8_t>(m);
  day = static_cast<uint8_t>(d);
}

}  // namespace

namespace {

// Extracts a JSON string field, failing closed on anything else (missing
// key, null, non-string, empty).
bool string_field(const cJSON* object, const char* key, std::string& out) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
  out = item->valuestring;
  return true;
}

}  // namespace

// See this function's own doc comment in market_parse.hpp for why it
// exists (naive stride sampling drops a spike; this does not) and its
// precondition (raw_count > out.size()). External linkage (not in the
// anonymous namespace above) on purpose: declared in market_parse.hpp so
// the host test suite can exercise it directly.
void reduce_to_extremes(const double* raw, std::size_t raw_count,
                        std::array<int, app_core::kIntradaySampleCount>& out) {
  const std::size_t n = out.size();
  // Deviation from the *previous bucket's own chosen value*, not from
  // this bucket's own mean: for a bucket of exactly two raw points (the
  // common case once raw_count is only modestly above n, e.g. the real
  // ~79 US bars into 64 slots), both points are by definition equidistant
  // from their own two-point average - "deviation from bucket mean" is an
  // unbreakable tie there, which silently drops whichever candidate does
  // not happen to be checked first. Comparing against the running series
  // instead - literally "furthest from the neighbour that precedes it" -
  // has no such blind spot: a spike is far from the flat value before it
  // regardless of what shares its own bucket.
  double previous = raw[0];
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t start = (i * raw_count) / n;
    std::size_t end = ((i + 1) * raw_count) / n;
    if (end <= start) end = start + 1;
    if (end > raw_count) end = raw_count;

    double best_value = raw[start];
    double best_deviation = -1.0;
    for (std::size_t j = start; j < end; ++j) {
      const double deviation = std::fabs(raw[j] - previous);
      if (deviation > best_deviation) {
        best_deviation = deviation;
        best_value = raw[j];
      }
    }
    out[i] = static_cast<int>(std::lround(best_value));
    previous = best_value;
  }
}

bool parse_nbu_rates(const char* json, std::size_t length,
                     app_core::MarketData& out) {
  out = app_core::MarketData{};

  cJSON* root = cJSON_ParseWithLength(json, length);
  if (root == nullptr) return false;

  bool ok = false;
  do {
    if (!cJSON_IsArray(root)) break;  // unexpected shape: not a row list.

    double usd_rate = 0.0, eur_rate = 0.0;
    bool have_usd = false, have_eur = false;
    uint16_t as_of_year = 0;
    uint8_t as_of_month = 0;
    uint8_t as_of_day = 0;

    const cJSON* row = nullptr;
    cJSON_ArrayForEach(row, root) {
      if (!cJSON_IsObject(row)) continue;
      std::string code;
      if (!string_field(row, "cc", code)) continue;
      const cJSON* rate_item = cJSON_GetObjectItemCaseSensitive(row, "rate");
      if (!cJSON_IsNumber(rate_item)) continue;

      // "13.09.2026" - DD.MM.YYYY, the same field on every row for a given
      // request, so the first row that has it names the whole response's
      // date.
      std::string date_text;
      if (as_of_year == 0 && string_field(row, "exchangedate", date_text) &&
          date_text.size() == 10 && date_text[2] == '.' &&
          date_text[5] == '.') {
        const int day = (date_text[0] - '0') * 10 + (date_text[1] - '0');
        const int month = (date_text[3] - '0') * 10 + (date_text[4] - '0');
        const int year = (date_text[6] - '0') * 1000 +
                         (date_text[7] - '0') * 100 +
                         (date_text[8] - '0') * 10 + (date_text[9] - '0');
        if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
          as_of_year = static_cast<uint16_t>(year);
          as_of_month = static_cast<uint8_t>(month);
          as_of_day = static_cast<uint8_t>(day);
        }
      }

      if (code == "USD" && !have_usd) {
        usd_rate = rate_item->valuedouble;
        have_usd = true;
      } else if (code == "EUR" && !have_eur) {
        eur_rate = rate_item->valuedouble;
        have_eur = true;
      }
    }
    if (!have_usd || !have_eur) break;

    app_core::MarketData parsed;
    parsed.display_name = "UA EXCHANGE RATE";
    parsed.primary_label = "USD/UAH";
    // Hundredths, not whole UAH - see value_has_decimals's own comment in
    // app_snapshot.hpp for why (44.55 -> 4455, rendered back with a decimal
    // point).
    parsed.primary_value = static_cast<int>(std::lround(usd_rate * 100.0));
    parsed.secondary_label = "EUR/UAH";
    parsed.secondary_value = static_cast<int>(std::lround(eur_rate * 100.0));
    parsed.value_has_decimals = true;
    parsed.as_of_year = as_of_year;
    parsed.as_of_month = as_of_month;
    parsed.as_of_day = as_of_day;
    parsed.valid = true;
    // primary/secondary_change_percent and has_change are left at their
    // defaults (0.0 / true): a single day's response has no notion of
    // "change" on its own. market.cpp's refresh_ua_fx() - which calls this
    // twice, for today and yesterday - fills those in once it has both.

    out = parsed;
    ok = true;
  } while (false);

  cJSON_Delete(root);
  return ok;
}

bool parse_yahoo_quote(const char* json, std::size_t length,
                        const std::string& display_label, IndexQuote& out) {
  out = IndexQuote{};

  cJSON* root = cJSON_ParseWithLength(json, length);
  if (root == nullptr) return false;

  bool ok = false;
  do {
    const cJSON* chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    if (!cJSON_IsObject(chart)) break;  // unexpected shape.

    // Yahoo's documented failure response for a bad/delisted symbol or a
    // declined request is {"chart":{"result":null,"error":{...}}} - treat
    // any non-null error object as authoritative failure regardless of
    // what `result` looks like.
    const cJSON* error = cJSON_GetObjectItemCaseSensitive(chart, "error");
    if (error != nullptr && !cJSON_IsNull(error)) break;

    const cJSON* results = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) < 1) break;
    const cJSON* result0 = cJSON_GetArrayItem(results, 0);
    if (!cJSON_IsObject(result0)) break;

    const cJSON* meta = cJSON_GetObjectItemCaseSensitive(result0, "meta");
    if (!cJSON_IsObject(meta)) break;
    uint16_t as_of_year = 0;
    uint8_t as_of_month = 0;
    uint8_t as_of_day = 0;
    const cJSON* market_time =
        cJSON_GetObjectItemCaseSensitive(meta, "regularMarketTime");
    if (cJSON_IsNumber(market_time) && market_time->valuedouble > 0) {
      civil_from_unix(static_cast<long long>(market_time->valuedouble),
                      as_of_year, as_of_month, as_of_day);
    }
    const cJSON* price_item =
        cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
    const cJSON* prev_item =
        cJSON_GetObjectItemCaseSensitive(meta, "previousClose");
    if (!cJSON_IsNumber(price_item) || !cJSON_IsNumber(prev_item)) break;
    const double price = price_item->valuedouble;
    const double previous_close = prev_item->valuedouble;
    if (previous_close == 0.0) break;  // would divide by zero below.

    // Intraday series: best-effort. Missing/malformed/short does not fail
    // the quote - price/change above already satisfied the required part.
    std::array<int, app_core::kIntradaySampleCount> samples{};
    uint8_t sample_count = 0;
    bool have_intraday = false;
    const cJSON* indicators =
        cJSON_GetObjectItemCaseSensitive(result0, "indicators");
    const cJSON* quotes = cJSON_IsObject(indicators)
                               ? cJSON_GetObjectItemCaseSensitive(indicators,
                                                                   "quote")
                               : nullptr;
    const cJSON* quote0 = (cJSON_IsArray(quotes) &&
                            cJSON_GetArraySize(quotes) >= 1)
                              ? cJSON_GetArrayItem(quotes, 0)
                              : nullptr;
    const cJSON* closes = cJSON_IsObject(quote0)
                               ? cJSON_GetObjectItemCaseSensitive(quote0,
                                                                   "close")
                               : nullptr;
    if (cJSON_IsArray(closes)) {
      // Bounded scratch buffer, not a heap vector: comfortably above the
      // most raw bars any (symbol, interval, range) this component
      // actually requests can produce - a hostile/garbled response is
      // simply capped rather than chased. This is scratch space for the
      // *raw* series; app_core::kIntradaySampleCount is the unrelated,
      // much smaller *output* resolution target.
      constexpr std::size_t kMaxRawPoints = 128;
      std::array<double, kMaxRawPoints> valid_closes{};
      std::size_t valid_count = 0;
      const cJSON* point = nullptr;
      cJSON_ArrayForEach(point, closes) {
        if (valid_count >= kMaxRawPoints) break;
        if (cJSON_IsNumber(point)) valid_closes[valid_count++] = point->valuedouble;
      }
      if (valid_count >= kMinIntradayPoints) {
        if (valid_count <= samples.size()) {
          // Fewer real bars than the chart's own resolution target -
          // nothing to reduce, and nothing to pad the remaining slots
          // with either. One raw point per output slot, in order.
          for (std::size_t i = 0; i < valid_count; ++i) {
            samples[i] = static_cast<int>(std::lround(valid_closes[i]));
          }
          sample_count = static_cast<uint8_t>(valid_count);
        } else {
          reduce_to_extremes(valid_closes.data(), valid_count, samples);
          sample_count = static_cast<uint8_t>(samples.size());
        }
        have_intraday = true;
      }
    }
    if (!have_intraday) {
      samples.fill(static_cast<int>(std::lround(price)));
    }

    // session_elapsed_fraction: best-effort, from this response's own
    // session-bounds metadata and its own last timestamp - see
    // app_core::MarketData's comment for the full reasoning. Left at the
    // IndexQuote default (1.0) if any of this is missing; never fails the
    // quote.
    float session_elapsed_fraction = 1.0f;
    long long session_start = 0;
    const cJSON* trading_period =
        cJSON_GetObjectItemCaseSensitive(meta, "currentTradingPeriod");
    const cJSON* regular_period =
        cJSON_IsObject(trading_period)
            ? cJSON_GetObjectItemCaseSensitive(trading_period, "regular")
            : nullptr;
    const cJSON* period_start =
        cJSON_IsObject(regular_period)
            ? cJSON_GetObjectItemCaseSensitive(regular_period, "start")
            : nullptr;
    const cJSON* period_end =
        cJSON_IsObject(regular_period)
            ? cJSON_GetObjectItemCaseSensitive(regular_period, "end")
            : nullptr;
    if (cJSON_IsNumber(period_start) && cJSON_IsNumber(period_end) &&
        period_end->valuedouble > period_start->valuedouble) {
      // Reported as-is, whether or not the elapsed-fraction arithmetic
      // below finds a timestamp to work with: the next-refresh decision
      // needs the session's start even from a response taken before the
      // session has produced a single bar.
      session_start = static_cast<long long>(period_start->valuedouble);
      const cJSON* timestamps =
          cJSON_GetObjectItemCaseSensitive(result0, "timestamp");
      double last_timestamp = -1.0;
      if (cJSON_IsArray(timestamps)) {
        const cJSON* stamp = nullptr;
        cJSON_ArrayForEach(stamp, timestamps) {
          if (cJSON_IsNumber(stamp)) last_timestamp = stamp->valuedouble;
        }
      }
      if (last_timestamp >= 0.0) {
        const double start = period_start->valuedouble;
        const double end = period_end->valuedouble;
        if (last_timestamp <= start || last_timestamp >= end) {
          // Not actively in this session - a completed prior session or a
          // finished current one, either way not partial. See this
          // field's own comment for why that is 1.0, not 0.0.
          session_elapsed_fraction = 1.0f;
        } else {
          session_elapsed_fraction =
              static_cast<float>((last_timestamp - start) / (end - start));
        }
      }
    }

    IndexQuote parsed;
    parsed.has_intraday = have_intraday;
    parsed.as_of_year = as_of_year;
    parsed.as_of_month = as_of_month;
    parsed.as_of_day = as_of_day;
    parsed.valid = true;
    parsed.label = display_label;
    parsed.value = static_cast<int>(std::lround(price));
    parsed.change_percent = (price - previous_close) / previous_close * 100.0;
    parsed.samples = samples;
    parsed.sample_count = sample_count;
    parsed.session_elapsed_fraction = session_elapsed_fraction;
    parsed.session_start = session_start;

    out = parsed;
    ok = true;
  } while (false);

  cJSON_Delete(root);
  return ok;
}

}  // namespace market
