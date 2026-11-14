#include "wifi_provision.hpp"

#include "internal.hpp"
#include "ui_app.hpp"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <optional>

namespace wifi_provision {
namespace {

constexpr char kTag[] = "wifi_provision";
constexpr char kPortalUrl[] = "http://192.168.4.1/";

// Bounds how long "Connecting" can sit without any Wi-Fi event before it's
// treated as a failed attempt. Association plus a DHCP lease normally
// finishes in a couple of seconds; 15s gives a slow/congested AP headroom
// without letting a silent hang (no event ever arriving - the exact way this
// bug was originally triggered) stall the retry loop forever.
constexpr uint64_t kConnectTimeoutUs = 15 * 1000 * 1000ULL;

app_core::AppSnapshot snapshot_;
std::optional<wifi_config::StateMachine> state_machine_;
std::string ap_ssid_;
#ifndef NDEBUG
// See debug_force_charging()'s own comment (wifi_provision.hpp).
bool g_debug_force_charging = false;
#endif
// Regenerated fresh on every entry into SetupAp; never written to NVS or
// logged. Gates the setup HTTP portal - the AP itself is open.
std::string portal_password_;
// The address from the last IP_EVENT_STA_GOT_IP. Never published directly:
// wifi_config::published_station_ip() gates it on the state, so it can stay
// stale here (a lease is not announced when it goes) without ever reaching the
// screen.
std::string sta_ip_;
// Why the last attempt failed, set when a failure moves the machine to
// Waiting or SetupAp; cleared on connect, gesture and credentials save.
// Drives the status text, and SetupData::error in SetupAp only - Waiting is
// not an error box, it is a board that will try again on its own.
DisconnectReason last_disconnect_reason_ = DisconnectReason::None;
// Two flags, not one: the setup hotspot and the HTTP server have different
// lifetimes (the server also runs while Connected, for firmware uploads), and
// sharing a single "portal active" flag is what once skipped starting the
// hotspot on a fallback to setup after a lost network. See plan_radio().
bool ap_active_ = false;
bool http_active_ = false;
// The state the last apply_state_and_publish() acted on, so the plan knows a
// state was just entered (fresh page password, retry timer armed once).
std::optional<wifi_config::StateMachine::State> applied_state_;
SemaphoreHandle_t mutex_ = nullptr;
esp_timer_handle_t connect_timeout_timer_ = nullptr;
// One-shot wait between attempts while Waiting (StateMachine::retry_delay_ms).
// The only periodic wakeup an outage adds: one per 5 s..5 min, nothing else.
esp_timer_handle_t retry_timer_ = nullptr;

void lock() { xSemaphoreTake(mutex_, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mutex_); }

const char* state_name(wifi_config::StateMachine::State state) {
  using State = wifi_config::StateMachine::State;
  switch (state) {
    case State::Connecting: return "Connecting";
    case State::Connected: return "Connected";
    case State::SetupAp: return "SetupAp";
    case State::Waiting: return "Waiting";
  }
  return "?";
}

// Mutex must be held by the caller. Logs every call site so a serial
// capture shows the full state trail, with the failure count and the wait
// the next retry will use.
void log_transition(const char* trigger,
                    wifi_config::StateMachine::State old_state) {
  ESP_LOGI(kTag, "%s: state %s -> %s (failures %d, next wait %u ms)", trigger,
          state_name(old_state), state_name(state_machine_->state()),
          state_machine_->retries(),
          static_cast<unsigned>(state_machine_->retry_delay_ms()));
}

// "Network not found" etc. for a failure the board will retry by itself.
const char* waiting_reason_text() {
  switch (last_disconnect_reason_) {
    case DisconnectReason::AuthFailure: return "Password rejected";
    case DisconnectReason::NotFound: return "Network not found";
    case DisconnectReason::Timeout: return "Connection timed out";
    case DisconnectReason::Other:
    case DisconnectReason::None: return "Connection lost";
  }
  return "Connection lost";
}

std::string status_for_state() {
  using State = wifi_config::StateMachine::State;
  switch (state_machine_->state()) {
    case State::Connecting: {
      char buffer[40];
      std::snprintf(buffer, sizeof(buffer), "Connecting - attempt %d",
                    state_machine_->retries() + 1);
      return buffer;
    }
    case State::Waiting: {
      // Static, not a countdown: counting down would mean a publish (and a
      // panel repaint) every second for the whole outage.
      const uint32_t seconds = state_machine_->retry_delay_ms() / 1000;
      char buffer[64];
      if (seconds >= 60) {
        std::snprintf(buffer, sizeof(buffer), "%s - retrying in %u min",
                      waiting_reason_text(),
                      static_cast<unsigned>(seconds / 60));
      } else {
        std::snprintf(buffer, sizeof(buffer), "%s - retrying in %u s",
                      waiting_reason_text(), static_cast<unsigned>(seconds));
      }
      return buffer;
    }
    case State::Connected:
      return "Connected";
    case State::SetupAp:
      switch (last_disconnect_reason_) {
        case DisconnectReason::AuthFailure:
          return "Wrong password - try again";
        case DisconnectReason::NotFound:
          return "Network not found - out of range";
        case DisconnectReason::Timeout:
          return "Connection timed out - try again";
        case DisconnectReason::Other:
          return "Could not connect - try again";
        case DisconnectReason::None:
          return "Waiting for phone";
      }
  }
  return {};
}

// True only for a genuine failure state, per SetupData::error's contract.
bool error_for_state() {
  return state_machine_->state() == wifi_config::StateMachine::State::SetupAp &&
        last_disconnect_reason_ != DisconnectReason::None;
}

// esp_timer callback (runs on the esp_timer task, not the LVGL thread) for a
// Connecting attempt that never produced a wifi/ip event. Routes through the
// same handle_wifi_disconnected() path as a real disconnect, so it waits and
// retries like any ordinary failure; DisconnectReason::Timeout keeps it
// distinguishable from an actual 802.11 disconnect in the log and status
// text. handle_wifi_disconnected() drops it if the state has already left
// Connecting (the callback can be blocked on the mutex while a connect lands).
void connect_timeout_cb(void*) {
  ESP_LOGW(kTag, "connect attempt timed out with no wifi event");
  handle_wifi_disconnected(DisconnectReason::Timeout);
}

void handle_retry_timer();

// esp_timer callback for the end of a Waiting period. Same shape as
// connect_timeout_cb: everything, including esp_wifi_connect(), happens in a
// handler that takes the provisioning mutex, never in the callback itself.
void retry_timer_cb(void*) { handle_retry_timer(); }

// Starts/stops the AP + portal to match the current state and republishes
// the snapshot. Mutex must be held by the caller.
void apply_state_and_publish() {
  using State = wifi_config::StateMachine::State;
  const State state = state_machine_->state();

  const bool entered = !applied_state_.has_value() || *applied_state_ != state;
  applied_state_ = state;
  const wifi_config::RadioPlan plan =
      wifi_config::plan_radio(state, entered, ap_active_, http_active_);

  if (plan.connect_timeout == wifi_config::TimerAction::Arm) {
    // (Re-)arm the bound on this attempt; stop first since starting an
    // already-running one-shot timer is an error.
    esp_timer_stop(connect_timeout_timer_);
    const esp_err_t err =
        esp_timer_start_once(connect_timeout_timer_, kConnectTimeoutUs);
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
  } else if (plan.connect_timeout == wifi_config::TimerAction::Stop) {
    esp_timer_stop(connect_timeout_timer_);
  }

  if (plan.retry_timer == wifi_config::TimerAction::Arm) {
    esp_timer_stop(retry_timer_);
    const uint64_t wait_us =
        static_cast<uint64_t>(state_machine_->retry_delay_ms()) * 1000ULL;
    const esp_err_t err = esp_timer_start_once(retry_timer_, wait_us);
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "retry esp_timer_start_once failed: %s",
               esp_err_to_name(err));
    }
  } else if (plan.retry_timer == wifi_config::TimerAction::Stop) {
    esp_timer_stop(retry_timer_);
  }

  if (plan.new_portal_password) {
    if (ap_ssid_.empty()) {
      ap_ssid_ = wifi_config::setup_ap_ssid(wifi_manager_ap_mac());
    }
    uint8_t random_bytes[wifi_config::kPassphraseLength];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    portal_password_ =
        wifi_config::format_passphrase(random_bytes, sizeof(random_bytes));
  }
  if (plan.start_ap) {
    wifi_manager_start_ap(ap_ssid_);
    ap_active_ = true;
  }
  if (plan.stop_ap) {
    wifi_manager_stop_ap();
    ap_active_ = false;
  }
  if (plan.start_http) {
    portal_start();
    http_active_ = true;
  }
  if (plan.clear_portal_password) {
    // Clearing the password is what keeps the setup form shut: with none
    // set, portal_password_ok() refuses everything, so the only route that
    // still answers is the upload - and that one asks the board.
    portal_password_.clear();
  }

  // setup_flags() is host-tested: only SetupAp sets active (the one thing
  // that puts the Setup page on screen), only Connecting blinks the icon.
  const wifi_config::SetupFlags flags = wifi_config::setup_flags(state);
  snapshot_.setup.active = flags.active;
  snapshot_.setup.connected = flags.connected;
  snapshot_.setup.connecting = flags.connecting;
  snapshot_.setup.ap_ssid = ap_ssid_;
  snapshot_.setup.portal_password = portal_password_;
  snapshot_.setup.portal_url = ap_ssid_.empty() ? std::string{} : kPortalUrl;
  // The password rides in the query string, so it lands in the phone's
  // browser history and any on-path log for the LAN hop between the phone
  // and the board. Acceptable here only because it is per-session, never
  // persisted, and dead the moment setup mode exits; the page password (not
  // network isolation - the AP is open and the portal is LAN-reachable by
  // design) is the only thing protecting the portal.
  snapshot_.setup.qr_payload =
      ap_ssid_.empty()
          ? std::string{}
          : wifi_config::portal_qr_payload(kPortalUrl, portal_password_);
  snapshot_.setup.ip = wifi_config::published_station_ip(state, sta_ip_);
  snapshot_.setup.status = status_for_state();
  snapshot_.setup.error = error_for_state();
  ui::publish_snapshot(snapshot_);
}

}  // namespace

esp_err_t start(const app_core::AppSnapshot& snapshot) {
  snapshot_ = snapshot;
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) return ESP_ERR_NO_MEM;

  esp_timer_create_args_t timer_args{};
  timer_args.callback = &connect_timeout_cb;
  timer_args.dispatch_method = ESP_TIMER_TASK;
  timer_args.name = "wifi_connect_timeout";
  esp_err_t result = esp_timer_create(&timer_args, &connect_timeout_timer_);
  if (result != ESP_OK) return result;

  esp_timer_create_args_t retry_args{};
  retry_args.callback = &retry_timer_cb;
  retry_args.dispatch_method = ESP_TIMER_TASK;
  retry_args.name = "wifi_retry_wait";
  result = esp_timer_create(&retry_args, &retry_timer_);
  if (result != ESP_OK) return result;

  result = nvs_store_init();
  if (result != ESP_OK) return result;

  wifi_config::Credentials creds;
  const bool has_creds = nvs_load(creds);
  state_machine_.emplace(has_creds);
  ESP_LOGI(kTag, "start: has_creds=%d initial_state=%s", has_creds,
          state_name(state_machine_->state()));

  result = wifi_manager_init();
  if (result != ESP_OK) return result;

  lock();
  if (has_creds) wifi_manager_connect_sta(creds);
  apply_state_and_publish();
  unlock();
  return ESP_OK;
}

void toggle_setup() {
  lock();
  const auto old_state = state_machine_->state();
  last_disconnect_reason_ = DisconnectReason::None;
  state_machine_->on_setup_gesture();
  log_transition("setup_gesture", old_state);
  if (state_machine_->state() == wifi_config::StateMachine::State::Connecting) {
    // Toggled out of setup with saved credentials: reconnect.
    wifi_manager_reconnect_sta();
  }
  apply_state_and_publish();
  unlock();
}

void handle_wifi_connected(const std::string& ip) {
  lock();
  sta_ip_ = ip;
  const auto old_state = state_machine_->state();
  last_disconnect_reason_ = DisconnectReason::None;
  state_machine_->on_connected();
  log_transition("connected", old_state);
  apply_state_and_publish();
  unlock();
}

void handle_wifi_disconnected(DisconnectReason reason) {
  using State = wifi_config::StateMachine::State;
  lock();
  const auto old_state = state_machine_->state();
  if (reason == DisconnectReason::Timeout && old_state != State::Connecting) {
    // The timeout callback lost a race with a transition (it was blocked on
    // the mutex while a connect or gesture landed). Not a failure.
    ESP_LOGI(kTag, "stale connect timeout ignored in %s", state_name(old_state));
    unlock();
    return;
  }
  state_machine_->on_disconnected(reason == DisconnectReason::AuthFailure);
  const State new_state = state_machine_->state();
  log_transition("disconnected", old_state);
  if (new_state == old_state) {
    // Waiting or SetupAp: no attempt in flight, so this is a late event from
    // one that already ended or was aborted. Republishing would only restart
    // nothing; leave the running wait alone.
    unlock();
    return;
  }
  if (reason == DisconnectReason::Timeout) {
    // The attempt that timed out may still be in flight inside the driver.
    // Abort it now so the next esp_wifi_connect() (from the retry timer)
    // never lands on a connect already in progress. Its own disconnect event
    // arrives in Waiting and is ignored above.
    wifi_manager_abort_sta();
  }
  if (new_state == State::Connecting) {
    wifi_manager_reconnect_sta();
  } else {
    // Waiting or SetupAp: surface why.
    last_disconnect_reason_ = reason;
    if (new_state == State::SetupAp) {
      ESP_LOGW(kTag, "new credentials rejected before ever connecting, entering setup AP: %s",
               to_string(reason));
    } else {
      ESP_LOGW(kTag, "attempt failed (%s), retrying in %u ms", to_string(reason),
               static_cast<unsigned>(state_machine_->retry_delay_ms()));
    }
  }
  apply_state_and_publish();
  unlock();
}

void handle_wifi_lost_ip() {
  using State = wifi_config::StateMachine::State;
  lock();
  const auto old_state = state_machine_->state();
  state_machine_->on_lost_ip();
  log_transition("lost_ip", old_state);
  if (state_machine_->state() == old_state) {
    // esp_netif raises LOST_IP a while after an ordinary disconnect too; that
    // one is already being handled by the retry loop.
    unlock();
    return;
  }
  if (state_machine_->state() == State::Connecting) {
    // Still associated but without a lease: wifi_manager_reconnect_sta()
    // tears the association down first and reconnects from the disconnect
    // event, which runs DHCP again.
    wifi_manager_reconnect_sta();
  }
  apply_state_and_publish();
  unlock();
}

namespace {

void handle_retry_timer() {
  lock();
  const auto old_state = state_machine_->state();
  state_machine_->on_retry_timer();
  log_transition("retry_timer", old_state);
  if (state_machine_->state() == old_state) {
    // Raced a gesture or connect that already moved the machine on.
    unlock();
    return;
  }
  // Waiting -> Connecting. The previous attempt has ended (its disconnect
  // event is what moved us to Waiting) or was aborted on timeout, so this
  // esp_wifi_connect() never lands on one in flight.
  wifi_manager_reconnect_sta();
  apply_state_and_publish();
  unlock();
}

}  // namespace

// Provider tasks start about 1.7 s before DHCP completes, so a fetch issued at
// task start hits a stack with no route and fails with ESP_ERR_HTTP_CONNECT.
// They wait on this instead of guessing a startup delay.
bool station_has_ip() {
  lock();
  const bool connected = snapshot_.setup.connected;
  unlock();
  return connected;
}

void set_battery(const app_core::BatteryData& battery) {
  lock();
  snapshot_.battery = battery;
#ifndef NDEBUG
  if (g_debug_force_charging && snapshot_.battery.valid) {
    // Logged only on the false->true transition, not on every ~30s sample
    // while it stays forced - enough to answer "did the flag even take
    // effect" without spamming the log for the rest of the test. Without
    // this, GET /force-charging having reached this function at all was
    // only visible by inference from a screenshot.
    if (!snapshot_.battery.charging) {
      ESP_LOGI(kTag,
              "debug_force_charging: overriding charging=false to true "
              "(mV=%d percent=%u)",
              snapshot_.battery.millivolts, snapshot_.battery.percent);
    }
    snapshot_.battery.charging = true;
  }
#endif
  ui::publish_snapshot(snapshot_);
  unlock();
}

#ifndef NDEBUG
void debug_force_charging() {
  lock();
  g_debug_force_charging = true;
  unlock();
}
#endif

void set_ota(const app_core::OtaData& ota) {
  lock();
  snapshot_.ota = ota;
  ui::publish_snapshot(snapshot_);
  unlock();
}

void set_indoor(const app_core::IndoorData& indoor) {
  lock();
  snapshot_.indoor = indoor;
  ui::publish_snapshot(snapshot_);
  unlock();
}

void set_runtime_estimate(const app_core::RuntimeEstimate& estimate) {
  lock();
  // Its own AppSnapshot field, not part of BatteryData - see that field's
  // own comment in app_snapshot.hpp for why: this runs on a different task,
  // at a different cadence, than set_battery() below, and the two must not
  // share a struct either one of them assigns to wholesale.
  snapshot_.battery_runtime = estimate;
  ui::publish_snapshot(snapshot_);
  unlock();
}

void set_weather(const app_core::WeatherData& weather) {
  lock();
  snapshot_.weather = weather;
  ui::publish_snapshot(snapshot_);
  unlock();
}

void set_ua_fx(const app_core::MarketData& market) {
  lock();
  snapshot_.ua_fx = market;
  ui::publish_snapshot(snapshot_);
  unlock();
}

void set_us_market(const app_core::MarketData& market) {
  lock();
  snapshot_.us_market = market;
  ui::publish_snapshot(snapshot_);
  unlock();
}

void set_clock(const app_core::ClockData& clock) {
  lock();
  snapshot_.clock = clock;
  ui::publish_snapshot(snapshot_);
  unlock();
}

std::string current_ap_ssid() {
  lock();
  std::string result = ap_ssid_;
  unlock();
  return result;
}

std::string current_status_text() {
  lock();
  std::string result = status_for_state();
  unlock();
  return result;
}

std::string current_portal_password() {
  lock();
  std::string result = portal_password_;
  unlock();
  return result;
}

void handle_credentials_saved(const wifi_config::Credentials& creds) {
  lock();
  esp_err_t result = nvs_save(creds);
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "NVS save failed: %s", esp_err_to_name(result));
  }
  const auto old_state = state_machine_->state();
  last_disconnect_reason_ = DisconnectReason::None;
  state_machine_->on_credentials_saved();
  log_transition("credentials_saved", old_state);
  wifi_manager_connect_sta(creds);
  apply_state_and_publish();
  unlock();
}

}  // namespace wifi_provision
