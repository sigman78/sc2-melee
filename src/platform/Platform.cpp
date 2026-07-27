// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Platform.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <cstdlib>

namespace uqm::platform {
namespace {

[[noreturn]] void
fatal(const char *what) noexcept
{
	// Rule 3: a failure here is not recoverable and there is nothing useful a
	// caller could do with it. Say what broke, say what SDL said, stop.
	std::fprintf(stderr, "fatal: %s: %s\n", what, SDL_GetError());
	std::abort();
}

constexpr std::array<Binding, 12> kDefaultBindings{{
		// Player 0: arrow keys, right-hand modifiers.
		{SDL_SCANCODE_LEFT, 0, input::Button::Left},
		{SDL_SCANCODE_RIGHT, 0, input::Button::Right},
		{SDL_SCANCODE_UP, 0, input::Button::Thrust},
		{SDL_SCANCODE_RCTRL, 0, input::Button::Weapon},
		{SDL_SCANCODE_RSHIFT, 0, input::Button::Special},
		{SDL_SCANCODE_ESCAPE, 0, input::Button::Escape},

		// Player 1: WASD, left-hand modifiers.
		{SDL_SCANCODE_A, 1, input::Button::Left},
		{SDL_SCANCODE_D, 1, input::Button::Right},
		{SDL_SCANCODE_W, 1, input::Button::Thrust},
		{SDL_SCANCODE_LCTRL, 1, input::Button::Weapon},
		{SDL_SCANCODE_LSHIFT, 1, input::Button::Special},
		{SDL_SCANCODE_TAB, 1, input::Button::Escape},
}};

}  // namespace

std::span<const Binding>
defaultBindings() noexcept
{
	return kDefaultBindings;
}

std::filesystem::path
executableDirectory()
{
	// SDL owns the returned string and keeps it for the process lifetime, so
	// there is nothing to free and nothing to outlive.
	const char *base = SDL_GetBasePath();
	return base != nullptr ? std::filesystem::path(base)
						   : std::filesystem::current_path();
}

Platform::Platform(const char *title, Extent2u logical, int scale)
{
	// Video only. No audio yet, and deliberately no SDL_INIT_EVENTS-adjacent
	// subsystems we are not using -- each one is a thread or a device poll we
	// would then have to reason about.
	if (!SDL_Init(SDL_INIT_VIDEO))
		fatal("SDL_Init");

	window_ = SDL_CreateWindow(title,
			static_cast<int>(logical.w) * scale,
			static_cast<int>(logical.h) * scale, SDL_WINDOW_RESIZABLE);
	if (window_ == nullptr)
		fatal("SDL_CreateWindow");

	renderer_ = SDL_CreateRenderer(window_, nullptr);
	if (renderer_ == nullptr)
		fatal("SDL_CreateRenderer");

	// Everything downstream draws in 320x240 and SDL does the scaling. This is
	// the decision the plan wanted taken in M1 or never: the world is 320x240
	// and the window is a presentation detail. Letterbox rather than stretch,
	// so the aspect cannot change under the game.
	if (!SDL_SetRenderLogicalPresentation(renderer_,
				static_cast<int>(logical.w),
				static_cast<int>(logical.h),
				SDL_LOGICAL_PRESENTATION_LETTERBOX))
		fatal("SDL_SetRenderLogicalPresentation");

	startNs_ = SDL_GetTicksNS();
}

Platform::~Platform()
{
	if (renderer_ != nullptr)
		SDL_DestroyRenderer(renderer_);
	if (window_ != nullptr)
		SDL_DestroyWindow(window_);
	SDL_Quit();
}

Ticks
Platform::now() const noexcept
{
	const std::uint64_t ns = SDL_GetTicksNS() - startNs_;

	// Split rather than multiplying first. ns * 840 overflows a signed 64-bit
	// value after about three hours of uptime, which is well inside a long
	// session -- sdltime.c:26-27 splits for the same reason.
	constexpr std::uint64_t kNsPerSecond = 1000000000;
	return static_cast<Ticks>((ns / kNsPerSecond) * kOneSecond
			+ (ns % kNsPerSecond) * kOneSecond / kNsPerSecond);
}

bool
Platform::pump(std::span<input::InputAccumulator> players,
		std::span<const Binding> bindings) noexcept
{
	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		switch (e.type)
		{
			case SDL_EVENT_QUIT:
				return false;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				// Drop everything, including the sticky half. A key pressed
				// while the window was not focused must not fire on return,
				// and a key held when focus was lost will not send its release.
				for (input::InputAccumulator &p : players)
					p.reset();
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
			{
				if (e.key.repeat)
					break;
				const bool down = e.type == SDL_EVENT_KEY_DOWN;
				for (const Binding &b : bindings)
				{
					if (b.scancode != static_cast<int>(e.key.scancode))
						continue;
					if (b.player < 0
							|| static_cast<std::size_t>(b.player)
									>= players.size())
						continue;
					if (down)
						players[static_cast<std::size_t>(b.player)].press(
								b.button);
					else
						players[static_cast<std::size_t>(b.player)].release(
								b.button);
				}
				break;
			}

			default:
				break;
		}
	}
	return true;
}

void
Platform::clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
	SDL_SetRenderDrawColor(renderer_, r, g, b, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(renderer_);
}

void
Platform::fillRect(Vec2i topLeft, Extent2u size, std::uint8_t r,
		std::uint8_t g, std::uint8_t b) noexcept
{
	SDL_SetRenderDrawColor(renderer_, r, g, b, SDL_ALPHA_OPAQUE);
	const SDL_FRect rect{static_cast<float>(topLeft.x),
			static_cast<float>(topLeft.y), static_cast<float>(size.w),
			static_cast<float>(size.h)};
	SDL_RenderFillRect(renderer_, &rect);
}

Texture::~Texture()
{
	if (handle_ != nullptr)
		SDL_DestroyTexture(handle_);
}

Texture::Texture(Texture &&other) noexcept
	: handle_(other.handle_), size_(other.size_)
{
	other.handle_ = nullptr;
	other.size_ = Extent2u{};
}

Texture &
Texture::operator=(Texture &&other) noexcept
{
	if (this != &other)
	{
		if (handle_ != nullptr)
			SDL_DestroyTexture(handle_);
		handle_ = other.handle_;
		size_ = other.size_;
		other.handle_ = nullptr;
		other.size_ = Extent2u{};
	}
	return *this;
}

Texture
Platform::upload(Extent2u size, std::span<const std::uint8_t> rgba) noexcept
{
	Texture t;
	const std::size_t need = static_cast<std::size_t>(size.w) * size.h * 4;
	if (size.empty() || rgba.size() < need)
		return t;

	SDL_Texture *tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888,
			SDL_TEXTUREACCESS_STATIC, static_cast<int>(size.w),
			static_cast<int>(size.h));
	if (tex == nullptr)
		return t;

	SDL_UpdateTexture(tex, nullptr, rgba.data(), static_cast<int>(size.w) * 4);
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	// Pixel art. Smoothing it looks wrong at every zoom, and at the 2:1 and
	// 4:1 steps it turns the sprites to mush.
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

	t.handle_ = tex;
	t.size_ = size;
	return t;
}

void
Platform::draw(const Texture &t, Vec2i topLeft, Extent2u dest) noexcept
{
	if (!t.valid() || dest.empty())
		return;
	const SDL_FRect to{static_cast<float>(topLeft.x),
			static_cast<float>(topLeft.y), static_cast<float>(dest.w),
			static_cast<float>(dest.h)};
	SDL_RenderTexture(renderer_, t.handle_, nullptr, &to);
}

void
Platform::present() noexcept
{
	SDL_RenderPresent(renderer_);
}

}  // namespace uqm::platform
