#include "wifi_config.hpp"

#include <cassert>
#include <cctype>
#include <cstdio>

namespace wifi_config {
namespace {

// Excludes 0/O/1/l/I so a person can retype it from the screen without
// second-guessing a character.
constexpr char kPassphraseAlphabet[] =
    "23456789abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr std::size_t kPassphraseAlphabetSize = sizeof(kPassphraseAlphabet) - 1;

bool hex_digit(char c, int& value) {
  if (c >= '0' && c <= '9') {
    value = c - '0';
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    value = c - 'a' + 10;
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    value = c - 'A' + 10;
    return true;
  }
  return false;
}

}  // namespace

CredentialError validate(const Credentials& credentials) {
  if (credentials.ssid.empty()) return CredentialError::SsidEmpty;
  if (credentials.ssid.size() > 32) return CredentialError::SsidTooLong;
  if (!credentials.password.empty()) {
    if (credentials.password.size() < 8) return CredentialError::PasswordTooShort;
    if (credentials.password.size() > 63) return CredentialError::PasswordTooLong;
  }
  return CredentialError::None;
}

std::string url_decode(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      decoded.push_back(' ');
    } else if (c == '%' && i + 2 < text.size()) {
      int high = 0;
      int low = 0;
      if (hex_digit(text[i + 1], high) && hex_digit(text[i + 2], low)) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
      } else {
        decoded.push_back(c);
      }
    } else {
      decoded.push_back(c);
    }
  }
  return decoded;
}

bool parse_form(std::string_view body, Credentials& out) {
  bool found_ssid = false;
  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t amp = body.find('&', start);
    const std::string_view pair = body.substr(
        start, amp == std::string_view::npos ? amp : amp - start);
    const std::size_t eq = pair.find('=');
    const std::string_view key = pair.substr(0, eq);
    const std::string_view value =
        eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
    if (key == "ssid") {
      out.ssid = url_decode(value);
      found_ssid = true;
    } else if (key == "password") {
      out.password = url_decode(value);
    }
    if (amp == std::string_view::npos) break;
    start = amp + 1;
  }
  return found_ssid;
}

bool find_form_value(std::string_view body, std::string_view key, std::string& out) {
  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t amp = body.find('&', start);
    const std::string_view pair = body.substr(
        start, amp == std::string_view::npos ? amp : amp - start);
    const std::size_t eq = pair.find('=');
    const std::string_view pair_key = pair.substr(0, eq);
    if (pair_key == key) {
      const std::string_view value =
          eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
      out = url_decode(value);
      return true;
    }
    if (amp == std::string_view::npos) break;
    start = amp + 1;
  }
  return false;
}

std::string setup_ap_ssid(const std::array<uint8_t, 6>& mac) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "RLCD-%02X%02X%02X", mac[3], mac[4],
                mac[5]);
  return std::string(buffer);
}

std::string portal_qr_payload(std::string_view portal_url, std::string_view password) {
  std::string payload(portal_url);
  if (password.empty()) return payload;
  for (const char c : password) {
    assert(std::isalnum(static_cast<unsigned char>(c)));
  }
  payload += "?pw=";
  payload += password;
  return payload;
}

std::string format_passphrase(const uint8_t* bytes, std::size_t length) {
  if (bytes == nullptr || length < kPassphraseLength) return {};
  std::string out;
  out.reserve(kPassphraseLength);
  for (std::size_t i = 0; i < kPassphraseLength; ++i) {
    out.push_back(kPassphraseAlphabet[bytes[i] % kPassphraseAlphabetSize]);
  }
  return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

StateMachine::StateMachine(bool has_saved_credentials)
    : state_(has_saved_credentials ? State::Connecting : State::SetupAp),
      has_credentials_(has_saved_credentials) {}

uint32_t StateMachine::retry_delay_ms() const {
  // Indexed by consecutive failures; the last entry repeats forever.
  static constexpr uint32_t kDelaysMs[] = {5000, 10000, 30000, 60000, 300000};
  constexpr int kCount = static_cast<int>(sizeof(kDelaysMs) / sizeof(kDelaysMs[0]));
  if (retries_ <= 1) return kDelaysMs[0];
  return retries_ >= kCount ? kDelaysMs[kCount - 1] : kDelaysMs[retries_ - 1];
}

void StateMachine::on_connected() {
  state_ = State::Connected;
  retries_ = 0;
  connected_once_ = true;
}

void StateMachine::on_disconnected(bool auth_failure) {
  switch (state_) {
    case State::Connecting:
      ++retries_;
      // Only a rejection of credentials that have never worked is worth
      // asking a person about; everything else is the network's problem and
      // goes away on its own, so wait and try again.
      state_ = (auth_failure && !connected_once_) ? State::SetupAp
                                                  : State::Waiting;
      break;
    case State::Connected:
      // Try again at once: most drops are momentary. The backoff starts
      // fresh from the first delay if that attempt fails.
      state_ = State::Connecting;
      retries_ = 0;
      break;
    case State::Waiting:
    case State::SetupAp:
      // No attempt is in flight: a late event from one that already ended
      // (or was aborted) must neither count nor restart anything.
      break;
  }
}

void StateMachine::on_lost_ip() {
  if (state_ == State::Connected) on_disconnected(false);
}

void StateMachine::on_retry_timer() {
  if (state_ == State::Waiting) state_ = State::Connecting;
}

void StateMachine::on_setup_gesture() {
  switch (state_) {
    case State::Connecting:
    case State::Connected:
    case State::Waiting:
      state_ = State::SetupAp;
      break;
    case State::SetupAp:
      if (has_credentials_) {
        state_ = State::Connecting;
        retries_ = 0;
      }
      break;
  }
}

void StateMachine::on_credentials_saved() {
  has_credentials_ = true;
  connected_once_ = false;
  state_ = State::Connecting;
  retries_ = 0;
}

SetupFlags setup_flags(StateMachine::State state) {
  using State = StateMachine::State;
  SetupFlags flags;
  flags.active = state == State::SetupAp;
  flags.connected = state == State::Connected;
  flags.connecting = state == State::Connecting;
  return flags;
}

RadioPlan plan_radio(StateMachine::State state, bool entered, bool ap_active,
                     bool http_active) {
  using State = StateMachine::State;
  RadioPlan plan;
  plan.connect_timeout =
      state == State::Connecting ? TimerAction::Arm : TimerAction::Stop;
  plan.retry_timer = state != State::Waiting ? TimerAction::Stop
                     : entered               ? TimerAction::Arm
                                             : TimerAction::Keep;
  switch (state) {
    case State::SetupAp:
      // Decided by the hotspot's own flag, never by the HTTP server's: the
      // server also runs while Connected (firmware uploads), and reading it
      // as "setup is up" is exactly what stranded a board that lost its
      // network with no hotspot to join.
      plan.start_ap = !ap_active;
      plan.new_portal_password = entered || !ap_active;
      plan.start_http = !http_active;
      break;
    case State::Connected:
      // The hotspot goes; the HTTP server stays - it is what a firmware push
      // from a machine on the same network arrives at.
      plan.stop_ap = ap_active;
      plan.clear_portal_password = true;
      plan.start_http = !http_active;
      break;
    case State::Waiting:
      // Setup was not asked for: no hotspot while the board just waits to
      // retry its own network. The HTTP server is left as it is.
      plan.stop_ap = ap_active;
      plan.clear_portal_password = true;
      break;
    case State::Connecting:
      // A Connecting that came out of setup (credentials just saved, or the
      // gesture back) leaves the hotspot up, as it always has: the phone that
      // submitted the form is still on it, and if the attempt fails with a
      // wrong password the board is back in setup with nothing to rejoin.
      break;
  }
  return plan;
}

std::string published_station_ip(StateMachine::State state,
                                 std::string_view ip) {
  return state == StateMachine::State::Connected ? std::string(ip)
                                                 : std::string();
}

}  // namespace wifi_config
