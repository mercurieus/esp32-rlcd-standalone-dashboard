#pragma once

// One place every firmware-initiated esp_restart() calls first, so the board
// can say goodbye (the farewell melody, spec section 6) without the OTA and
// portal components knowing sound exists. They depend on app_core already;
// main/ registers the hook, because main/ is the only layer that may reach the
// audio module (modules/README.md rule 4).
//
// Not for panics, aborts or watchdog resets: those do not pass through any
// code of ours, and a farewell from a crashed task would be worse than silence.
namespace app_core {

// Registers the function run_before_restart() calls (nullptr unregisters).
// Registering also re-arms run_before_restart(), so a test - or a later boot
// stage that swaps the hook - gets one run of the new hook.
void set_before_restart_hook(void (*hook)());

// Runs the registered hook, once: a second call (the OTA task and the portal's
// timer can both reach a restart) is a no-op. Safe with no hook registered.
// May block for the hook's duration - the farewell is under 1 s.
void run_before_restart();

}  // namespace app_core
