#include "market_schedule.hpp"

#include "market.hpp"

namespace market {
namespace {

// Floor on the shortened pre-open sleep. A wake-up seconds from now buys
// nothing the next one would not, and a task that sleeps ~0 between two
// pairs of HTTPS requests is a battery drain and a way to get rate-limited.
constexpr int kMinSleepSeconds = 60;

}  // namespace

int us_refresh_interval_seconds(long long now_epoch, long long session_start) {
  // No clock, or a response that did not date its session: the flat
  // interval, which is what this page did before any of this existed.
  if (now_epoch <= 0 || session_start <= 0) return kRefreshIntervalSeconds;
  const long long until_warm =
      session_start + kOpenWarmupSeconds - now_epoch;
  // Already past the open (mid-session, or any time after the close, since
  // the source keeps reporting a session start until it rolls to the next
  // one) - nothing to align to.
  if (until_warm <= 0) return kRefreshIntervalSeconds;
  // Further out than one ordinary interval - including the whole of a
  // weekend or an overnight - is also nothing to align to yet. Sleeping
  // straight through to a distant open would mean trusting one number from
  // one response with hours of blackout; the ordinary interval gets there
  // in steps, and the last of those steps is the one that lands short.
  if (until_warm >= kRefreshIntervalSeconds) return kRefreshIntervalSeconds;
  return static_cast<int>(until_warm < kMinSleepSeconds ? kMinSleepSeconds
                                                        : until_warm);
}

}  // namespace market
