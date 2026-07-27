// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_PLATFORM_PLATFORM_HPP
#define UQM2_PLATFORM_PLATFORM_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Pacing.hpp"
#include "engine/input/Input.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace uqm::platform {

// The SDL3 window, renderer, event pump and clock -- the whole of what the
// game needs from the host, in one place.
//
// Single-threaded, and that is a commitment rather than a convenience. The C
// runs the game on Starcon2Main with a separate TFB render thread and a
// draw-command queue between them, which on Emscripten forces SDL_PTHREADS on,
// which forces SharedArrayBuffer, which forces the host to send COOP/COEP
// headers before the page will even load. Deploying to a URL is an M1 exit
// criterion, so that chain is a real cost and it is paid for a thread the
// rewrite does not want. Nothing here starts one.
//
// SDL types stay behind forward declarations: `renderer()` is the one place
// they leak, and only into whoever draws.

// One key, bound to one player's button. Scancode is kept as a plain int so
// SDL.h does not have to be included here.
struct Binding
{
	int scancode;
	int player;
	input::Button button;
};

// Two players at one keyboard, which is what M1 asks for. Player 0 has the
// arrow keys and the right-hand modifiers; player 1 has WASD and the
// left-hand ones. There is no rebinding yet and no config file -- when that
// arrives it produces one of these tables and nothing else changes.
[[nodiscard]] std::span<const Binding> defaultBindings() noexcept;

// The directory the executable lives in. Needed because "where is the content"
// cannot be answered from the working directory alone: a program launched from
// Explorer, or from a build tree, has a working directory that says nothing
// about where it was installed.
[[nodiscard]] std::filesystem::path executableDirectory();

// An uploaded image. Move-only, because it owns a GPU resource and there is
// no sane copy (docs/cpp-conventions.md rule 6).
class Texture
{
public:
	Texture() = default;
	~Texture();

	Texture(Texture &&other) noexcept;
	Texture &operator=(Texture &&other) noexcept;
	Texture(const Texture &) = delete;
	Texture &operator=(const Texture &) = delete;

	[[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
	[[nodiscard]] Extent2u size() const noexcept { return size_; }

private:
	friend class Platform;

	SDL_Texture *handle_ = nullptr;
	Extent2u size_;
};

class Platform
{
public:
	// Aborts on failure. A window we cannot open is not a condition anything
	// can usefully recover from, and reporting it upward would only add a
	// branch every caller writes the same way (docs/cpp-conventions.md rule 3).
	Platform(const char *title, Extent2u logical, int scale);
	~Platform();

	Platform(const Platform &) = delete;
	Platform &operator=(const Platform &) = delete;

	// Drains the event queue into `players`. Returns false once the user has
	// asked to quit.
	//
	// Key repeat is dropped: the accumulator's sticky bit already guarantees a
	// press is seen, and letting the OS repeat rate inject extra presses would
	// make held-vs-tapped depend on a system setting.
	[[nodiscard]] bool pump(std::span<input::InputAccumulator> players,
			std::span<const Binding> bindings) noexcept;

	// Monotonic, in 840ths of a second, zero at construction.
	[[nodiscard]] Ticks now() const noexcept;

	[[nodiscard]] SDL_Renderer *renderer() const noexcept { return renderer_; }

	void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;
	void fillRect(Vec2i topLeft, Extent2u size, std::uint8_t r,
			std::uint8_t g, std::uint8_t b) noexcept;

	// Uploads 8-bit RGBA. Returns an invalid Texture if `rgba` is the wrong
	// size for `size`, which is a content bug and worth not crashing over.
	[[nodiscard]] Texture upload(
			Extent2u size, std::span<const std::uint8_t> rgba) noexcept;

	// Draws `t` scaled into `dest` pixels at `topLeft`. Nearest-neighbour, set
	// at upload: this is pixel art and smoothing it looks wrong.
	void draw(const Texture &t, Vec2i topLeft, Extent2u dest) noexcept;

	void present() noexcept;

private:
	SDL_Window *window_ = nullptr;
	SDL_Renderer *renderer_ = nullptr;
	std::uint64_t startNs_ = 0;
};

}  // namespace uqm::platform

#endif  // UQM2_PLATFORM_PLATFORM_HPP
