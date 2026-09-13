#pragma once

#include "app_snapshot.hpp"
#include "market_parse.hpp"

// Fetch + refresh-policy layer on top of market_parse.hpp. This header pulls
// in ESP-IDF (via market.cpp) and is not part of the host test build; only
// market_parse.hpp/.cpp are host-tested. Mirrors components/weather's
// weather.hpp/weather_parse.hpp split.
//
// This component does not run its own task and does not publish into
// AppSnapshot - both are main/app_core's job. A caller there is expected to
// call refresh_ua_fx()/refresh_us() periodically (see
// kRefreshIntervalSeconds/kUaFxRefreshIntervalSeconds) from whatever task
// already owns network I/O, and to copy ua_fx()/us() into
// AppSnapshot::ua_fx / us_market on the LVGL thread.
namespace market {

// The baseline, flat interval - what US uses unconditionally. 30 minutes is
// at most 48 requests/day: respectful of the *unofficial* Yahoo endpoint,
// where aggressive polling is the surest way to get rate-limited or blocked
// outright. Not polled any faster during its own session than outside it -
// see market_schedule.hpp's us_refresh_interval_seconds(), which only moves
// *when* a refresh lands, never how many there are.
inline constexpr int kRefreshIntervalSeconds = 30 * 60;

// NBU publishes its official rate at most once per business day (and holds
// it through the weekend - see market.cpp's refresh_ua_fx()), so there is no
// "session" to poll faster during; hourly is already far more often than the
// value can change.
inline constexpr int kUaFxRefreshIntervalSeconds = 60 * 60;

// Blocking. Fetches bank.gov.ua's NBU exchange-rate directory twice - once
// for today's (i.e. the latest published) rate, once for yesterday's, both
// requests covering every currency the bank publishes in one response (see
// parse_nbu_rates() in market_parse.hpp) - and combines the USD/EUR rows
// from each into one MarketData with a real day-over-day change percent.
//
// On total failure, or if today's fetch/parse fails, the cache is set to
// invalid - never left at a stale prior value - so the next ua_fx() call
// reports valid == false and the UI shows NO DATA. If only yesterday's
// fetch/parse fails, today's data still publishes (it is real and
// complete on its own), but with has_change set false so the UI does not
// print a fabricated "+0.00%" for a comparison that was never made.
bool refresh_ua_fx();

// Blocking. Fetches both S&P 500 and NASDAQ from the Yahoo-Finance-style
// chart endpoint (two requests: this source answers one symbol per call).
// Both must succeed and parse for the US cache to become valid; if either
// fails, the whole cache is set to invalid, matching refresh_ua_fx()'s
// no-stale-data rule - a half-real, half-blank MarketData is not
// representable (there is one `valid` flag for the whole struct) and would
// not be honest anyway. Returns true on success.
bool refresh_us();

// meta.currentTradingPeriod.regular.start from the last successful
// refresh_us() - the epoch second the exchange itself said its regular
// session begins - or 0 when the last call failed or the response did not
// carry it. market_schedule.hpp's us_refresh_interval_seconds() needs it
// to land a refresh just after the open instead of up to a full interval
// past it. Refresh state the scheduler needs and the screen does not, so
// it stays out of MarketData.
long long us_session_start();

// Returns the current cached snapshot. No I/O; safe to call from any task,
// including the LVGL thread.
app_core::MarketData ua_fx();
app_core::MarketData us();

}  // namespace market
