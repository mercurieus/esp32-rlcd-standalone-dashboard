#include "tray_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif

namespace app_core {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "tray_registry";
#endif

// Both booleans are atomic, not because this is a hot path, but because
// register_tray_indicator() (called once, at a module's init) and
// tray_indicator_slot() (called every ~100ms from the LVGL task) run on
// different tasks with no other synchronization between them, and
// set_tray_indicator_active() is explicitly documented as callable from
// any task. Default std::atomic operations (sequentially consistent) are
// simple to reason about and cheap enough at this frequency - no need to
// hand-roll a weaker memory order for a handful of flag flips a second.
constexpr int64_t kNever = INT64_MIN;

struct Slot {
  std::atomic<bool> registered{false};
  std::atomic<bool> active{false};
  // Monotonic ms of the last active -> inactive edge; meaningful only after
  // one happened (kNever until then).
  std::atomic<int64_t> went_inactive_ms{kNever};
  TrayIndicatorBitmap bitmap;
};

Slot g_slots[kMaxTrayIndicators];

// -1 = use the real monotonic clock; anything else is a test's fake "now".
std::atomic<int64_t> g_test_now_ms{-1};

int64_t now_ms() {
  const int64_t fake = g_test_now_ms.load();
  if (fake >= 0) return fake;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TrayIndicatorHandle register_tray_indicator(const TrayIndicatorBitmap& bitmap) {
  if (bitmap.pixels == nullptr || bitmap.width != kTrayIconSize ||
      bitmap.height != kTrayIconSize || bitmap.byte_count != kTrayIconBitmapBytes) {
    return TrayIndicatorHandle{};
  }

  for (int i = 0; i < kMaxTrayIndicators; ++i) {
    if (g_slots[i].registered.load()) continue;
    // bitmap written before registered flips true, so a reader that
    // observes registered==true is guaranteed to see the real bitmap, not
    // a half-written one.
    g_slots[i].bitmap = bitmap;
    g_slots[i].active.store(false);
    g_slots[i].went_inactive_ms.store(kNever);
    g_slots[i].registered.store(true);
    return TrayIndicatorHandle{static_cast<int8_t>(i)};
  }
  return TrayIndicatorHandle{};  // full
}

void set_tray_indicator_active(TrayIndicatorHandle handle, bool active) {
  if (!handle.valid() || handle.slot >= kMaxTrayIndicators) {
    // Loud, not a quiet return: an unwired or invalid handle reaching here
    // is a caller bug (a handle captured before registration, or one from a
    // registry that was full), and it must not look identical in the log to
    // a genuine, successful no-op. See modules/audio/README.md's "activation
    // never happens" section for the failure mode this exists to make loud.
#ifdef ESP_PLATFORM
    ESP_LOGW(kTag, "set_tray_indicator_active ignored: invalid handle (slot=%d)",
             static_cast<int>(handle.slot));
#endif
    return;
  }
  // Stamp before the flag drops, so a reader that sees "inactive" also sees
  // when it went inactive and never a stale "never".
  if (!active && g_slots[handle.slot].active.load()) {
    g_slots[handle.slot].went_inactive_ms.store(now_ms());
  }
  g_slots[handle.slot].active.store(active);
}

bool tray_any_active_within(int64_t grace_ms) {
  const int64_t now = now_ms();
  for (int i = 0; i < kMaxTrayIndicators; ++i) {
    if (!g_slots[i].registered.load()) continue;
    if (g_slots[i].active.load()) return true;
    const int64_t gone = g_slots[i].went_inactive_ms.load();
    if (gone != kNever && now - gone < grace_ms) return true;
  }
  return false;
}

void set_tray_clock_for_test(int64_t now_ms_value) {
  g_test_now_ms.store(now_ms_value);
}

TrayIndicatorSlot tray_indicator_slot(int index) {
  if (index < 0 || index >= kMaxTrayIndicators || !g_slots[index].registered.load()) {
    return TrayIndicatorSlot{};
  }
  return TrayIndicatorSlot{true, g_slots[index].active.load(), g_slots[index].bitmap};
}

void reset_tray_registry_for_test() {
  for (int i = 0; i < kMaxTrayIndicators; ++i) {
    g_slots[i].registered.store(false);
    g_slots[i].active.store(false);
    g_slots[i].went_inactive_ms.store(kNever);
    g_slots[i].bitmap = TrayIndicatorBitmap{};
  }
}

}  // namespace app_core
