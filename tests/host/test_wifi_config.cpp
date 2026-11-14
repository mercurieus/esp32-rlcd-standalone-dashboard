#include "test_support.hpp"

#include "wifi_config.hpp"

#include <array>
#include <string>

using wifi_config::Credentials;
using wifi_config::CredentialError;
using wifi_config::StateMachine;

HOST_TEST(validate_rejects_empty_ssid) {
  EXPECT_EQ(wifi_config::validate(Credentials{"", ""}), CredentialError::SsidEmpty);
}

HOST_TEST(validate_accepts_thirty_two_byte_ssid) {
  const Credentials credentials{std::string(32, 'a'), ""};
  EXPECT_EQ(wifi_config::validate(credentials), CredentialError::None);
}

HOST_TEST(validate_rejects_thirty_three_byte_ssid) {
  const Credentials credentials{std::string(33, 'a'), ""};
  EXPECT_EQ(wifi_config::validate(credentials), CredentialError::SsidTooLong);
}

HOST_TEST(validate_accepts_empty_password_for_open_network) {
  EXPECT_EQ(wifi_config::validate(Credentials{"net", ""}), CredentialError::None);
}

HOST_TEST(validate_rejects_seven_byte_password) {
  const Credentials credentials{"net", std::string(7, 'p')};
  EXPECT_EQ(wifi_config::validate(credentials), CredentialError::PasswordTooShort);
}

HOST_TEST(validate_accepts_eight_byte_password) {
  const Credentials credentials{"net", std::string(8, 'p')};
  EXPECT_EQ(wifi_config::validate(credentials), CredentialError::None);
}

HOST_TEST(validate_accepts_sixty_three_byte_password) {
  const Credentials credentials{"net", std::string(63, 'p')};
  EXPECT_EQ(wifi_config::validate(credentials), CredentialError::None);
}

HOST_TEST(validate_rejects_sixty_four_byte_password) {
  const Credentials credentials{"net", std::string(64, 'p')};
  EXPECT_EQ(wifi_config::validate(credentials), CredentialError::PasswordTooLong);
}

HOST_TEST(url_decode_turns_plus_into_space) {
  EXPECT_EQ(wifi_config::url_decode("My+Net"), std::string("My Net"));
}

HOST_TEST(url_decode_handles_upper_and_lower_hex) {
  EXPECT_EQ(wifi_config::url_decode("hunter2%21"), std::string("hunter2!"));
  EXPECT_EQ(wifi_config::url_decode("%2b%2B"), std::string("++"));
}

HOST_TEST(url_decode_keeps_malformed_escape_literal) {
  EXPECT_EQ(wifi_config::url_decode("100%"), std::string("100%"));
  EXPECT_EQ(wifi_config::url_decode("100%2"), std::string("100%2"));
  EXPECT_EQ(wifi_config::url_decode("100%zz"), std::string("100%zz"));
}

HOST_TEST(parse_form_decodes_ssid_and_password) {
  Credentials credentials;
  const bool ok =
      wifi_config::parse_form("ssid=My+Net&password=hunter2%21", credentials);
  EXPECT_TRUE(ok);
  EXPECT_EQ(credentials.ssid, std::string("My Net"));
  EXPECT_EQ(credentials.password, std::string("hunter2!"));
}

HOST_TEST(parse_form_defaults_password_when_key_missing) {
  Credentials credentials;
  const bool ok = wifi_config::parse_form("ssid=OpenNet", credentials);
  EXPECT_TRUE(ok);
  EXPECT_EQ(credentials.ssid, std::string("OpenNet"));
  EXPECT_EQ(credentials.password, std::string(""));
}

HOST_TEST(parse_form_ignores_unknown_keys) {
  Credentials credentials;
  const bool ok =
      wifi_config::parse_form("foo=bar&ssid=Net&password=secret12", credentials);
  EXPECT_TRUE(ok);
  EXPECT_EQ(credentials.ssid, std::string("Net"));
  EXPECT_EQ(credentials.password, std::string("secret12"));
}

HOST_TEST(parse_form_fails_when_ssid_key_is_absent) {
  Credentials credentials;
  const bool ok = wifi_config::parse_form("password=secret12", credentials);
  EXPECT_TRUE(!ok);
}

HOST_TEST(setup_ap_ssid_uses_last_three_mac_bytes_uppercase) {
  const std::array<uint8_t, 6> mac{0xDE, 0xAD, 0xBE, 0xA1, 0xB2, 0xC3};
  EXPECT_EQ(wifi_config::setup_ap_ssid(mac), std::string("RLCD-A1B2C3"));
}

HOST_TEST(portal_qr_payload_appends_password_as_query_param) {
  EXPECT_EQ(wifi_config::portal_qr_payload("http://192.168.4.1/", "aB3dEfGh"),
            std::string("http://192.168.4.1/?pw=aB3dEfGh"));
}

HOST_TEST(portal_qr_payload_returns_bare_url_when_password_empty) {
  EXPECT_EQ(wifi_config::portal_qr_payload("http://192.168.4.1/", ""),
            std::string("http://192.168.4.1/"));
}

HOST_TEST(find_form_value_extracts_named_key_from_query_or_body_syntax) {
  std::string value;
  EXPECT_TRUE(wifi_config::find_form_value("pw=aB3dEfGh", "pw", value));
  EXPECT_EQ(value, std::string("aB3dEfGh"));
  EXPECT_TRUE(wifi_config::find_form_value("ssid=My+Net&pw=hunter2%21", "pw", value));
  EXPECT_EQ(value, std::string("hunter2!"));
}

HOST_TEST(find_form_value_returns_false_when_key_is_absent) {
  std::string value;
  EXPECT_TRUE(!wifi_config::find_form_value("ssid=Net", "pw", value));
}

HOST_TEST(constant_time_equal_matches_identical_strings) {
  EXPECT_TRUE(wifi_config::constant_time_equal("aB3dEfGh", "aB3dEfGh"));
}

HOST_TEST(constant_time_equal_rejects_different_strings_same_length) {
  EXPECT_TRUE(!wifi_config::constant_time_equal("aB3dEfGh", "aB3dEfGx"));
}

HOST_TEST(constant_time_equal_rejects_different_lengths) {
  EXPECT_TRUE(!wifi_config::constant_time_equal("short", "muchlonger"));
  EXPECT_TRUE(!wifi_config::constant_time_equal("", "aB3dEfGh"));
}

HOST_TEST(format_passphrase_has_pinned_length) {
  EXPECT_EQ(wifi_config::kPassphraseLength, static_cast<std::size_t>(8));
  const std::array<uint8_t, wifi_config::kPassphraseLength> bytes{
      0, 1, 2, 3, 4, 5, 6, 7};
  EXPECT_EQ(wifi_config::format_passphrase(bytes.data(), bytes.size()).size(),
            wifi_config::kPassphraseLength);
}

HOST_TEST(format_passphrase_uses_only_unambiguous_alphabet) {
  const std::string alphabet =
      "23456789abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ";
  std::array<uint8_t, wifi_config::kPassphraseLength> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(i * 37 + 5);
  }
  const std::string passphrase =
      wifi_config::format_passphrase(bytes.data(), bytes.size());
  for (const char c : passphrase) {
    EXPECT_TRUE(alphabet.find(c) != std::string::npos);
    EXPECT_TRUE(c != '0' && c != 'O' && c != '1' && c != 'l' && c != 'I');
  }
}

HOST_TEST(format_passphrase_is_deterministic_for_identical_input) {
  const std::array<uint8_t, wifi_config::kPassphraseLength> bytes{
      9, 8, 7, 6, 5, 4, 3, 2};
  EXPECT_EQ(wifi_config::format_passphrase(bytes.data(), bytes.size()),
            wifi_config::format_passphrase(bytes.data(), bytes.size()));
}

HOST_TEST(format_passphrase_differs_for_different_input) {
  const std::array<uint8_t, wifi_config::kPassphraseLength> a{
      1, 2, 3, 4, 5, 6, 7, 8};
  const std::array<uint8_t, wifi_config::kPassphraseLength> b{
      8, 7, 6, 5, 4, 3, 2, 1};
  EXPECT_TRUE(wifi_config::format_passphrase(a.data(), a.size()) !=
              wifi_config::format_passphrase(b.data(), b.size()));
}

HOST_TEST(format_passphrase_rejects_short_buffer) {
  const std::array<uint8_t, wifi_config::kPassphraseLength - 1> bytes{
      1, 2, 3, 4, 5, 6, 7};
  EXPECT_EQ(wifi_config::format_passphrase(bytes.data(), bytes.size()),
            std::string(""));
  EXPECT_EQ(wifi_config::format_passphrase(nullptr, 0), std::string(""));
}

HOST_TEST(state_machine_starts_connecting_when_credentials_saved) {
  StateMachine machine(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
}

HOST_TEST(state_machine_starts_setup_ap_without_saved_credentials) {
  StateMachine machine(false);
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
}

// Was state_machine_moves_to_setup_ap_after_max_retries. The retry budget is
// gone (it is what stranded the operator's board); the intent - a run of
// ordinary failures on a never-connected credential set - now pins that the
// machine keeps retrying instead of giving up.
HOST_TEST(state_machine_never_connected_ordinary_failures_keep_retrying) {
  StateMachine machine(true);
  for (int i = 1; i <= 8; ++i) {
    EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
    machine.on_disconnected(false);
    EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
    EXPECT_EQ(machine.retries(), i);
    machine.on_retry_timer();
  }
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
}

HOST_TEST(state_machine_connected_resets_retry_counter) {
  StateMachine machine(true);
  machine.on_disconnected(false);
  machine.on_retry_timer();
  machine.on_disconnected(false);
  machine.on_retry_timer();
  machine.on_connected();
  EXPECT_TRUE(machine.state() == StateMachine::State::Connected);
  EXPECT_EQ(machine.retries(), 0);
}

HOST_TEST(state_machine_disconnect_from_connected_returns_to_connecting) {
  StateMachine machine(true);
  machine.on_connected();
  machine.on_disconnected(false);
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
  EXPECT_EQ(machine.retries(), 0);
}

HOST_TEST(state_machine_ignores_disconnect_while_in_setup_ap) {
  StateMachine machine(false);
  machine.on_disconnected(false);
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
  machine.on_disconnected(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
  EXPECT_EQ(machine.retries(), 0);
}

HOST_TEST(state_machine_gesture_toggles_to_setup_ap_from_connecting_and_connected) {
  StateMachine connecting_machine(true);
  connecting_machine.on_setup_gesture();
  EXPECT_TRUE(connecting_machine.state() == StateMachine::State::SetupAp);

  StateMachine connected_machine(true);
  connected_machine.on_connected();
  connected_machine.on_setup_gesture();
  EXPECT_TRUE(connected_machine.state() == StateMachine::State::SetupAp);
}

HOST_TEST(state_machine_gesture_from_setup_ap_returns_to_connecting_with_credentials) {
  StateMachine machine(true);
  machine.on_setup_gesture();
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
  machine.on_setup_gesture();
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
}

HOST_TEST(state_machine_gesture_from_setup_ap_stays_without_credentials) {
  StateMachine machine(false);
  machine.on_setup_gesture();
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
}

HOST_TEST(state_machine_credentials_saved_moves_to_connecting_with_fresh_retries) {
  StateMachine machine(false);
  machine.on_setup_gesture();
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
  machine.on_credentials_saved();
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
  EXPECT_EQ(machine.retries(), 0);

  machine.on_disconnected(false);
  machine.on_retry_timer();
  machine.on_disconnected(false);
  machine.on_credentials_saved();
  EXPECT_EQ(machine.retries(), 0);
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
}

// Was: five failures after saving credentials land in SetupAp. The intent -
// what a just-saved credential set does when it fails - now splits by cause:
// a wrong password reopens setup, anything else waits and retries.
HOST_TEST(state_machine_reconnect_after_saving_credentials_then_disconnect) {
  StateMachine wrong_password(false);
  wrong_password.on_credentials_saved();
  EXPECT_TRUE(wrong_password.state() == StateMachine::State::Connecting);
  wrong_password.on_disconnected(true);
  EXPECT_TRUE(wrong_password.state() == StateMachine::State::SetupAp);

  StateMachine out_of_range(false);
  out_of_range.on_credentials_saved();
  for (int i = 0; i < 6; ++i) {
    out_of_range.on_disconnected(false);
    EXPECT_TRUE(out_of_range.state() == StateMachine::State::Waiting);
    out_of_range.on_retry_timer();
  }
}

// Was state_machine_on_disconnected_is_reason_agnostic_five_reach_setup_ap.
// wifi_provision.cpp's connect-timeout esp_timer fires
// handle_wifi_disconnected(DisconnectReason::Timeout), which reaches
// on_disconnected(false): a timed-out attempt is an ordinary failure, so it
// waits and retries like "network not found" and never opens setup.
HOST_TEST(state_machine_timed_out_attempts_wait_and_retry_like_any_ordinary_failure) {
  StateMachine machine(true);
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
    machine.on_disconnected(false);
    EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
    machine.on_retry_timer();
  }
  EXPECT_EQ(machine.retries(), 7);
}

// The operator's case: the board had a working network, then the router
// rebooted (or the board went out of range). It must never give up into
// setup on its own; it alternates waiting and trying, backing off.
HOST_TEST(state_machine_router_reboot_retries_forever_with_backoff) {
  StateMachine machine(true);
  machine.on_connected();
  machine.on_disconnected(false);
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
  const uint32_t expected[] = {5000, 10000, 30000, 60000, 300000, 300000,
                               300000};
  for (const uint32_t delay : expected) {
    EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
    machine.on_disconnected(false);  // "network not found"
    EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
    EXPECT_EQ(machine.retry_delay_ms(), delay);
    machine.on_retry_timer();
  }
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
}

// Was state_machine_wrong_password_never_connected_opens_setup, which booted
// with saved credentials. Fix round 1: credentials loaded from NVS are
// proven. After a shared power cut a half-booted router answers with
// 4WAY_HANDSHAKE_TIMEOUT / ASSOC_FAIL (auth-class), and that must not strand
// the board in setup - it backs off forever like any other failure.
HOST_TEST(state_machine_boot_credentials_never_open_setup_on_auth_failures) {
  StateMachine machine(true);
  const uint32_t expected[] = {5000, 10000, 30000, 60000, 300000, 300000,
                               300000};
  for (const uint32_t delay : expected) {
    EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
    machine.on_disconnected(true);
    EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
    EXPECT_EQ(machine.retry_delay_ms(), delay);
    machine.on_retry_timer();
  }
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
}

// Credentials typed into the portal this boot and rejected before they ever
// worked: most likely a typo, so setup reopens for the person to fix it.
HOST_TEST(state_machine_portal_credentials_auth_failure_before_connect_opens_setup) {
  StateMachine machine(true);
  machine.on_setup_gesture();
  machine.on_credentials_saved();
  machine.on_disconnected(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
}

// The same portal credentials, once they have connected, are proven too.
HOST_TEST(state_machine_portal_credentials_auth_failure_after_connect_waits) {
  StateMachine machine(false);
  machine.on_credentials_saved();
  machine.on_connected();
  machine.on_disconnected(false);  // Connected -> immediate attempt
  machine.on_disconnected(true);   // that attempt is rejected
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
  machine.on_retry_timer();
  machine.on_disconnected(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
}

// A rejected handshake after the network has worked this boot is far more
// likely a flaky router than a password that changed under us; retry.
HOST_TEST(state_machine_auth_failure_after_a_connect_waits_instead_of_setup) {
  StateMachine machine(true);
  machine.on_connected();
  machine.on_disconnected(false);
  machine.on_disconnected(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
  machine.on_retry_timer();
  machine.on_disconnected(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
}

// New credentials are a new set: "has connected" does not carry over.
HOST_TEST(state_machine_new_credentials_forget_the_earlier_connect) {
  StateMachine machine(true);
  machine.on_connected();
  machine.on_setup_gesture();
  machine.on_credentials_saved();
  machine.on_disconnected(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
}

HOST_TEST(state_machine_successful_connect_resets_backoff) {
  StateMachine machine(true);
  for (int i = 0; i < 4; ++i) {
    machine.on_disconnected(false);
    machine.on_retry_timer();
  }
  machine.on_disconnected(false);
  EXPECT_EQ(machine.retry_delay_ms(), static_cast<uint32_t>(300000));
  machine.on_retry_timer();
  machine.on_connected();
  machine.on_disconnected(false);  // Connected -> immediate attempt
  machine.on_disconnected(false);  // that attempt fails
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
  EXPECT_EQ(machine.retry_delay_ms(), static_cast<uint32_t>(5000));
}

HOST_TEST(state_machine_gesture_from_waiting_opens_setup_and_back) {
  StateMachine machine(true);
  machine.on_disconnected(false);
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
  machine.on_setup_gesture();
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
  machine.on_setup_gesture();
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
}

// Stale events must not move a waiting machine: a late disconnect (e.g. from
// aborting a timed-out attempt) neither counts nor restarts anything.
HOST_TEST(state_machine_waiting_ignores_disconnects_and_retry_timer_only_acts_there) {
  StateMachine machine(true);
  machine.on_disconnected(false);
  machine.on_disconnected(false);
  machine.on_disconnected(true);
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
  EXPECT_EQ(machine.retries(), 1);

  StateMachine connected(true);
  connected.on_connected();
  connected.on_retry_timer();
  EXPECT_TRUE(connected.state() == StateMachine::State::Connected);
  StateMachine setup(false);
  setup.on_retry_timer();
  EXPECT_TRUE(setup.state() == StateMachine::State::SetupAp);
}

// IP_EVENT_STA_LOST_IP is a disconnect only while Connected. esp_netif also
// raises it ~120 s after an ordinary disconnect, by which time the machine
// is already Waiting or Connecting; that one must change nothing.
HOST_TEST(state_machine_lost_ip_acts_only_while_connected) {
  StateMachine machine(true);
  machine.on_connected();
  machine.on_lost_ip();
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
  EXPECT_EQ(machine.retries(), 0);
  machine.on_lost_ip();
  EXPECT_TRUE(machine.state() == StateMachine::State::Connecting);
  machine.on_disconnected(false);
  machine.on_lost_ip();
  EXPECT_TRUE(machine.state() == StateMachine::State::Waiting);
  EXPECT_EQ(machine.retries(), 1);
}

HOST_TEST(state_machine_no_credentials_starts_in_setup_unchanged) {
  StateMachine machine(false);
  EXPECT_TRUE(machine.state() == StateMachine::State::SetupAp);
  EXPECT_EQ(machine.retries(), 0);
}

// Operator requirement: only SetupAp takes the screen. ui_app.cpp shows the
// Setup page exactly while SetupData::active is true, so Waiting and
// Connecting must publish active == false (the carousel stays up), and only
// Connecting may set the flag that blinks the tray icon.
HOST_TEST(setup_flags_only_setup_ap_takes_the_screen) {
  using State = StateMachine::State;
  const wifi_config::SetupFlags waiting = wifi_config::setup_flags(State::Waiting);
  EXPECT_TRUE(!waiting.active);
  EXPECT_TRUE(!waiting.connected);
  EXPECT_TRUE(!waiting.connecting);
  const wifi_config::SetupFlags connecting =
      wifi_config::setup_flags(State::Connecting);
  EXPECT_TRUE(!connecting.active);
  EXPECT_TRUE(connecting.connecting);
  const wifi_config::SetupFlags connected =
      wifi_config::setup_flags(State::Connected);
  EXPECT_TRUE(!connected.active);
  EXPECT_TRUE(connected.connected);
  const wifi_config::SetupFlags setup = wifi_config::setup_flags(State::SetupAp);
  EXPECT_TRUE(setup.active);
  EXPECT_TRUE(!setup.connecting);
}

HOST_TEST(plan_radio_waiting_stops_hotspot_and_arms_retry_once) {
  using State = StateMachine::State;
  const wifi_config::RadioPlan entered =
      wifi_config::plan_radio(State::Waiting, true, true, true);
  EXPECT_TRUE(entered.stop_ap);
  EXPECT_TRUE(entered.clear_portal_password);
  EXPECT_TRUE(entered.retry_timer == wifi_config::TimerAction::Arm);
  EXPECT_TRUE(entered.connect_timeout == wifi_config::TimerAction::Stop);
  // A later publish while still Waiting must not restart the wait.
  const wifi_config::RadioPlan again =
      wifi_config::plan_radio(State::Waiting, false, false, true);
  EXPECT_TRUE(again.retry_timer == wifi_config::TimerAction::Keep);
  for (const State other :
       {State::Connecting, State::Connected, State::SetupAp}) {
    EXPECT_TRUE(wifi_config::plan_radio(other, true, false, false).retry_timer ==
                wifi_config::TimerAction::Stop);
  }
}

// SetupData::ip feeds the Settings IP row. A lease is not announced when it is
// lost, so the published address is decided by the state, not by whatever
// string was remembered last: only Connected may carry one.
HOST_TEST(published_station_ip_is_empty_unless_connected) {
  using State = StateMachine::State;
  EXPECT_TRUE(wifi_config::published_station_ip(State::Connected,
                                                "192.168.1.210") ==
              "192.168.1.210");
  EXPECT_TRUE(wifi_config::published_station_ip(State::Connecting,
                                                "192.168.1.210")
                  .empty());
  EXPECT_TRUE(wifi_config::published_station_ip(State::SetupAp,
                                                "192.168.1.210")
                  .empty());
  EXPECT_TRUE(wifi_config::published_station_ip(State::Waiting,
                                                "192.168.1.210")
                  .empty());
  // Connected with no address yet (the state flips before the string is
  // stored in no ordering we rely on) stays empty rather than inventing one.
  EXPECT_TRUE(wifi_config::published_station_ip(State::Connected, "").empty());

  // Walk the real machine: every state it can reach, not just the ones named.
  StateMachine machine(true);
  EXPECT_TRUE(wifi_config::published_station_ip(machine.state(), "10.0.0.5")
                  .empty());  // Connecting
  machine.on_connected();
  EXPECT_TRUE(wifi_config::published_station_ip(machine.state(), "10.0.0.5") ==
              "10.0.0.5");
  machine.on_disconnected(false);
  EXPECT_TRUE(wifi_config::published_station_ip(machine.state(), "10.0.0.5")
                  .empty());  // back to Connecting
  machine.on_disconnected(false);
  EXPECT_TRUE(wifi_config::published_station_ip(machine.state(), "10.0.0.5")
                  .empty());  // Waiting
  machine.on_setup_gesture();
  EXPECT_TRUE(wifi_config::published_station_ip(machine.state(), "10.0.0.5")
                  .empty());  // SetupAp
}

// The operator's stranded board: Connected first (which starts the HTTP
// server for firmware uploads), then the network goes and the machine falls
// back to SetupAp. The hotspot must still come up, with a page password -
// the HTTP server already running must not be mistaken for it.
HOST_TEST(plan_radio_starts_hotspot_in_setup_even_when_http_server_already_runs) {
  using State = StateMachine::State;
  const wifi_config::RadioPlan plan = wifi_config::plan_radio(
      State::SetupAp, /*entered=*/true, /*ap_active=*/false,
      /*http_active=*/true);
  EXPECT_TRUE(plan.start_ap);
  EXPECT_TRUE(plan.new_portal_password);
  EXPECT_TRUE(!plan.start_http);
  EXPECT_TRUE(!plan.stop_ap);
  EXPECT_TRUE(plan.connect_timeout == wifi_config::TimerAction::Stop);
}

HOST_TEST(plan_radio_first_setup_starts_hotspot_and_http_server) {
  using State = StateMachine::State;
  const wifi_config::RadioPlan plan =
      wifi_config::plan_radio(State::SetupAp, true, false, false);
  EXPECT_TRUE(plan.start_ap);
  EXPECT_TRUE(plan.start_http);
  EXPECT_TRUE(plan.new_portal_password);
}

// Re-entering SetupAp while the hotspot is still up (a setup attempt that
// failed) keeps the hotspot but still issues a fresh page password.
HOST_TEST(plan_radio_reentering_setup_with_hotspot_up_only_refreshes_password) {
  using State = StateMachine::State;
  const wifi_config::RadioPlan plan =
      wifi_config::plan_radio(State::SetupAp, true, true, true);
  EXPECT_TRUE(!plan.start_ap);
  EXPECT_TRUE(!plan.stop_ap);
  EXPECT_TRUE(plan.new_portal_password);
  const wifi_config::RadioPlan again =
      wifi_config::plan_radio(State::SetupAp, false, true, true);
  EXPECT_TRUE(!again.new_portal_password);
}

// Leaving setup for Connected stops the hotspot and keeps the HTTP server.
HOST_TEST(plan_radio_connected_stops_hotspot_keeps_http_server) {
  using State = StateMachine::State;
  const wifi_config::RadioPlan plan =
      wifi_config::plan_radio(State::Connected, true, true, true);
  EXPECT_TRUE(plan.stop_ap);
  EXPECT_TRUE(!plan.start_http);
  EXPECT_TRUE(plan.clear_portal_password);
  const wifi_config::RadioPlan cold =
      wifi_config::plan_radio(State::Connected, true, false, false);
  EXPECT_TRUE(!cold.stop_ap);
  EXPECT_TRUE(cold.start_http);
}

HOST_TEST(plan_radio_connecting_arms_the_connect_timeout_only) {
  using State = StateMachine::State;
  const wifi_config::RadioPlan plan =
      wifi_config::plan_radio(State::Connecting, true, true, true);
  EXPECT_TRUE(plan.connect_timeout == wifi_config::TimerAction::Arm);
  EXPECT_TRUE(!plan.start_ap);
  EXPECT_TRUE(!plan.stop_ap);
  EXPECT_TRUE(!plan.new_portal_password);
  EXPECT_TRUE(!plan.clear_portal_password);
}
