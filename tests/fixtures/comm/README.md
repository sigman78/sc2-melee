# Conversation transcript fixtures

The dialogue oracle from `docs/game-rewrite-plan.md`, "Verification" (1).
Each `.trace` is one conversation played through the **unmodified C**, recorded
as the sequence of things the rewrite has to reproduce. The rewrite replays a
fixture and must match it line for line.

Captured before any of `comm/` is touched, deliberately. A transcript taken
after a change is a record of the change, not a baseline.

## What a transcript records

| Line | Meaning |
| --- | --- |
| `conv <race> site=<n>` | conversation entered. `site` is `GLOBAL_FLAGS_AND_DATA`, which `commglue.c:391-395` reads *before the conversation exists* to choose which race file runs — not a preference, an input |
| `phrase <NAME>` | an NPC phrase went to the subtitle track |
| `offer <slot> <NAME>` | a response was registered, at that menu position |
| `offer <slot> <NAME> = <text>` | …and the race built the text with `construct_response` rather than naming a phrase |
| `pick <slot> <NAME>` | the response taken |
| `retire <NAME>` | `DISABLE_PHRASE` retired it for the rest of this conversation |
| `state <NAME> <old> -> <new>` | a `SET_GAME_STATE` landed. Unchanged writes are recorded too: a race re-asserting a state it already holds is a fact about the script |
| `exit <disposition>` | `done`, `battle`, `abort`, `load`, or a `script-*` failure |

Phrases are named by the `#(NAME)` label the content already carries, never by
the ordinal the C binds on. Name binding is what the rewrite moves to, and an
ordinal-keyed fixture would need regenerating the moment phrase order shifted —
the exact silent drift `tools/check-phrases.py` exists to catch.

Offer **order** is part of the contract, not a detail. Twelve race files
reorder the menu by hand; `neutral-utwig-first.trace` below is the Supox doing
it.

## What it deliberately does not record

Timing, pixels, subtitle geometry, voice-clip identity, animation state. Those
are presentation — the parts the rewrite is free to change. Pinning them would
turn an oracle into a change detector.

## Regenerating

```powershell
tools/capture-comm-trace.ps1 -Race supox -Script '0,0,0,0,0,0,0,0,0,0,0' `
        -Out tests/fixtures/comm/supox/neutral-full.trace
```

`-Script` is one response **slot index** per menu, in order. The run
auto-starts a new game (`UQM_DEBUG_AUTOSTART=new`), forces the conversation
(`UQM_DEBUG_COMM`), and takes those picks instead of waiting for a human.

A new game rather than a save slot, on purpose: a committed fixture has to be
regenerable from the content alone, and "whatever is in `uqmsave.00` on this
machine" is not a state vector anyone else can reproduce. Every fixture here
therefore starts from the same initial state vector — a fresh game, Feb 17
2155 — which is why the header records the start mode rather than 447 fields.

Captures run with the audio device silenced and speech volume at zero. Neither
affects the transcript: phrases are recorded when spliced onto the track, not
when they finish scrolling. Verified — a capture with default audio and full
subtitle pacing (265s) is byte-identical to one with the skip (10s). The
`# speechContent` header is a different thing and *is* pinned, because the
voice pack replaces six races' `.txt` outright, so the same script can emit
different phrases with it on and off.

## Supox

The plan's chosen first race: the smallest one that exercises
`construct_response`, `GetAllianceName`, an offer-replacing handler, a column
gate and a host verb.

| Fixture | Script | Covers |
| --- | --- | --- |
| `neutral-full.trace` | `0` ×11 | the whole tree. `construct_response` ×3 (`i_am0`, `my_ship0`, `from_alliance0`), `GetAllianceName`, the column gate, the offer-replacing handler, `StartSphereTracking` |
| `neutral-farewell.trace` | `2` | shortest path out — hello, goodbye |
| `neutral-utwig-first.trace` | `1,0,0,0` | `LastStack` menu reordering: after `anyone_around_here` the utwig topic moves to slot 0 and `i_am0` drops to slot 1 |

Two things in `neutral-full.trace` are worth knowing before reading it:

**The column gate.** `supoxc.c:429-472` sets `pStr[1] = 0` in *every*
`SUPOX_STACK1` case, and there is no `case 5`. So the species column is
unreachable until the player has exhausted `i_am → my_ship → from_alliance →
are_you_copying → why_copy` and driven `SUPOX_STACK1` to 5 — a value with no
case, which is the only way `pStr[1]` survives. In the transcript that is the
five turns before `tell_us_of_your_species` first appears. It is a gate
expressed as an absent switch arm.

**The offer-replacing handler.** `supoxc.c:383-394`: `whats_ultron` emits,
writes two state fields, registers two offers and `return`s, bypassing the
node's entire offer computation at `:420-529`. In the transcript that is the
only menu with no `bye_neutral` on it.

## Not yet covered

- `what_relation_to_utwig` needs `UTWIG_VISITS`, `UTWIG_HOME_VISITS` or
  `BOMB_VISITS` non-zero, which a fresh game cannot reach. Needs either a
  state-vector input to the harness or a longer scripted route.
- The four `ULTRON_CONDITION` branches (`where_get_repairs`,
  `look_i_slightly_repaired`, `look_i_repaired_lots`, `got_fixed_ultron`) —
  same reason. `neutral-full.trace` reaches `ULTRON_CONDITION = 1` on its last
  turn but the conversation ends there.
- `HostileSupox`, and the post-Ultron nodes.
