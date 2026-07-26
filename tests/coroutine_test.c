/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Semantics of libs/coroutine.h -- the macro layer every Phase 2 conversion
 * is written in (docs/unthread.md §4). Duff's device is easy to get subtly
 * wrong, and a mistake here would show up as a mis-timed cutscene forty
 * files away, so the resume behaviour is pinned down here instead.
 *
 * The clock is faked so the timing assertions are exact rather than flaky.
 */

#include <stdio.h>
#include <string.h>

#include "libs/coroutine.h"

// --- Stubs for the two symbols coroutine.h leans on ------------------------

static TimeCount fakeClock;

TimeCount
GetTimeCounter (void)
{
	return fakeClock;
}

static int taskSwitches;

void
TaskSwitch (void)
{	// What UQM_RUN_BLOCKING calls; here it just advances the fake clock,
	// standing in for the real one-tick sleep.
	++taskSwitches;
	++fakeClock;
}

// --- Trace of where a task got to, so ordering can be asserted -------------

static char trace[512];

static void
T (const char *s)
{
	strncat (trace, s, sizeof (trace) - strlen (trace) - 1);
}

static int failures;

static void
Check (int cond, const char *what)
{
	if (!cond)
	{
		printf ("FAIL: %s\n", what);
		++failures;
	}
}

// --- A task that sleeps twice and returns a value --------------------------

typedef struct
{
	UQM_CORO_FIELDS;
	int arg;
	int result;
} InnerCtx;

static void
Inner_Init (InnerCtx *c, int arg)
{
	UQM_INIT (c);
	c->arg = arg;
}

static UqmStatus
Inner_Frame (InnerCtx *c)
{
	UQM_BEGIN (c);
	T ("i0");
	UQM_SLEEP (c, 3);
	T ("i1");
	UQM_SLEEP (c, 2);
	T ("i2");
	c->result = c->arg * 2;
	UQM_END (c);
}

// --- A task with a loop, a nested call and an early return -----------------

typedef struct
{
	UQM_CORO_FIELDS;
	int i;
	int bail;
	InnerCtx inner;
	int result;
} OuterCtx;

static void
Outer_Init (OuterCtx *c, int bail)
{
	UQM_INIT (c);
	c->bail = bail;
	c->i = 0;
}

static UqmStatus
Outer_Frame (OuterCtx *c)
{
	UQM_BEGIN (c);
	T ("o0");
	if (c->bail)
	{
		c->result = -1;
		UQM_RETURN (c);
	}
	while (c->i < 3)
	{
		T ("L");
		UQM_YIELD (c);
		++c->i;
	}
	T ("o1");
	Inner_Init (&c->inner, 21);
	UQM_CALL (c, Inner_Frame (&c->inner));
	T ("o2");
	c->result = c->inner.result;
	UQM_END (c);
}

// --- A task whose deadlines are already in the past ------------------------

typedef struct
{
	UQM_CORO_FIELDS;
} PastDueCtx;

static void
PastDue_Init (PastDueCtx *c)
{
	UQM_INIT (c);
}

static UqmStatus
PastDue_Frame (PastDueCtx *c)
{
	UQM_BEGIN (c);
	UQM_SLEEP_UNTIL (c, 1);
	UQM_SLEEP_UNTIL (c, 2);
	UQM_END (c);
}

// --- A task that waits on a condition --------------------------------------

typedef struct
{
	UQM_CORO_FIELDS;
} GateCtx;

static int gateOpen;

static UqmStatus
Gate_Frame (GateCtx *c)
{
	UQM_BEGIN (c);
	UQM_WAIT_UNTIL (c, gateOpen);
	T ("g");
	UQM_END (c);
}

#define EXPECTED_TRACE "o0LLLo1i0i1i2o2"

int
main (void)
{
	// 1. Frame-by-frame, with the exact interleaving pinned down: the entry
	//    chunk, three loop iterations, then the nested task's three chunks.
	{
		OuterCtx o;
		int frames = 0;

		fakeClock = 0;
		trace[0] = '\0';
		Outer_Init (&o, 0);
		while (Outer_Frame (&o) != UQM_DONE)
		{
			++frames;
			++fakeClock;
			if (frames > 100)
				break;
		}
		printf ("trace = %s (%d frames)\n", trace, frames);
		Check (strcmp (trace, EXPECTED_TRACE) == 0, "frame-by-frame trace");
		Check (o.result == 42, "nested task result propagated");
		Check (o._coro.pc == 0, "pc reset after UQM_END");
	}

	// 2. An early return finishes in the frame it happens in.
	{
		OuterCtx o;

		trace[0] = '\0';
		Outer_Init (&o, 1);
		Check (Outer_Frame (&o) == UQM_DONE, "UQM_RETURN finishes immediately");
		Check (strcmp (trace, "o0") == 0 && o.result == -1,
				"UQM_RETURN skips the rest of the body");
		Check (o._coro.pc == 0, "pc reset after UQM_RETURN");
	}

	// 3. A deadline already in the past still gives up exactly one frame,
	//    matching SleepThreadUntil's TaskSwitch branch. Without that, a
	//    pacing loop that fell behind would spin without ever ending a
	//    frame -- a hang, not a slowdown.
	{
		PastDueCtx pc;
		int yields = 0;

		fakeClock = 1000;
		PastDue_Init (&pc);
		while (PastDue_Frame (&pc) != UQM_DONE)
		{	// clock deliberately not advanced
			++yields;
			if (yields > 10)
				break;
		}
		Check (yields == 2, "one yield per past-due UQM_SLEEP_UNTIL");
	}

	// 4. UQM_SLEEP really waits for the clock.
	{
		InnerCtx ic;
		TimeCount started;

		fakeClock = 500;
		started = fakeClock;
		trace[0] = '\0';
		Inner_Init (&ic, 1);
		while (Inner_Frame (&ic) != UQM_DONE)
			++fakeClock;
		Check (fakeClock - started == 5, "UQM_SLEEP durations add up (3 + 2)");
	}

	// 5. UQM_WAIT_UNTIL yields until the condition holds.
	{
		GateCtx g;
		int frames = 0;

		UQM_INIT (&g);
		gateOpen = 0;
		trace[0] = '\0';
		while (Gate_Frame (&g) != UQM_DONE)
		{
			if (++frames == 4)
				gateOpen = 1;
			if (frames > 20)
				break;
		}
		Check (frames == 4, "UQM_WAIT_UNTIL yields until the condition holds");
		Check (strcmp (trace, "g") == 0, "UQM_WAIT_UNTIL falls through once open");
	}

	// 6. The transitional blocking wrapper reaches the same answer as the
	//    frame-by-frame drive. This is what keeps unconverted callers honest
	//    while the sweep is in progress.
	{
		OuterCtx o;

		fakeClock = 0;
		taskSwitches = 0;
		trace[0] = '\0';
		Outer_Init (&o, 0);
		UQM_RUN_BLOCKING (Outer_Frame (&o));
		Check (strcmp (trace, EXPECTED_TRACE) == 0 && o.result == 42,
				"UQM_RUN_BLOCKING matches the frame-by-frame result");
		Check (taskSwitches > 0, "UQM_RUN_BLOCKING yielded to the render thread");
	}

	// 7. A finished task can be re-run from the top.
	{
		OuterCtx o;

		fakeClock = 0;
		trace[0] = '\0';
		Outer_Init (&o, 0);
		UQM_RUN_BLOCKING (Outer_Frame (&o));
		Check (strcmp (trace, EXPECTED_TRACE) == 0, "re-running restarts cleanly");
	}

	// 8. Pacing yields are distinguishable from frame yields, and a sub-task's
	//    pacing propagates out through UQM_CALL. The driver relies on this to
	//    keep exactly one input sample per screen frame -- without it the
	//    edge-triggered PulsedInputState loses keypresses to pacing yields
	//    (see UqmStatus in libs/coroutine.h).
	{
		OuterCtx o;
		int pending = 0, pacing = 0, frames = 0;
		UqmStatus st;
		int sawPacing = 0;

		fakeClock = 0;
		trace[0] = '\0';
		Outer_Init (&o, 0);
		while ((st = Outer_Frame (&o)) != UQM_DONE)
		{
			if (st == UQM_PENDING)
			{
				++pending;
				// Every pacing yield must come from the nested sleeping task,
				// which only runs after the plain-yield loop is done.
				Check (!sawPacing, "no frame yield after pacing begins");
			}
			else if (st == UQM_PACING)
			{
				++pacing;
				sawPacing = 1;
			}
			else
			{
				Check (0, "unexpected status");
			}
			++fakeClock;
			if (++frames > 100)
				break;
		}
		Check (pending == 3, "the three UQM_YIELDs report UQM_PENDING");
		Check (pacing > 0, "UQM_SLEEP inside a sub-task reports UQM_PACING");
		Check (pending + pacing == frames, "every yield is one or the other");
	}

	if (failures)
		printf ("%d check(s) failed\n", failures);
	else
		printf ("all checks passed\n");
	return failures != 0;
}
