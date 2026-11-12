#pragma once

#include <cstddef>
#include <cstdint>

// The note-sequence vocabulary and its one piece of arithmetic, split out of
// audio.hpp for the same reason audio_drain.hpp is: audio.hpp is entirely
// #ifdef ESP_PLATFORM (it needs esp_err.h), but a caller wants to
// static_assert that a melody fits the sound budget, and a host test wants to
// check the sum, and neither can do that through a header that disappears off
// the device. audio.hpp includes this, so callers only ever see audio::Note.
// It is defined whether or not CONFIG_AUDIO_ENABLE is set - pure arithmetic,
// no codec - so a melody table compiles identically with audio off.
namespace audio {

// One step of a sequence. frequency_hz == 0 is a rest (silence of that
// length, amplifier still enabled, so a gap inside a melody does not toggle
// GPIO46 and cannot pop).
struct Note {
  int frequency_hz;
  int duration_ms;
};

// Total length of the notes and rests, in milliseconds. Excludes the fixed
// lead/trail silence and drain wait audio_play_notes() adds around the
// sequence. constexpr so a melody table can be static_assert-ed.
constexpr uint32_t notes_duration_ms(const Note* notes, size_t count) {
  uint32_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    total += static_cast<uint32_t>(notes[i].duration_ms);
  }
  return total;
}

}  // namespace audio
