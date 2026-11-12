#include "restart_hook.hpp"

#include <atomic>

namespace app_core {

namespace {
// Atomics: the OTA task, the esp_timer task and an HTTP handler can each reach
// a restart, and the exchange below is what makes "once" true across them.
std::atomic<void (*)()> g_hook{nullptr};
std::atomic<bool> g_ran{false};
}  // namespace

void set_before_restart_hook(void (*hook)()) {
  g_hook.store(hook);
  g_ran.store(false);
}

void run_before_restart() {
  if (g_ran.exchange(true)) return;
  void (*hook)() = g_hook.load();
  if (hook != nullptr) hook();
}

}  // namespace app_core
