# Content formats

What the content pack actually contains, read out of the C loaders and then
checked against every file in `sc2/content`. This is the specification the
rewrite's content library is written from (`docs/game-rewrite-plan.md`, M0).

Every count below was produced by reading the tree, not estimated. Where the C
does something surprising, the surprise is recorded rather than tidied away —
the point of doing this first is to find them before M1 depends on them.

---

## The resource map — `uqm.rmp`

Line-oriented `key = TYPE:path`, e.g.

```
comm.supox.dialogue = CONVERSATION:base/comm/supox/supox.txt
comm.supox.colortable = BINTAB:base/comm/supox/supox.ct
```

Keys are dotted, paths are relative to the content root. The type tag selects
the loader, registered in `sresins.c:36-38` and friends. 963 resources in the
shipped map:

| Type | Count | Value |
| --- | --- | --- |
| `GFXRES` | 582 | path to an `.ani` |
| `BINTAB` | 149 | path to a `.ct` |
| `MUSICRES` | 64 | path to a `.mod`/`.ogg` |
| `STRTAB` | 51 | path to a `.txt` |
| `FONTRES` | 32 | path to a `.fon` **directory** |
| `SNDRES` | 30 | path to a `.snd` |
| `SHIP` | 28 | **not a path** — see below |
| `CONVERSATION` | 27 | path to a race `.txt` |

**`SHIP` values are not paths.** `ship.supox.code = SHIP:16` names index 16 in
a table compiled into the binary; `dummy.c:157` routes the type to
`GetCodeResData`, because a ship's code is code. A tool that assumes every
value is a path reports all 28 as dangling — which is exactly how this
exception surfaced, in the browser's inventory on its first run.

Names in the map are the only link between code and content. `resinst.h` per
race turns a C identifier into a key (`#define SUPOX_COLOR_MAP
"comm.supox.colortable"`), and the map turns the key into a path. Race source
directories and content directories are *not* the same names — `blackur/`
loads `comm.kohrah.*`, `slyland/` loads `comm.probe.*`, `rebel/` loads
`comm.yehat.rebel.*` — and three source directories (`rebel`, `spahome`,
`starbas`) have no `resinst.h` at all and include a sibling's.

---

## The binary container — `GetResourceData` + string tables

Two layers, and they are easy to mistake for one.

**Layer 1, `loadres.c:25-53`.** A resource file begins with a 4-byte length
prefix. `0xFFFFFFFF` means "uncompressed, the rest is data". Any other value
meant LZ-compressed in the original and is now refused outright. The comment
there is worth keeping: *"Currently, .ct and .xlt files still carry a ~0 length
prefix"* — so this layer applies to those, not to every resource.

**Layer 2, `getstr.c:606-642`.** What follows is a **binary string table**: an
array of big-endian `DWORD`s, then the entry bytes back to back.

```
u32  count            number of entries
u32  extra            additional DWORDs to skip before the data (always 0 today)
u32  length[count]    byte length of each entry
...  entry data       count entries, concatenated, no padding
```

Entry data begins at DWORD index `2 + count + extra`, i.e. byte offset
`4 * (2 + count + extra)` into the post-prefix data.

Verified: **all 75 `.ct` files parse to exactly their file length**, no slack,
no truncation. `extra` is 0 in every one.

---

## Colour tables — `.ct`

A `.ct` is a binary string table (above) and **a COLORMAP is literally a
STRING_TABLE** — `gfxlib.h:452-463` defines `LoadColorMapFile`,
`SetAbsColorMapIndex`, `GetColorMapAddress` as the string-table calls. So the
container is generic; only the *entries* are colour data.

### The two shapes

The container says how long each entry is. It does **not** say what shape the
entry has, and the two shapes in the tree are not distinguishable from the
bytes. You have to know which resource key you are holding.

**Shape A — colormap slots.** `[startSlot, endSlot]` then
`(endSlot - startSlot + 1) x 768` bytes, each 768 being a full 256-entry RGB
palette. This is what `SetColorMap` (`cmap.c:266-335`) consumes: it loads each
768-byte palette into global colormap slot `startSlot..endSlot`.
`NUMBER_OF_PLUTVALS` is 256 and `PLUTVAL_BYTE_SIZE` is 3 (`cmap.h:28-36`).

79 entries across the tree. `base/uqm.ct` is one entry holding 136 palettes
(104,450 bytes); most comm tables hold one.

**Shape B — a partial palette.** `[firstIndex, lastIndex]` then
`(lastIndex - firstIndex + 1) x 3` bytes: one RGB triple per palette *index*,
not per palette. 123 entries, every one of them `firstIndex=128,
lastIndex=255` — 128 triples, 386 bytes — and every one of them a
`planets/*.ct`, reached as `planet.<type>.colortable`.

**These two are ambiguous by construction.** Supox's shape-A entry declares
`start=10 end=10`; read under shape B's rule it would be 3 bytes, not 770. A
planet's shape-B entry declares `128..255`; read under shape A's rule it
claims 128 palettes and 98,304 bytes while carrying 384. A reader that guesses
from the header is wrong half the time, so the rewrite's loader must take the
expected shape from the caller.

Whether `SetColorMap` is ever handed a shape-B entry is worth settling before
the planet renderer is ported: if it is, it over-reads by 97,920 bytes.
Nothing observed says it is — `plangen.c:1864-1882` selects entry 0/1/2 of a
planet table by surface temperature and hands it to the topography renderer,
not to `SetColorMap` — but the two paths meet closely enough to be worth a
deliberate check rather than an assumption.

---

## Sprites — `.ani`

Plain text, **CRLF in all 584 files**, one line per cel:

```
supox-000.png -1 10 0 0
supox-001.png -1 10 -81 -30
```

`<file> <transparentColour> <colormapIndex> <hotspotX> <hotspotY>`, read with
`sscanf(line, "%s %d %d %d %d")` (`gfxload.c:250`). Verified: every line in
every file has exactly five fields, and there are no blank lines — so the
naive parse happens to be safe today. It is not safe by construction:
`gfxload.c:223-228` counts cels by counting *lines*, then the second pass only
advances `cel_index` on a successful image load, so a blank line would leave
`filename` holding the previous line's value and silently load a duplicate.

The image path is the `.ani`'s own directory plus the parsed name
(`gfxload.c:176-220`), and an `.ani` whose first four bytes are the ZIP magic
`0x04034B50` is instead mounted as an archive and re-opened from inside it.

Transparency has three cases (`gfxload.c:54-75`), keyed on
`transparentColour`:

| Value | Paletted image | Truecolour image |
| --- | --- | --- |
| `>= 0` | that palette index is transparent | — |
| `0` | (as above, index 0) | RGB 0,0,0 becomes transparent |
| `-1` | no transparency at all | no transparency at all |
| `-2` | use the PNG `tRNS` chunk | use the PNG `tRNS` chunk |

`colormapIndex` refers to a global colormap slot — the same numbering shape A
of a `.ct` writes into. Supox's cels say `10`, and `supox.ct`'s single entry
declares `start=10 end=10`. That correspondence is the whole binding, and
nothing checks it.

### The PNG's own palette is not the one the game draws with

An indexed cel carries a `PLTE`, and it is *nearly* right — which makes it the
most dangerous kind of wrong. Comparing `supox.ct` slot 10 against
`supox-000.png`'s `PLTE`, **245 of 256 entries differ**, every one of them by
one or two counts: `170` against `168`, `85` against `84`, `199` against
`196`.

Both are expansions of the same 6-bit VGA palette, by different rules:

```
PLTE   channel = v << 2                 // plain shift; loses the top 2 bits
.ct    channel = (v << 2) | (v >> 4)    // bit replication; reaches 255
```

Checked across all 26 comm races that have both: 19,621 of 19,968 channels fit
that relation exactly. (The 347 that do not are all `safeones.ct`, whose first
entry is black where the PNG's is not — a slot-selection difference, since
that table holds two entries, not a counterexample to the rule.)

**The `.ct` is authoritative.** `v << 2` can never produce 255, so a renderer
that reaches for the convenient `PLTE` sitting right there in the file gets an
image that is systematically slightly dark and never reaches full white — with
no error, no warning, and nothing to compare against unless you happen to run
the old build side by side. `uqm2-browse ani` takes the `.ct` when given one
and says which it used.

---

## Fonts — `.fon`

**A directory, not a file.** 30 of them, holding 2,599 PNGs between them. Each
member is named for the Unicode code point it draws, in hex:
`00020.png` is space, `00021.png` is `!`. `gfxload.c:432` parses the name with
`sscanf(name, "%x.")` and skips anything above `0xFFFF` or unparseable, so
non-conforming files in a font directory are ignored rather than fatal.
Verified: all 2,599 members match `[0-9a-fA-F]+\.png`.

---

## Conversations — `.txt`

Phrase files, parsed by `getstr.c:262-360`:

```
#(NEUTRAL_SPACE_HELLO_1)	supox-000.ogg
Greetings Fellow Carbon Creature, may your roots always be well watered.
```

A `#(NAME)` line opens a phrase; everything until the next one is its text,
with trailing blank lines trimmed (`getstr.c:284-292`). The optional second
field on the header line is a voice clip. Names go into a hash table
(`strings.c`), which is what makes `GetStringByName` possible — it had zero
callers until the transcript oracle started using it.

Binding to code is by **ordinal**, not by name: `NPCPhrase(X)` reaches
`SetAbsStringTableIndex(table, X - 1)`. `tools/check-phrases.py` is the only
thing keeping the 27 enums and 27 files aligned; see
`tests/fixtures/comm/README.md`.

### Timestamps — `.ts`

One per race (27), alongside the voice pack. Read in lockstep with the `.txt`,
one `uio_fgets` per phrase (`getstr.c:306-349`), matched by `strstr`. Two
sharp edges, both checked by `tools/check-phrases.py`:

- any mismatch discards **every** timestamp for that race behind a single log
  warning, so one bad line silently desynchronises a whole alien's subtitles;
- `strstr` is a substring test, so a line naming `FOO_EXTRA` satisfies phrase
  `FOO`, passes silently, and yields garbage timings.

---

## Overlays

`content/addons/` holds `3domusic`, `3dovideo` and `3dovoice`. An addon
shadows base resources by path. `3dovoice` carries all 27 `.ts` files and
**replaces six races' `.txt` outright** (arilou, melnorme, mycon, starbase,
syreen, utwig), which is why the speech setting is a content input and not a
preference — the same script can emit different phrases with it on and off.
