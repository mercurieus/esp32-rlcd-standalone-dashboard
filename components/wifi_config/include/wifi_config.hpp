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
// events observed elsewhere.
class StateMachine {
 public:
  enum class State { Connecting, Connected, SetupAp };
  static constexpr int kMaxRetries = 5;

  explicit StateMachine(bool has_saved_credentials);

  State state() const { return state_; }
  int retries() const { return retries_; }

  void on_connected();
  void on_disconnected();
  void on_setup_gesture();
  void on_credentials_saved();

 private:
  State state_;
  bool has_credentials_;
  int retries_ = 0;
};

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
