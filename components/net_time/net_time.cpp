#include "net_time.hpp"

#include <atomic>
#include <cstdlib>
#include <ctime>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_netif_types.h>

namespace net_time {
namespace {

constexpr char kTag[] = "net_time";
constexpr char kNtpServer[] = "pool.ntp.org";

std::atomic<bool> g_synced{false};
bool g_started = false;

// esp_netif fires this on every resync, not just the first; only the
// transition into synced is worth a log line.
void on_sync(struct timeval* tv) {
  if (g_synced.exchange(true)) return;
  app_core::RtcDateTime decoded{};
  epoch_to_local(tv->tv_sec, decoded);
  ESP_LOGI(kTag, "NTP sync landed: %04u-%02u-%02u %02u:%02u:%02u local",
           decoded.year, decoded.month, decoded.day, decoded.hour,
           decoded.minute, decoded.second);
}

// SNTP does not queue requests until the network is up - the comment at this
// component's call site in app_main used to claim it did, and the board
// disproved it. lwIP fires its first request at the configured startup delay,
// which is before the station has an IP, so DNS fails and lwIP backs off,
// doubling its retry up to a 30 s cap. Measured on this board: the link and
// TLS were up at 4.5 s and the sync did not land until 31.7 s - about 27 s of
// waiting on a backoff bought by one request that never had a network.
//
// Shortening LWIP_SNTP_MAXIMUM_STARTUP_DELAY (it is 100 ms here) does not help
// and slightly hurts: it only makes the doomed first request happen sooner.
//
// So the request is re-sent when there is actually something to send it to.
// esp_sntp_restart() resets the backoff and polls immediately. Registering on
// the event rather than polling a flag also covers reconnects: every new lease
// re-syncs instead of waiting out whatever backoff the disconnection left.
void on_got_ip(void*, esp_event_base_t, int32_t, void*) {
  ESP_LOGI(kTag, "station has an IP; restarting SNTP");
  esp_sntp_restart();
}

}  // namespace

esp_err_t start() {
  if (g_started) return ESP_OK;
  g_started = true;

  setenv("TZ", kTimeZone, 1);
  tzset();

  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(kNtpServer);
  config.sync_cb = &on_sync;
  const esp_err_t result = esp_netif_sntp_init(&config);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "esp_netif_sntp_init failed: %s", esp_err_to_name(result));
    return result;
  }

  // Non-fatal on failure: without it the clock still syncs, just on lwIP's
  // own backoff - which is exactly the 31.7 s this handler exists to avoid,
  // so it is worth an error line rather than a silent degrade.
  const esp_err_t hooked = esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, nullptr);
  if (hooked != ESP_OK) {
    ESP_LOGE(kTag, "got-IP handler registration failed: %s; first sync will "
                   "wait out the SNTP backoff", esp_err_to_name(hooked));
  }
  return result;
}

bool synced() { return g_synced.load(); }

bool now(app_core::RtcDateTime& out) {
  if (!g_synced.load()) return false;
  epoch_to_local(std::time(nullptr), out);
  return true;
}

}  // namespace net_time
