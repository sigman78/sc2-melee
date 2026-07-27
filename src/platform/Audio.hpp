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

// Sound effects, via SDL3.
//
// **This replaces the lock-free ring, and the ring is deleted.** I wrote that
// ring on the reasoning that SDL's audio callback runs on a device thread and
// must never see game state -- which is true, and remains the constraint. What
// I had not checked was that SDL3 already solves it: SDL_AudioStream is fed
// from whatever thread you like, mixes any number of streams bound to one
// device, and owns the callback entirely. There is no callback of ours for
// game state to leak into, so there is nothing left for the ring to protect.
//
// Keeping it because it was written and tested would have been carrying a
// second, worse answer to a solved problem. The threading commitment is
// unchanged and is in fact stronger: nothing in src/ starts a thread, and now
// nothing in src/ has an atomic either.
//
// The .snd format helps as much as SDL does: it is a plain text list of .wav
// filenames, one per line, exactly as .ani lists .png. SDL_LoadWAV reads those
// directly, so the whole sound-effect path needs no decoder. Music is .mod and
// voice is .ogg; both need real decoders and neither is M1.

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

	// Starts `s` on the next free stream. Overlapping sounds need overlapping
	// streams, so there is a small pool and the oldest is recycled -- a flame
	// firing every frame would otherwise queue itself into an ever-growing
	// delay.
	void play(const Sound &s);

private:
	std::vector<SDL_AudioStream *> streams_;
	std::size_t next_ = 0;
};

}  // namespace uqm::platform

#endif  // UQM2_PLATFORM_AUDIO_HPP
