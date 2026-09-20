#include "market.hpp"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

namespace market {
namespace {

constexpr char kTag[] = "market";
constexpr int kHttpTimeoutMs = 8000;

// Bounded response buffers - an unbounded read of a remote body into a heap
// allocation is a defect even with plenty of PSRAM to spare. NBU's
// all-currencies response lists every currency it publishes (~50 rows) in
// one response - a few tens of KB, the same shape as TWSE's MI_INDEX this
// buffer size was originally sized against. Yahoo's 1-day/5-minute chart
// response for one symbol was ~7 KB live at 15-minute bars and stays well
// inside this cap at 5-minute ones too - a day's worth of extra
// timestamp/close entries is a few KB, not an order of magnitude.
// Both caps below are generous multiples of that, not "as much as fits": a
// response that blows the cap is simply truncated, and a truncated body
// fails to parse (see market_parse.cpp) rather than being accepted
// partially.
constexpr int kNbuBufferBytes = 96 * 1024;
constexpr int kYahooBufferBytes = 32 * 1024;

app_core::MarketData g_ua_fx;  // valid == false until the first success.
app_core::MarketData g_us;
// See us_session_start() below.
long long g_us_session_start = 0;

// GETs `url`, heap-allocating up to `max_bytes` for the body (never on the
// caller's stack - MI_INDEX alone is tens of KB). Every esp_err_t and the
// HTTP status are logged; on a non-200 status the start of the body is
// logged too, so a serial capture explains a NO DATA page without
// guesswork.
bool http_get(const char* url, int max_bytes, std::string& out_body) {
  out_body.clear();

  esp_http_client_config_t config = {};
  config.url = url;
  config.timeout_ms = kHttpTimeoutMs;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(kTag, "esp_http_client_init failed for %s", url);
    return false;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "open failed for %s: %s", url, esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }

  const int64_t content_length = esp_http_client_fetch_headers(client);
  if (content_length < -1) {
    ESP_LOGW(kTag, "fetch_headers failed for %s", url);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  std::string buffer(static_cast<std::size_t>(max_bytes), '\0');
  const int read = esp_http_client_read_response(client, buffer.data(), max_bytes);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (read < 0) {
    ESP_LOGW(kTag, "read failed for %s", url);
    return false;
  }
  if (status != 200) {
    ESP_LOGW(kTag, "%s returned HTTP %d: %.*s", url, status,
             std::min(read, 200), buffer.data());
    return false;
  }

  buffer.resize(static_cast<std::size_t>(read));
  out_body = std::move(buffer);
  return true;
}

// NBU's own base URL: no valcode means "every currency it publishes", and
// an explicit date=YYYYMMDD asks for that specific day's rate rather than
// the latest one (see market_parse.hpp's parse_nbu_rates() for the
// response shape both calls share).
constexpr char kNbuBaseUrl[] =
    "https://bank.gov.ua/NBUStatService/v1/statdirectory/exchange";

// A different NBU service for the history, because the one above has no
// history to give: statdirectory/exchange ignores `start`/`end` entirely and
// answers with the latest day whatever is asked. Verified twice against the
// live endpoint, once without `valcode` and once with; both returned a single
// row. NBU_Exchange/exchange_site honours the range and returns one row per
// calendar day.
constexpr char kNbuSeriesUrl[] = "https://bank.gov.ua/NBU_Exchange/exchange_site";

// Thirty days of it: 31 points into the chart's 64 slots, and a response of
// about 5 KB.
//
// One row per calendar day, not per banking day - checked against the live
// service, which answered a 31-day span with 31 rows. NBU carries the last
// published rate across weekends itself, so the flat Saturday and Sunday on
// the chart are the provider's own statement about those days rather than
// anything invented here.
constexpr int kNbuSeriesDays = 30;


// Both of NBU's history requests below are parameterised by a calendar date,
// which makes them meaningless until the clock is real. This board has no RTC
// battery, so every power-on starts at the epoch and stays there until SNTP
// lands - and the monitor task waits only for an IP, which arrives first.
//
// Asked for a 1970 range the service does not fail: it answers 200 with an
// empty array (verified live - six bytes, "[]"). So the series fetch appeared
// to succeed, parsed to zero points, and the page said NO INTRADAY DATA for a
// full hour, because today's rates had parsed fine and the task slept its
// success interval.
//
// The floor is 2020-01-01 UTC: far past any plausible epoch-start garbage and
// far below any date this firmware will really see.
constexpr std::time_t kCredibleClockFloor = 1577836800;

bool clock_is_credible() {
  return std::time(nullptr) >= kCredibleClockFloor;
}

// True when the last refresh left the page missing something it could still
// get later - see ua_fx_incomplete() in market.hpp.
bool g_ua_fx_incomplete = false;

}  // namespace

bool refresh_ua_fx() {
  std::string today_body;
  app_core::MarketData today;
  char today_url[96];
  std::snprintf(today_url, sizeof(today_url), "%s?json", kNbuBaseUrl);
  if (!http_get(today_url, kNbuBufferBytes, today_body) ||
      !parse_nbu_rates(today_body.data(), today_body.size(), today)) {
    ESP_LOGW(kTag, "NBU today fetch/parse failed (%zu bytes)",
             today_body.size());
    g_ua_fx = app_core::MarketData{};
    return false;
  }

  // Yesterday's calendar date, UTC rather than the device's own local
  // timezone - the same reason market_parse.cpp's civil_from_unix() stays
  // in UTC: NBU's rate never changes intraday, so being off by the few
  // hours between UTC midnight and Kyiv midnight costs nothing here, and
  // this avoids coupling this component to net_time's TZ state.
  const bool clock_ready = clock_is_credible();
  g_ua_fx_incomplete = !clock_ready;
  if (!clock_ready) {
    ESP_LOGW(kTag,
             "clock not yet synced; skipping NBU history (a date range asked "
             "at the epoch returns an empty array, not an error)");
  }

  const std::time_t yesterday_epoch = std::time(nullptr) - 24 * 60 * 60;
  std::tm yesterday_tm{};
  gmtime_r(&yesterday_epoch, &yesterday_tm);
  char yesterday_url[96];
  std::snprintf(yesterday_url, sizeof(yesterday_url), "%s?date=%04d%02d%02d&json",
               kNbuBaseUrl, yesterday_tm.tm_year + 1900,
               yesterday_tm.tm_mon + 1, yesterday_tm.tm_mday);

  std::string yesterday_body;
  app_core::MarketData yesterday;
  const bool have_yesterday =
      clock_ready &&
      http_get(yesterday_url, kNbuBufferBytes, yesterday_body) &&
      parse_nbu_rates(yesterday_body.data(), yesterday_body.size(), yesterday);
  if (!have_yesterday) {
    ESP_LOGW(kTag,
             "NBU yesterday fetch/parse failed; publishing today's rate "
             "with no change figure");
  }

  // Thirty days of USD closes for the chart. Failure is not fatal and not
  // faked: has_intraday stays false, the page says it has no series, and
  // today's rates - which are real and already parsed - still render.
  const std::time_t series_start_epoch =
      std::time(nullptr) - static_cast<std::time_t>(kNbuSeriesDays) * 24 * 60 * 60;
  std::tm start_tm{};
  gmtime_r(&series_start_epoch, &start_tm);
  const std::time_t now_epoch = std::time(nullptr);
  std::tm end_tm{};
  gmtime_r(&now_epoch, &end_tm);
  char series_url[192];
  std::snprintf(series_url, sizeof(series_url),
                "%s?start=%04d%02d%02d&end=%04d%02d%02d&valcode=usd"
                "&sort=exchangedate&order=asc&json",
                kNbuSeriesUrl, start_tm.tm_year + 1900, start_tm.tm_mon + 1,
                start_tm.tm_mday, end_tm.tm_year + 1900, end_tm.tm_mon + 1,
                end_tm.tm_mday);
  std::string series_body;
  if (clock_ready && http_get(series_url, kNbuBufferBytes, series_body) &&
      parse_nbu_daily_series(series_body.data(), series_body.size(), today)) {
    ESP_LOGI(kTag, "NBU series ok points=%u %02u.%02u-%02u.%02u",
             static_cast<unsigned>(today.intraday_sample_count),
             static_cast<unsigned>(today.series_first_day),
             static_cast<unsigned>(today.series_first_month),
             static_cast<unsigned>(today.series_last_day),
             static_cast<unsigned>(today.series_last_month));
  } else if (clock_ready) {
    g_ua_fx_incomplete = true;
    ESP_LOGW(kTag, "NBU series fetch/parse failed (%zu bytes); no chart",
             series_body.size());
  }

  // See has_change's own comment in app_snapshot.hpp: without yesterday's
  // rate there is no honest change to report, so the field stays false
  // rather than printing a fabricated "+0.00%".
  today.has_change = have_yesterday;
  if (have_yesterday) {
    today.primary_change_percent =
        (today.primary_value - yesterday.primary_value) * 100.0 /
        yesterday.primary_value;
    today.secondary_change_percent =
        (today.secondary_value - yesterday.secondary_value) * 100.0 /
        yesterday.secondary_value;
  }

  g_ua_fx = today;
  return true;
}

bool refresh_us() {
  std::string sp500_body;
  if (!http_get(
          "https://query1.finance.yahoo.com/v8/finance/chart/%5EGSPC"
          "?interval=5m&range=1d",
          kYahooBufferBytes, sp500_body)) {
    g_us = app_core::MarketData{};
    return false;
  }
  IndexQuote primary;
  if (!parse_yahoo_quote(sp500_body.data(), sp500_body.size(), "S&P 500",
                          primary)) {
    ESP_LOGW(kTag, "S&P 500 chart body did not parse (%zu bytes)",
             sp500_body.size());
    g_us = app_core::MarketData{};
    return false;
  }

  std::string nasdaq_body;
  if (!http_get(
          "https://query1.finance.yahoo.com/v8/finance/chart/%5EIXIC"
          "?interval=5m&range=1d",
          kYahooBufferBytes, nasdaq_body)) {
    g_us = app_core::MarketData{};
    return false;
  }
  IndexQuote secondary;
  if (!parse_yahoo_quote(nasdaq_body.data(), nasdaq_body.size(), "NASDAQ",
                          secondary)) {
    ESP_LOGW(kTag, "NASDAQ chart body did not parse (%zu bytes)",
             nasdaq_body.size());
    g_us = app_core::MarketData{};
    return false;
  }

  app_core::MarketData parsed;
  parsed.display_name = "US MARKET";
  parsed.has_intraday = primary.has_intraday;
  // The primary index dates the page: both quotes come from the same session,
  // and taking it from one of them keeps this a reported fact rather than a
  // reconciliation of two.
  parsed.as_of_year = primary.as_of_year;
  parsed.as_of_month = primary.as_of_month;
  parsed.as_of_day = primary.as_of_day;
  parsed.primary_label = primary.label;
  parsed.primary_value = primary.value;
  parsed.primary_change_percent = primary.change_percent;
  parsed.secondary_label = secondary.label;
  parsed.secondary_value = secondary.value;
  parsed.secondary_change_percent = secondary.change_percent;
  parsed.intraday_samples = primary.samples;
  parsed.intraday_sample_count = primary.sample_count;
  // Same reasoning as the as_of date above: both quotes are the same
  // session, so the primary's own value is the reported fact, not a
  // reconciliation.
  parsed.session_elapsed_fraction = primary.session_elapsed_fraction;
  parsed.valid = true;

  g_us = parsed;
  g_us_session_start = primary.session_start;
  return true;
}

// Gated on g_us.valid rather than reset on each of refresh_us()'s four
// failure paths: one condition cannot be forgotten by a fifth one added
// later, and a scheduler must never be handed a session boundary from a
// snapshot that is no longer on screen.
long long us_session_start() { return g_us.valid ? g_us_session_start : 0; }

app_core::MarketData ua_fx() { return g_ua_fx; }

bool ua_fx_incomplete() { return g_ua_fx_incomplete; }
app_core::MarketData us() { return g_us; }

}  // namespace market
