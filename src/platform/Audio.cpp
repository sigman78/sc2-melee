// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Audio.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <utility>

namespace uqm::platform {

namespace {

// Enough for the shots, hits and explosions a two-ship melee produces at once.
// The Avenger's flame fires every frame, so this is chosen to be comfortably
// more than one ship can start in the time a short effect lasts.
constexpr std::size_t kStreamCount = 12;

}  // namespace

Sound::~Sound()
{
	if (data_ != nullptr)
		SDL_free(data_);
}

Sound::Sound(Sound &&other) noexcept
	: data_(other.data_)
	, length_(other.length_)
	, format_(other.format_)
	, channels_(other.channels_)
	, rate_(other.rate_)
{
	other.data_ = nullptr;
	other.length_ = 0;
}

Sound &
Sound::operator=(Sound &&other) noexcept
{
	if (this != &other)
	{
		if (data_ != nullptr)
			SDL_free(data_);
		data_ = other.data_;
		length_ = other.length_;
		format_ = other.format_;
		channels_ = other.channels_;
		rate_ = other.rate_;
		other.data_ = nullptr;
		other.length_ = 0;
	}
	return *this;
}

Audio::Audio()
{
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		// Not fatal. A machine with no sound device still plays the game.
		SDL_Log("audio: %s", SDL_GetError());
		return;
	}

	// One spec for the whole pool. SDL converts each sound into it, which is
	// what lets a 22kHz mono effect and a 44kHz stereo one play together
	// without any mixing code here.
	SDL_AudioSpec spec{};
	spec.format = SDL_AUDIO_S16;
	spec.channels = 2;
	spec.freq = 44100;

	streams_.reserve(kStreamCount);
	for (std::size_t i = 0; i < kStreamCount; ++i)
	{
		SDL_AudioStream *s = SDL_OpenAudioDeviceStream(
				SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (s == nullptr)
			break;
		SDL_ResumeAudioStreamDevice(s);
		streams_.push_back(s);
		playing_.push_back(nullptr);
	}

	if (streams_.empty())
		SDL_Log("audio: no streams: %s", SDL_GetError());
}

Audio::~Audio()
{
	for (SDL_AudioStream *s : streams_)
		SDL_DestroyAudioStream(s);
}

Sound
Audio::load(const std::filesystem::path &wav) const
{
	Sound out;

	SDL_AudioSpec spec{};
	std::uint8_t *data = nullptr;
	std::uint32_t length = 0;
	if (!SDL_LoadWAV(wav.string().c_str(), &spec, &data, &length))
		return out;

	out.data_ = data;
	out.length_ = length;
	out.format_ = static_cast<int>(spec.format);
	out.channels_ = spec.channels;
	out.rate_ = spec.freq;
	return out;
}

void
Audio::play(const Sound &s, float gain)
{
	if (streams_.empty() || !s.valid())
		return;

	// If this sound is already playing, restart *that* stream rather than
	// taking another. One voice per effect: see the header for why this, and
	// not the gain, is what governs how loud a melee is.
	std::size_t slot = streams_.size();
	for (std::size_t i = 0; i < streams_.size(); ++i)
	{
		if (playing_[i] == &s && SDL_GetAudioStreamAvailable(streams_[i]) > 0)
		{
			slot = i;
			break;
		}
	}
	if (slot == streams_.size())
	{
		slot = next_;
		next_ = (next_ + 1) % streams_.size();
	}

	SDL_AudioStream *stream = streams_[slot];
	playing_[slot] = &s;

	// Clear before putting. Without it a sound that fires every frame queues
	// behind itself and drifts further and further from the thing that caused
	// it -- the flame would be seconds behind the picture within a few
	// seconds of holding the trigger.
	SDL_ClearAudioStream(stream);

	SDL_AudioSpec src{};
	src.format = static_cast<SDL_AudioFormat>(s.format_);
	src.channels = s.channels_;
	src.freq = s.rate_;
	SDL_SetAudioStreamFormat(stream, &src, nullptr);

	SDL_SetAudioStreamGain(stream, gain);
	SDL_PutAudioStreamData(stream, s.data_, static_cast<int>(s.length_));
}

}  // namespace uqm::platform
