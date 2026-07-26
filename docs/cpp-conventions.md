# C++ conventions for `src/`

Rules for the green-field tree, from review of the first content-library
commits. `src/` follows all of them; `sc2/` is C, keeps its own conventions
and is not being restyled.

The through-line: **this is a game.** It runs a fixed simulation step against a
frame deadline and ships to WASM, where every allocation, every copy and every
byte of unwind machinery is paid for by someone holding a controller. Code
that only ever runs in a tool is held to a different standard, and the split is
explicit below rather than assumed.

---

## 1. Allocation and copying are the default thing to avoid

Prefer, in order: a compile-time constant, a `constexpr` table, a
`std::array`, a view into memory someone else owns, and only then a container
that allocates.

`std::vector` and `std::string` allocate, and they allocate *especially* as
return values and struct members, where it is easy not to notice. A parser
that returns `vector<Thing>` where each `Thing` holds a `std::string` has
allocated once per field per record before anyone has looked at the data.

- Fixed-size data gets a fixed-size type. A 256-entry palette is
  `std::array<Rgb, 256>`, never a `vector`.
- Tables known at compile time are `constexpr`, not built at startup.
- String literals stay literals. `static constexpr std::string_view` costs
  nothing; a `static const std::string` runs a constructor and allocates.
- If a function needs scratch space, take a caller-provided buffer rather
  than allocating one per call.

**The C++ standard is a tool for this, not a badge.** Raise it whenever a
newer standard removes an allocation or a copy. `src/` is C++23, for three
things that each delete code rather than add it: `std::expected` (rule 2),
`std::byteswap` (an intrinsic in place of shift chains) and `std::format`
(tool-side messages, in place of `ostringstream`). Verified on the local
GCC 15 — `__cpp_lib_expected` 202211, `__cpp_lib_format` 202304,
`__cpp_lib_byteswap` 202110. The CI matrix, Apple clang above all, is the
first exercise of any of it.

## 2. Errors: `std::expected`, not `optional` plus an out-parameter

```cpp
// No.
std::optional<Table> parse (Bytes b, std::string &error);

// Yes.
std::expected<Table, ParseError> parse (Bytes b);
```

The out-parameter idiom forces every caller to declare a string it usually
does not use, gives no way to ignore the error cleanly, and reads worse at
every call site. Where a message really is wanted, build it with
`std::format` at the point it is *shown*, not at the point it is detected.

## 3. Most internal failures are not errors, they are bugs — assert or abort

There is nothing to report and nothing to recover from when a program has run
out of memory, cannot initialise its renderer, or has broken its own
invariant. Handling those paths costs code, obscures the paths that matter and
gets exercised by nothing.

| Kind | Response |
| --- | --- |
| Broken invariant, bad index, impossible state | `assert`, and let the release build fault |
| Allocation failure, failed critical init | abort; there is no game without them |
| Malformed or missing **content** | a real error, with a message |
| Anything a user did | a real error, with a message |

The message budget belongs to the boundaries where the outside world gets in
— the resource manager above all. "`comm.supox.dialogue` names
`base/comm/supox/supox.txt`, which is not there" earns its bytes because a
human has to go fix a file. "entry 3 of 770 bytes overruns the file at 2318"
earns them in `uqm2-browse` and nowhere else.

So: the content layer gets two faces. Parsers report *what* failed as a small
enum the engine can branch on; the browser and the tests turn that into prose.
Engine code never carries the prose.

## 4. No `<fstream>`, no `<sstream>`

`std::ifstream` drags in locales, per-character virtual dispatch and an
exception-configurable state machine to do something the OS does in one call.
`std::istreambuf_iterator` into a `std::string` reads a file one character at
a time through a virtual, then copies the whole thing again.

Read a whole file with a platform call into a caller-owned buffer. Format with
`std::format` into a fixed buffer. String building goes through
`std::format_to_n` on a `std::array`, not `ostringstream`.

## 5. Pass views, not containers

`std::string_view` and `std::span` for anything the callee only reads. That
applies to what parsers *produce*, not only what they consume: a parsed name
is a view into the file buffer, and the buffer outlives it.

Where a view is stored rather than passed, its lifetime is part of the type's
contract and says so in a `LIFETIME:` comment on the type. Every parsed name,
path and phrase body in `engine/content/` is a view into the file buffer the
caller still holds.

One caveat worth knowing, because it cost a test: **a view has to be
contiguous, and a parse result is not always a slice.** The C drops a `#()`
line from a phrase file and concatenates across it, which no single view can
express. In every shipped file the dropped line is followed only by blank
lines, so trailing-trim makes the view exact — and `parsePhrases` reports the
case it cannot represent instead of silently handing back a body with a stray
`#()` inside. When a view cannot model the semantics, say so loudly; do not
quietly approximate.

## 6. Big resources move; they do not copy

Anything wrapping a large allocation — a decoded image, a loaded font, a sound
buffer — is move-only by default. A silent copy of a four-megabyte image is a
frame-time cliff nobody will find by reading.

Make it `= delete` on the copy operations and let the compiler point at the
mistake. If assignment is genuinely needed and the payload is shared and
immutable, a lightweight copy-on-write handle (a refcounted immutable buffer)
is the escape hatch — but reach for move first, and only pay for CoW when
assignment is actually required.

## 7. Give logically-single data a type

Two `int`s that are really a point should be a point. Loose parallel fields
invite transposed arguments that compile fine:

```cpp
// No.
int hotspotX, hotspotY;
std::uint32_t width, height;
void set (std::uint32_t x, std::uint32_t y, Rgb c);

// Yes.
Vec2i hotspot;
Extent2u size;
void set (Vec2u at, Rgb c);
```

Small, `constexpr`, trivially copyable, no virtuals, no allocation:
`Vec2<T>` for points and offsets, `Extent2<T>` for sizes, and a range type for
the `first`/`last` pairs the colour tables are full of. These live in
`engine/core/` so `sim/` can use them without reaching upward.

`Extent2` is deliberately not `Vec2`: adding two sizes is meaningless and
`size.x` reads worse than `size.w`. The type separation is the point.

**Name a type against the convention it breaks.** The range type is
`ClosedRange`, not `Range`, because `[first, last]` is inclusive and every
range in the standard library is half-open — a bare `Range{128, 255}` reads
as half-open to any C++ programmer and is wrong by one everywhere. Closed is
not a preference here, it is what the content is: `SetColorMap` loops
`start <= end`, and a half-open version could not even represent the shipped
content, since `planets/*.ct` ends at 255 and its exclusive end would be 256,
which does not fit the `uint8_t` the format uses. It has no `begin()`/`end()`
either, so it cannot be used in a range-for where the half-open assumption
would be silent.

## 8. Use the language — lambdas and templates where they earn it

The first content-library commits read like C with namespaces: free functions,
out-parameters, hand-rolled loops, a file-static helper for every three-line
job. That is not the house style; it was just habit.

- **Lambdas** for anything used once and locally: a comparator, a predicate, a
  small transform, the body of an algorithm. A lambda next to its only call
  site beats a file-static twenty lines away, and the compiler inlines it
  without a function-pointer indirection.
- **Templates** where a single implementation genuinely serves several types
  — `Vec2<T>`, a big-endian `read<T>`, a table parser parameterised on its
  entry type. Not for its own sake: a template that has exactly one
  instantiation forever is a function with worse error messages.
- **`constexpr`/`consteval`** on anything that can run at compile time, which
  is rule 1 restated with teeth.

These serve the rules above rather than competing with them. A `Vec2<T>` is
how rule 7 stops being three near-identical structs; a `read<T>` is how rule 1
stops being four hand-unrolled shift chains; `if constexpr` is often what lets
a zero-copy view type stay ergonomic.

The limit is the usual one: templates that leak into headers cost compile time
and readability, and clever metaprogramming in a game loop is a liability, not
a flex. Reach for them when they remove duplication or an allocation, not to
demonstrate that they were available.

## 9. Formatting: no space before a call's parenthesis

```cpp
parseBinaryTable(bytes)      // yes
parseBinaryTable (bytes)     // no
```

Applies to calls, declarations and definitions alike. Control-flow keywords
keep their space — `if (x)`, `for (…)`, `while (…)`, `switch (…)` — because
they are not calls.

`sc2/` uses the opposite convention (GNU style, `SetColorMap (map)`) and is
not being restyled; the first `src/` commits copied it out of habit. `src/` is
new code and uses the rule above.

Applied with a tokenizer, not a regex: a plain regex also rewrote English
inside comments ("the plan (docs/…)" → "the plan(docs/…)") and text inside
string literals. The two remaining ` (` in `src/` are a `>` comparison and a
`>>` shift, which are correct as they stand.

---

## Where these live

```
src/platform/File.hpp          rule 4 -- <filesystem> for paths, C stdio for bytes
src/engine/core/Geometry.hpp   rule 7 -- Vec2, Extent2, ClosedRange
src/engine/core/Text.hpp       rules 1, 8 -- forEachLine/forEachField/trim/parseInt,
                                             one each instead of three
src/engine/content/Bytes.hpp        rules 1, 8 -- readBE over std::byteswap
src/engine/content/ContentError.hpp rules 2, 3 -- a code and three numbers, no prose
src/app/browse/main.cpp             where the prose is, and the only place it is
```

The split in rule 3 is visible in the file list: `ContentError` carries an
enum and three integers, and every English sentence about content is formatted
in `uqm2-browse` with `std::format` at the point it is printed.

