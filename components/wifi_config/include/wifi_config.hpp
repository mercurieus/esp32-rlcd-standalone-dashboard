#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wifi_config {

struct Credentials {
  std::string ssid;
  std::string password;
};

enum class CredentialError {
  None,
  SsidEmpty,
  SsidTooLong,
  PasswordTooShort,
  PasswordTooLong
};

// SSID must be 1..32 bytes. Password is empty (open network) or 8..63 bytes
// (WPA2-PSK). Lengths are byte lengths, not character counts.
CredentialError validate(const Credentials& credentials);

// Decodes application/x-www-form-urlencoded text: '+' becomes a space and
// '%XX' becomes the byte XX (hex, either case). A malformed or truncated '%'
// escape is kept literally rather than dropped.
std::string url_decode(std::string_view text);

// Parses an application/x-www-form-urlencoded body for the "ssid" and
// "password" keys; unknown keys are ignored. A missing "password" key means
// an empty password. Returns false only if the "ssid" key is absent.
bool parse_form(std::string_view body, Credentials& out);

// Extracts and url-decodes the value for `key` from an
// application/x-www-form-urlencoded string - a POST body or a URL query
// string (without the leading '?'), same syntax either way. Returns false
// and leaves `out` untouched if `key` is absent.
bool find_form_value(std::string_view body, std::string_view key, std::string& out);

// Builds the setup-mode access point SSID, e.g. "RLCD-A1B2C3", from the last
// three bytes of the given MAC address.
std::string setup_ap_ssid(const std::array<uint8_t, 6>& mac);

// Builds the setup-portal QR payload: the bare portal URL with the
// per-session page password appended as a query parameter, e.g.
// "http://192.168.4.1/?pw=aB3dEfGh". Returns portal_url unchanged if
// password is empty. The password alphabet (format_passphrase) is
// alnum-only and therefore already URL-safe; that is asserted rather than
// implemented as a percent-encoder nothing exercises.
std::string portal_qr_payload(std::string_view portal_url, std::string_view password);

// Length of a generated setup-portal page password.
inline constexpr std::size_t kPassphraseLength = 8;

// Formats an 8-character setup-portal page password from caller-supplied
// random bytes (one byte consumed per character), drawn from an alphabet
// that excludes 0/O/1/l/I so it can be retyped from the screen. Returns an
// empty string if fewer than kPassphraseLength bytes are supplied.
std::string format_passphrase(const uint8_t* bytes, std::size_t length);

// Constant-time comparison for the portal page password: never early-returns
// on the first differing byte, so a network attacker cannot time their way
// to the password one character at a time.
bool constant_time_equal(std::string_view a, std::string_view b);

// Provisioning lifecycle. Owns no timers or threads; callers drive it with
// events observed elsewhere (wifi_provision.cpp arms the one-shot timers the
// plan below asks for and feeds their expiry back in).
//
//   (no saved credentials) --------------------------> SetupAp
//   (saved credentials) ---> Connecting
//   Connecting --connected--> Connected
//   Connecting --failed, auth failure, set not proven--> SetupAp
//   Connecting --failed, anything else--> Waiting   (retries + 1)
//   Waiting --retry timer (retry_delay_ms())--> Connecting
//   Connected --disconnected / lost IP--> Connecting (immediate, retries = 0)
//   Connecting / Connected / Waiting --setup gesture--> SetupAp
//   SetupAp --setup gesture, has credentials--> Connecting (retries = 0)
//   any --credentials saved--> Connecting (new set: retries = 0, not proven)
//
// There is no retry budget. A fixed budget of five spent itself in well under
// a minute on an ordinary router reboot (immediate reconnects plus the 15 s
// connect timeout) and then sat in SetupAp, which retried nothing, until
// someone pressed KEY. Setup now opens on its own only when it is actually
// needed: no credentials, or credentials typed into the portal this boot that
// the network rejected before they ever worked (a typo). Everything else -
// including any failure, auth-class or not, of credentials loaded from NVS -
// waits and retries, backing off, forever.
class StateMachine {
 public:
  enum class State { Connecting, Connected, SetupAp, Waiting };

  explicit StateMachine(bool has_saved_credentials);

  State state() const { return state_; }
  // Consecutive failed attempts since the last connect, gesture back from
  // setup, or credentials save.
  int retries() const { return retries_; }
  // How long Waiting waits before the next attempt, from retries(): 5 s,
  // 10 s, 30 s, 60 s, then 300 s for every later failure. Short at first so
  // a router reboot or a blip costs seconds; capped at 5 min so an outage
  // costs one scan every 5 min of radio time, not a scan every few seconds.
  uint32_t retry_delay_ms() const;

  void on_connected();
  // auth_failure: the network rejected the credentials (wrong password).
  // Only consulted in Connecting, and only for credentials that are not yet
  // proven (see proven_); ignored (stale) in Waiting and SetupAp.
  void on_disconnected(bool auth_failure);
  // IP_EVENT_STA_LOST_IP. A disconnect only while Connected; esp_netif also
  // raises it after an ordinary disconnect, which is already being handled.
  void on_lost_ip();
  // The Waiting timer expired: Waiting -> Connecting. No-op elsewhere, so a
  // timer callback that raced a transition changes nothing.
  void on_retry_timer();
  void on_setup_gesture();
  void on_credentials_saved();

 private:
  State state_;
  bool has_credentials_;
  // The current credential set is trusted: it was loaded from NVS at boot
  // (it was saved because it once worked), or it has reached Connected since
  // it was saved through the portal. An auth-class failure of a proven set is
  // a flaky or half-booted router - after a shared power cut one answers with
  // 4WAY_HANDSHAKE_TIMEOUT / ASSOC_FAIL - not a wrong password, and must not
  // strand the board in setup. Only a portal save this boot clears it.
  bool proven_ = false;
  int retries_ = 0;
};

// The three SetupData flags a state publishes. Only SetupAp sets active,
// which is the one thing ui_app.cpp uses to put the Setup page on screen -
// Waiting and Connecting leave the carousel up. Only Connecting sets
// connecting, the flag that blinks the tray icon, so the icon blinks during
// an actual attempt and sits crossed out (Off) while Waiting.
struct SetupFlags {
  bool active = false;
  bool connected = false;
  bool connecting = false;
};
SetupFlags setup_flags(StateMachine::State state);

// What the driver (wifi_provision.cpp) must do to the radio, the HTTP server
// and its one-shot timers after the state machine settles in `state`. Pure so
// the rules can be host-tested; the driver only executes the plan.
//
// The setup hotspot and the HTTP server are tracked separately on purpose.
// They used to share one "portal active" flag: Connected starts the HTTP
// server for firmware uploads and set that flag, so a board that later lost
// its network and fell back to SetupAp saw "already active" and never started
// the hotspot - no SSID, no page password, no QR, nothing to join. Entering
// SetupAp now always brings the hotspot up whatever ran before.
enum class TimerAction { Keep, Arm, Stop };

struct RadioPlan {
  bool start_ap = false;
  bool stop_ap = false;
  // A fresh page password on every entry into SetupAp (and whenever the
  // hotspot itself has to be started); cleared once setup is left for good.
  bool new_portal_password = false;
  bool clear_portal_password = false;
  bool start_http = false;
  TimerAction connect_timeout = TimerAction::Keep;
  // Armed once on entry into Waiting with retry_delay_ms(); Keep while
  // Waiting so a republish never restarts the wait; Stop elsewhere.
  TimerAction retry_timer = TimerAction::Keep;
};

// entered: this is the first plan since the state changed (or the first one
// ever). ap_active / http_active: what is running right now.
RadioPlan plan_radio(StateMachine::State state, bool entered, bool ap_active,
                     bool http_active);

// The station address to publish for a given state: the address itself only
// while Connected, empty in every other state. The address arrives with
// IP_EVENT_STA_GOT_IP and a lease does not announce its own loss, so the
// publisher decides by state rather than trusting a remembered string - a
// board that dropped to Connecting or fell back to setup must not keep showing
// the address it used to have.
std::string published_station_ip(StateMachine::State state,
                                 std::string_view ip);

}  // namespace wifi_config
