#pragma once

#include "app_snapshot.hpp"

#include <array>
#include <cstddef>
#include <string>

// Pure JSON-parsing core: no ESP-IDF, no network, no globals. Kept separate
// from market.hpp (the fetch/refresh layer) so it can be exercised by the
// host test suite without pulling in esp_http_client. Mirrors the split used
// by components/weather (weather_parse.hpp vs weather.hpp).
namespace market {

// One index quote (price + change), independently of which MarketData slot
// (primary/secondary) it ends up in. Yahoo's chart endpoint answers one
// symbol per request, so the US fetch layer issues two requests (S&P 500,
// NASDAQ) and combines two IndexQuote results into one MarketData; NBU
// answers with every currency it publishes in a single call, so
// parse_nbu_rates() below fills a MarketData directly instead of going
// through this type.
struct IndexQuote {
  bool valid = false;
  std::string label;
  int value = 0;
  double change_percent = 0.0;
  // Real intraday close samples when the source supplied at least
  // kMinIntradayPoints (see parse_yahoo_quote); a flat repeat of `value`
  // otherwise. Never interpolated/invented points. Only the first
  // `sample_count` entries are meaningful - see that field.
  std::array<int, app_core::kIntradaySampleCount> samples{};
  // How many of the leading `samples` slots are real - may be fewer than
  // the array's own size early in a session (nothing pads the rest), or
  // exactly the array's size once enough raw points exist to fill it via
  // reduce_to_extremes(). 0 whenever has_intraday is false.
  uint8_t sample_count = 0;
  // The session these figures are from, from the source's own timestamp.
  // Zero when it did not supply one.
  uint16_t as_of_year = 0;
  uint8_t as_of_month = 0;
  uint8_t as_of_day = 0;
  // False when `samples` is a flat repeat of `value` rather than a real
  // series. The UI must not draw a chart in that case: the numbers are real
  // but the shape would not be.
  bool has_intraday = false;
  // See app_core::MarketData::session_elapsed_fraction's own comment for
  // the full reasoning - this is where it is actually computed, from this
  // response's own meta.currentTradingPeriod.regular.start/end and its
  // last timestamp. Default 1.0 (no shrink) whenever that metadata is
  // missing or the session is not actively in progress.
  float session_elapsed_fraction = 1.0f;
  // meta.currentTradingPeriod.regular.start, verbatim: the epoch second
  // this response's own exchange says its regular session begins. 0 when
  // the response did not carry it.
  //
  // Epoch seconds are the whole point - the same absolute scale the device
  // clock already runs on, so "has the US session begun" is a comparison
  // of two integers with no timezone, no DST rules, and no exchange
  // calendar anywhere in it (see market_schedule.hpp's
  // us_refresh_interval_seconds()). The response also names the zone
  // ("EDT") and its offset; both are deliberately ignored here for the
  // same reason civil_from_unix() ignores them in market_parse.cpp -
  // applying one to the other turns a reported fact into a computed guess.
  //
  // Not a MarketData field: nothing on screen shows it. It only decides
  // when to fetch again, which is market.hpp's us_session_start()'s job to
  // carry.
  long long session_start = 0;
};

// Parses an NBU (bank.gov.ua) /statdirectory/exchange response - a JSON
// array covering every currency the bank publishes a rate for on the
// requested date, one row per currency ({"cc":"USD","rate":44.55,...}) -
// into `out`, matching by "cc" the two rows the page needs: "USD" (primary)
// and "EUR" (secondary). On any malformed or truncated body, or if either
// row is absent or missing a required field, returns false and resets `out`
// to a freshly default-constructed MarketData (valid == false). A missing
// field is never defaulted to zero - the whole parse fails instead.
//
// `rate` (a decimal UAH-per-unit figure) is stored in `out` as hundredths -
// e.g. 44.55 becomes 4455 - via out.primary_value/secondary_value, with
// out.value_has_decimals set so the UI renders it back with a decimal
// point. NBU publishes one rate per day, not an intraday feed, so
// has_intraday stays false, matching the once-daily TWSE-style branch this
// mirrors. change_percent and as_of are left at their defaults; the caller
// (market.cpp's refresh_ua_fx(), which has both today's and yesterday's
// parsed rows) fills those in, since a single response has no notion of
// "change" on its own.
bool parse_nbu_rates(const char* json, std::size_t length,
                     app_core::MarketData& out);

// Fills out.intraday_samples with one close per banking day from NBU's
// period service (NBU_Exchange/exchange_site), oldest first, and sets
// series_is_daily plus the span's own first/last dates.
//
// A different service from the one parse_nbu_rates() reads, because the
// statdirectory/exchange endpoint ignores `start`/`end` - verified twice,
// once without `valcode` and once with, both returning only the latest day.
//
// Rates are stored in hundredths, matching value_has_decimals: the chart
// scales over its own min..max, and a month of NBU movement spans tens of
// hundredths, which is ample resolution for a polyline.
//
// Only rows that carry both a date and a rate are taken. A response with
// fewer than kMinIntradayPoints usable rows returns false and leaves
// `out` untouched: the page then says it has no series rather than drawing
// a shape from one or two points.
bool parse_nbu_daily_series(const char* json, std::size_t length,
                            app_core::MarketData& out);

// Smallest number of real raw points that can be drawn as a series -
// independent of app_core::kIntradaySampleCount, the chart's own *target*
// resolution: a session with fewer real bars than the target still
// deserves a chart (see parse_yahoo_quote's own comment on sample_count),
// it just is not downsampled.
//
// Two, because two is what a line is made of. This was 8, on the reasoning
// that fewer was "a couple of dots" rather than a series - but the US feed
// is 5-minute bars, so 8 of them is the first 40 minutes of every trading
// day, during which the page fell to the no-intraday branch and read
// "CLOSE <today>" / "NO INTRADAY DATA": a live, open market rendered as a
// closed one. A short line over the first few percent of the axis (the
// width render_market.cpp already scales by session_elapsed_fraction) is
// both true and legible; a threshold that blanks the chart is neither.
inline constexpr std::size_t kMinIntradayPoints = 2;

// Reduces `raw` (raw_count > out.size(), a precondition - the caller
// already knows to call this only once there are more raw points than
// output slots) to exactly out.size() points, one per contiguous bucket -
// bucket boundaries are the standard even i*raw_count/out.size() split, so
// every bucket gets at least one raw point. Each output slot is whichever
// raw point in its own bucket deviates most from the *previous* output
// slot's own chosen value - not the bucket's first, last, or middle point,
// and deliberately not the bucket's own mean either: a bucket of exactly
// two raw points (the common case once raw_count is only modestly above
// out.size(), e.g. this project's real ~79 US 5-minute bars into 64
// slots) has both points equidistant from their own two-point average by
// definition, which makes "deviates most from the bucket's own mean" an
// unbreakable tie that silently drops whichever point is not checked
// first. Comparing against the running series instead has no such blind
// spot: a spike is far from the flat value that precedes it regardless of
// what else shares its bucket.
//
// This is the fix for the actual complaint that motivated it: naive
// stride/index sampling (evenly picking one raw point per output slot,
// what this function replaces) drops whatever does not land on a kept
// index, including a real spike or dip - the shape ends up wrong, not just
// low-resolution.
void reduce_to_extremes(const double* raw, std::size_t raw_count,
                        std::array<int, app_core::kIntradaySampleCount>& out);

// Parses one Yahoo-Finance-style /v8/finance/chart/<symbol> response into a
// single IndexQuote. `display_label` is supplied by the caller (e.g.
// "S&P 500") rather than trusted from the response, since the caller
// already knows which symbol it requested.
//
// Required: chart.result[0].meta.regularMarketPrice and .previousClose,
// both numeric, with previousClose != 0. Any other shape - including the
// chart.error error-object response Yahoo returns for a bad/delisted symbol
// or when it declines the request - returns false with `out` reset to a
// default IndexQuote.
//
// chart.result[0].indicators.quote[0].close is read best-effort for the
// intraday series: fewer than kMinIntradayPoints usable points (missing,
// malformed, or the very first bar of a session) leaves has_intraday
// false and fills `samples` with kIntradaySampleCount copies of the
// current price, same as before. Otherwise has_intraday is true and
// sample_count is set to whichever is smaller - the raw point count itself
// (copied through unchanged, one raw point per output slot, when there are
// not yet enough real bars to fill the target resolution) or
// kIntradaySampleCount (via reduce_to_extremes() above, once there are
// more raw points than that). Neither branch interpolates or invents a
// point; this does not fail the whole quote either way - price/change
// already came from `meta`, which is the only part treated as required.
//
// session_elapsed_fraction (see app_core::MarketData's own comment) is
// read best-effort from meta.currentTradingPeriod.regular.start/.end and
// the response's own last timestamp - all optional, all epoch seconds, no
// effect on whether the quote itself succeeds.
bool parse_yahoo_quote(const char* json, std::size_t length,
                        const std::string& display_label, IndexQuote& out);

}  // namespace market
