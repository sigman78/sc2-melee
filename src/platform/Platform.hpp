// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_PLATFORM_PLATFORM_HPP
#define UQM2_PLATFORM_PLATFORM_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Pacing.hpp"
#include "engine/core/Types.hpp"
#include "engine/input/Input.hpp"

#include <filesystem>
#include <span>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace uqm::platform {

// The SDL3 window, renderer, event pump and clock, single-threaded on
// purpose: a second thread forces SDL_PTHREADS on Emscripten, which forces
// SharedArrayBuffer and COOP/COEP headers the M1 URL-deploy goal can't pay.
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

// Two players at one keyboard (M1): player 0 is arrow keys and right-hand
// modifiers, player 1 is WASD and left-hand. No rebinding or config file
// yet -- that would just produce one of these tables.
[[nodiscard]] std::span<const Binding> defaultBindings() noexcept;

// The directory the executable lives in: "where is the content" cannot be
// answered from the working directory alone, since a program launched from
// Explorer or a build tree has one that says nothing about install location.
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

	// Drains the event queue into `players`; false once the user quits. Key
	// repeat is dropped -- the accumulator's sticky bit already guarantees a
	// press is seen without depending on a system repeat rate.
	[[nodiscard]] bool pump(std::span<input::InputAccumulator> players,
			std::span<const Binding> bindings) noexcept;

	// Monotonic, in 840ths of a second, zero at construction.
	[[nodiscard]] Ticks now() const noexcept;

	[[nodiscard]] SDL_Renderer *renderer() const noexcept { return renderer_; }

	void clear(u8 r, u8 g, u8 b) noexcept;
	void fillRect(Vec2i topLeft, Extent2u size, u8 r,
			u8 g, u8 b) noexcept;

	// Uploads 8-bit RGBA. Returns an invalid Texture if `rgba` is the wrong
	// size for `size`, which is a content bug and worth not crashing over.
	[[nodiscard]] Texture upload(
			Extent2u size, std::span<const u8> rgba) noexcept;

	// Draws `t` scaled into `dest` pixels at `topLeft` (nearest-neighbour, set
	// at upload -- this is pixel art). `alpha` is 255 for opaque; used for the
	// Ilwrath cloak, the only thing drawn partly-there so far.
	void draw(const Texture &t, Vec2i topLeft, Extent2u dest,
			u8 alpha = 255) noexcept;

	// Draws `t` multiplied by a colour. With a white-filled texture that is a
	// flat silhouette of any colour, which is what the C's STAMPFILL_PRIM is.
	void drawTinted(const Texture &t, Vec2i topLeft, Extent2u dest,
			u8 r, u8 g, u8 b) noexcept;

	// A one-pixel line. Only the debug overlay uses this -- the game draws
	// sprites -- but drawing a vector is how you see a vector.
	void drawLine(Vec2i from, Vec2i to, u8 r, u8 g,
			u8 b) noexcept;

	void present() noexcept;

private:
	SDL_Window *window_ = nullptr;
	SDL_Renderer *renderer_ = nullptr;
	u64 startNs_ = 0;
};

}  // namespace uqm::platform

#endif  // UQM2_PLATFORM_PLATFORM_HPP
