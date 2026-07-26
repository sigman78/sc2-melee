# C++ conventions for `src/`

Rules for the green-field tree, from review of the first content-library
commits. They are written down because the code that prompted them is still
in the tree and does not follow them — see "Where the tree stands today" at
the end, which is the work list.

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
newer standard removes an allocation or a copy. C++23 is confirmed on the
local GCC 15 (`__cpp_lib_expected` 202211, `__cpp_lib_format` 202304); the CI
matrix — Apple clang above all — has to be checked before the tree depends on
either.

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
contract and is stated in a comment. `BinaryTable` already holds spans into a
caller's buffer; that is the right shape and the wrong amount of documentation.

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
`engine/` (or lower) so `sim/` can use them without reaching upward.

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

---

## Where the tree stands today

Written after the review, against `src/` as committed. Each is a defect
against the rules above, not a style preference.

**Allocation and copying (1, 5)**

- `ColorTableEntry::palettes` is `std::vector<std::array<Rgb, 256>>`. Every
  palette is 768 bytes and `base/uqm.ct` holds 136 of them, so parsing one
  entry allocates and fills 104 KB that already exists, laid out identically,
  in the source buffer. It should be a view.
- `Cel::file`, `Phrase::{name,text,clip,timestamps}` and
  `Resource::{type,path}` are all `std::string` copied out of a buffer the
  caller already holds. 5,034 phrases × 4 strings is a lot of `malloc` to
  answer "what is phrase 12 called".
- `ResourceMap` is a `std::map<std::string, Resource>` — a node per key, 963
  nodes, plus three strings each. A sorted flat array of views would be one
  allocation and faster to search.
- Every error path builds its message with `std::string` `operator+` chains.
  Those allocate on failure and cost code size always.

**Errors (2, 3)**

- Every parser is `std::optional<T> (…, std::string &error)`. All should be
  `std::expected<T, E>` with a small `E`, and the prose should move to
  `uqm2-browse` and the tests.
- `Canvas::set` silently drops out-of-bounds writes; that is an assert.
- `readFile` returns an empty vector for "missing", "empty" and "unreadable"
  alike, so callers cannot tell them apart and mostly do not try.

**`<fstream>` (4)**

- `readFile`/`readText`/`writeFile` in both `tests/content_test.cpp` and
  `src/app/browse/main.cpp` use `ifstream`/`ofstream` with
  `istreambuf_iterator`. Tool code, so lowest priority — but the same helper
  will be wanted in the engine, and it should not be this one.

**Ownership (6)**

- `PngImage` holds up to megabytes in two vectors and is freely copyable.
  Returning it by value moves, which is why nothing has gone wrong yet; one
  `auto img = …` instead of `auto &img` is all it takes.

**Compound types (7)**

- `Cel::hotspotX/hotspotY`, `PngImage::width/height`, `Canvas::width/height`
  and `Canvas::set(x, y, …)`, `Sheet::cellW/cellH`,
  `ColorTableEntry::first/last`. None of these has a type yet.

**Language use (8)**

- `Bytes.hpp` hand-writes `readU8` and `readU32BE` as separate shift chains;
  one `constexpr` `readBE<T>` covers both and the 16-bit case nobody has
  needed yet.
- `AniFile.cpp` has file-static `isSpace`, `fields` and `parseInt` used once
  each; `PhraseFile.cpp` has `trimRight`, `nextLine`, `strtokStep`. Several
  want to be lambdas, and the line-splitting duplicated between
  `AniFile.cpp`, `PhraseFile.cpp` and `ResourceMap.cpp` wants to be one
  `forEachLine` taking a callable.
- Three separate "parse an integer / trim / split" implementations exist
  across the four parsers.
