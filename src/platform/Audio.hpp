// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_PLATFORM_AUDIO_HPP
#define UQM2_PLATFORM_AUDIO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

struct SDL_AudioStream;

namespace uqm::platform {

// Sound effects, via SDL3. SDL_AudioStream can be fed from any thread and
// owns its own mixing, so audio needs no thread and no atomic in src/ --
// the constraint that a device callback must never see game state.
//
// .snd is a plain text list of .wav filenames, one per line (like .ani
// lists .png), so SDL_LoadWAV covers the whole sound-effect path with no
// decoder. Music (.mod) and voice (.ogg) need real decoders; not M1.

// One decoded effect, held in memory. Move-only: it owns a buffer SDL
// allocated.
class Sound
{
public:
	Sound() = default;
	~Sound();

	Sound(Sound &&other) noexcept;
	Sound &operator=(Sound &&other) noexcept;
	Sound(const Sound &) = delete;
	Sound &operator=(const Sound &) = delete;

	[[nodiscard]] bool valid() const noexcept { return length_ != 0; }

private:
	friend class Audio;

	std::uint8_t *data_ = nullptr;
	std::uint32_t length_ = 0;

	// SDL_AudioSpec by value would drag SDL.h into this header for three
	// fields, so it is kept opaque and unpacked in the .cpp.
	int format_ = 0;
	int channels_ = 0;
	int rate_ = 0;
};

class Audio
{
public:
	// Opening the device is allowed to fail: a machine with no sound card is
	// not a reason to refuse to run the game. Everything else then no-ops.
	Audio();
	~Audio();

	Audio(const Audio &) = delete;
	Audio &operator=(const Audio &) = delete;

	[[nodiscard]] bool valid() const noexcept { return !streams_.empty(); }

	// Empty Sound if the file will not load, which the caller can still
	// "play" harmlessly.
	[[nodiscard]] Sound load(const std::filesystem::path &wav) const;

	// Starts `s` on the stream already playing it, if any -- one voice per
	// distinct sound governs loudness; round-robin alone lets a fast-repeating
	// effect stack copies additively. Matches the C's per-source channel (sound.c).
	//
	// `gain` is a multiplier, 1.0 being the file's own level.
	void play(const Sound &s, float gain = 1.0f);

private:
	std::vector<SDL_AudioStream *> streams_;

	// Which sound each stream last started, parallel to `streams_`. Compared
	// by address: two Sounds are the same effect exactly when they are the
	// same object, since the cache hands out references into it.
	std::vector<const Sound *> playing_;
	std::size_t next_ = 0;
};

}  // namespace uqm::platform

#endif  // UQM2_PLATFORM_AUDIO_HPP
