#include "settings_menu.hpp"

namespace ui {
namespace {

VolumePreset g_volume_preset = VolumePreset::Medium;
void (*g_volume_preset_store)(VolumePreset) = nullptr;
QuietHoursPreset g_quiet_hours_preset = QuietHoursPreset::From23To07;
void (*g_quiet_hours_store)(QuietHoursPreset) = nullptr;

// Runtime, Battery, and Firmware are display-only: activate() returns None
// for all three (see below), so pressing select on one does nothing. That
// used to mean the row still ate a press for nothing; skipping them here
// means the cursor can never land on one in the first place. Keyed off
// activate()'s own behaviour by listing the same three rows, not off
// position in the enum, so a future reorder cannot silently make one
// focusable again without also making it do something when selected.
constexpr bool is_focusable(SettingsItem item) {
  switch (item) {
    case SettingsItem::Runtime:
    case SettingsItem::Battery:
    case SettingsItem::Firmware:
      return false;
    default:
      return true;
  }
}

}  // namespace

VolumePreset volume_preset() { return g_volume_preset; }

void set_volume_preset(VolumePreset value) {
  if (value >= VolumePreset::Count || value == g_volume_preset) return;
  g_volume_preset = value;
  if (g_volume_preset_store != nullptr) g_volume_preset_store(value);
}

void set_volume_preset_store_handler(void (*handler)(VolumePreset value)) {
  g_volume_preset_store = handler;
}

const char* volume_preset_name(VolumePreset value) {
  switch (value) {
    case VolumePreset::Off:
      return text(Text::VolumeOff);
    case VolumePreset::Low:
      return text(Text::VolumeLow);
    case VolumePreset::Medium:
      return text(Text::VolumeMedium);
    case VolumePreset::High:
      return text(Text::VolumeHigh);
    case VolumePreset::Count:
      break;
  }
  return "";
}

QuietHoursPreset quiet_hours_preset() { return g_quiet_hours_preset; }

void set_quiet_hours_preset(QuietHoursPreset value) {
  if (value >= QuietHoursPreset::Count || value == g_quiet_hours_preset) return;
  g_quiet_hours_preset = value;
  if (g_quiet_hours_store != nullptr) g_quiet_hours_store(value);
}

void set_quiet_hours_store_handler(void (*handler)(QuietHoursPreset value)) {
  g_quiet_hours_store = handler;
}

const char* quiet_hours_preset_name(QuietHoursPreset value) {
  switch (value) {
    case QuietHoursPreset::Off:
      return text(Text::QuietHoursOff);
    case QuietHoursPreset::From22To07:
      return text(Text::QuietHours22To07);
    case QuietHoursPreset::From23To07:
      return text(Text::QuietHours23To07);
    case QuietHoursPreset::From00To07:
      return text(Text::QuietHours00To07);
    case QuietHoursPreset::Count:
      break;
  }
  return "";
}

bool in_quiet_hours(QuietHoursPreset preset, uint8_t local_hour) {
  constexpr uint8_t kEnd = 7;
  switch (preset) {
    case QuietHoursPreset::From22To07:
      return local_hour >= 22 || local_hour < kEnd;
    case QuietHoursPreset::From23To07:
      return local_hour >= 23 || local_hour < kEnd;
    case QuietHoursPreset::From00To07:
      return local_hour < kEnd;
    case QuietHoursPreset::Off:
    case QuietHoursPreset::Count:
      break;
  }
  return false;
}

void SettingsMenu::focus_next() {
  // Bounded by count(): every non-focusable item skipped is one fewer than
  // count() possible stops, so this always finds the next focusable row
  // (there is always at least one - Language) without ever spinning past
  // a full lap.
  std::size_t next = focused_index();
  for (std::size_t step = 0; step < count(); ++step) {
    next = (next + 1) % count();
    if (is_focusable(static_cast<SettingsItem>(next))) break;
  }
  focused_ = static_cast<SettingsItem>(next);
}

SettingsAction SettingsMenu::activate() {
  switch (focused_) {
    case SettingsItem::Language: {
      const auto next = static_cast<Language>(
          (static_cast<std::size_t>(ui::language()) + 1) %
          static_cast<std::size_t>(Language::Count));
      set_language(next);
      return SettingsAction::LanguageChanged;
    }
    case SettingsItem::Volume: {
      const auto next = static_cast<VolumePreset>(
          (static_cast<std::size_t>(ui::volume_preset()) + 1) %
          static_cast<std::size_t>(VolumePreset::Count));
      set_volume_preset(next);
      return SettingsAction::VolumeChanged;
    }
    case SettingsItem::QuietHours: {
      const auto next = static_cast<QuietHoursPreset>(
          (static_cast<std::size_t>(ui::quiet_hours_preset()) + 1) %
          static_cast<std::size_t>(QuietHoursPreset::Count));
      set_quiet_hours_preset(next);
      return SettingsAction::QuietHoursChanged;
    }
    case SettingsItem::WifiSetup:
      return SettingsAction::EnterWifiSetup;
    case SettingsItem::CheckUpdates:
      // One row, two jobs: it asks until there is an answer, then it offers.
      // A separate install row would sit there inert most of the time and
      // still need the check to have run first.
      return update_offered_ ? SettingsAction::StartUpdateInstall
                             : SettingsAction::StartUpdateCheck;
    case SettingsItem::Runtime:
    case SettingsItem::Battery:
    case SettingsItem::Firmware:
      // Display only, and unreachable via focus_next() (see is_focusable()
      // above) - this branch is defensive/exhaustiveness only, not a path
      // the UI can actually take. Kept returning None rather than removed:
      // if a setter for focused_ is ever added, this stays correct.
      return SettingsAction::None;
    case SettingsItem::Count:
      break;
  }
  return SettingsAction::None;
}

Text settings_item_label(SettingsItem item) {
  switch (item) {
    case SettingsItem::Language:
      return Text::SettingsLanguage;
    case SettingsItem::Volume:
      return Text::SettingsVolume;
    case SettingsItem::QuietHours:
      return Text::SettingsQuietHours;
    case SettingsItem::WifiSetup:
      return Text::SettingsWifiSetup;
    case SettingsItem::CheckUpdates:
      return Text::SettingsCheckUpdates;
    case SettingsItem::Runtime:
      return Text::SettingsRuntime;
    case SettingsItem::Battery:
      return Text::SettingsBattery;
    case SettingsItem::Firmware:
      return Text::SettingsFirmware;
    case SettingsItem::Count:
      break;
  }
  return Text::SettingsTitle;
}

InputHints input_hints(InputContext context) {
  switch (context) {
    case InputContext::Carousel:
      return {Text::HintPrevPage, Text::HintNextPage, true};
    case InputContext::Menu:
      return {Text::HintNextItem, Text::HintSelect, true};
    case InputContext::Locked:
      // Naming buttons that are deliberately ignored would be worse than
      // silence: it invites the one interaction that must not happen while
      // flash is being written.
      return {Text::HintNextItem, Text::HintSelect, false};
  }
  return {Text::HintNextItem, Text::HintSelect, false};
}

}  // namespace ui
