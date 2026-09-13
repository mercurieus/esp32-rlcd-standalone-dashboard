#pragma once

#include "app_snapshot.hpp"

// Pure "when to refresh" scheduling policy for the market/rate pages - no
// I/O, no ESP-IDF, host-testable like market_parse.hpp. Kept separate from
// it: that file is about what a response means, this one is about how
// often to ask for one.
namespace market {

// How long after an open the first in-session refresh should land -
// us_refresh_interval_seconds() below shortens a pre-open sleep by epoch
// arithmetic so a refresh lands this long after the open instead of on it.
//
// Not zero, for a reason visible in a real response taken 12 minutes into
// a session: the intraday series is the session's completed 5-minute bars
// plus one partial bar at the current time. At the open itself that is a
// single point, fewer than market_parse.hpp's kMinIntradayPoints, so the
// page would draw no chart and print "CLOSE <today>" over "NO INTRADAY
// DATA" - a market that just opened, rendered as a closed one, which is
// the exact failure this whole path exists to stop. Five minutes in there
// are at least two points, which is a line.
inline constexpr int kOpenWarmupSeconds = 5 * 60;

// Seconds to sleep before the next US refresh.
//
// `now_epoch` is the device's current time in epoch seconds (0 when it has
// no trustworthy clock - the caller must not guess one), and
// `session_start` is market.hpp's us_session_start(), the exchange's own
// regular-session start from the last response, on that same scale.
//
// Both being epoch seconds is what makes this possible at all. The US
// market is the DST-observing timezone this component still has no data
// for, and it needs none: two absolute instants compare directly. There
// is no exchange calendar here either - a holiday's "start" is simply
// whatever the source last reported, and being wrong about it costs one
// refresh landing at an ordinary time.
//
// The result is only ever kRefreshIntervalSeconds (market.hpp) or *less
// than* it, and only for the one sleep that would otherwise step over the
// open: this moves when a refresh lands, it never adds one. Off-hours,
// mid-session and unknown-clock all return the flat interval this page
// has always used, so the request budget - and the radio time a
// battery-powered panel pays for it - is exactly what it was before.
//
// Without this the flat interval is free to straddle the open, leaving the
// page showing the previous session, correctly dated and complete, for up
// to a full interval after this one began. That reads as "the market has
// not opened yet", which was the original complaint.
int us_refresh_interval_seconds(long long now_epoch, long long session_start);

// No session_elapsed_fraction() here (an earlier version derived one from
// session bounds and the board's own RTC). app_core::MarketData's own field
// of that name is computed instead in market_parse.cpp's parse_yahoo_quote(),
// from Yahoo's own meta.currentTradingPeriod.regular.start/end and the
// response's own last timestamp - both epoch seconds, verified present in a
// real response before this was built. That needs no device clock, no
// timezone, and no DST arithmetic (a genuine device-clock version would
// need US Eastern time's DST rules, which this component deliberately has
// no data for).

}  // namespace market
