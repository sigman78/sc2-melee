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
 * Semantics of uqm/commresponse.h -- the resume point the race files' response
 * functions use to wait for a talk segue (docs/unthread.md §7b.2).
 *
 * The property everything rests on is that re-entering a response function
 * runs the code *after* the RESPONSE_SEGUE and not the code before it. If that
 * were wrong the failure would be an NPCPhrase queued twice or a SET_GAME_STATE
 * applied twice, one story beat deep in one alien's conversation, which is
 * about the worst place in this codebase to find a bug. So it is pinned here.
 *
 * comm.c's real driver is modelled by RunResponse below: call the function,
 * and while it asked for a segue, call it again.
 */

#include <stdio.h>

#include "libs/coroutine.h"
#include "uqm/commresponse.h"

static int failures;

#define Check(cond, what) \
		do { \
			if (!(cond)) { \
				printf ("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__); \
				++failures; \
			} \
		} while (0)

/* The two hooks commresponse.h declares; comm.c has the real ones. */
static int resumePc;
static COUNT segueWait;
static int seguePending;

int
ResponseResumePoint (void)
{
	int pc = resumePc;

	resumePc = 0;
	return pc;
}

void
SetResponseResumePoint (int pc, COUNT waitTrack)
{
	resumePc = pc;
	segueWait = waitTrack;
	seguePending = 1;
}

/* What comm.c's ResponseSegue_Frame does, minus the yielding. */
static int seguesRun;
static COUNT lastWait;

static void
RunResponse (void (*func) (COUNT R), COUNT ref)
{
	int guard = 0;

	resumePc = 0;
	seguePending = 0;

	func (ref);
	while (seguePending)
	{
		seguePending = 0;
		++seguesRun;
		lastWait = segueWait;
		func (ref);

		if (++guard > 100)
		{	// A resume point that does not advance -- the function restarts
			// from the top and asks for the same segue forever. Bounded so
			// that a broken mechanism fails the test instead of hanging it,
			// which is how this was found: mutating ResponseResumePoint to
			// return 0 made the unbounded version spin rather than report.
			Check (0, "the resume point advances");
			break;
		}
	}
	resumePc = 0;
}

/* ------------------------------------------------------------------ */
/* A response function shaped like the ones in the race files. */

static int beforeCount;
static int afterCount;
static int elseCount;
static COUNT sawRef;

static void
OneSegue (COUNT R)
{
	RESPONSE_BEGIN ();

	if (R == 1)
	{
		++beforeCount;
		sawRef = R;
		RESPONSE_SEGUE ((COUNT)~0);
		++afterCount;
	}
	else
	{
		++elseCount;
	}

	RESPONSE_END ();
}

/* The paired shape: two segues around something, as in vuxc.c. */
static int stage0, stage1, stage2;

static void
TwoSegues (COUNT R)
{
	(void) R;

	RESPONSE_BEGIN ();

	++stage0;
	RESPONSE_SEGUE (1);
	++stage1;
	RESPONSE_SEGUE ((COUNT)~0);
	++stage2;

	RESPONSE_END ();
}

/* A function with no segue at all -- the several hundred that pay nothing. */
static int plainCount;

static void
NoSegue (COUNT R)
{
	(void) R;

	RESPONSE_BEGIN ();
	++plainCount;
	RESPONSE_END ();
}

/* zoqfotc.c's Intro shape: yield, then call another function that has a resume
 * point of its own but does not yield on this ref. The callee must start from
 * the top. It only does because RESPONSE_BEGIN consumes the resume point --
 * with a plain read the callee would inherit the caller's label, match none of
 * its own cases, and skip its whole body, registering no responses. */
static int calleeRuns;

static void
NestedCallee (COUNT R)
{
	(void) R;

	RESPONSE_BEGIN ();
	++calleeRuns;
	RESPONSE_END ();
}

static int callerBefore, callerAfter;

static void
NestedCaller (COUNT R)
{
	(void) R;

	RESPONSE_BEGIN ();

	++callerBefore;
	RESPONSE_SEGUE ((COUNT)~0);
	++callerAfter;
	NestedCallee (0);

	RESPONSE_END ();
}

/* A segue nested inside a loop and an if, to check the label placement is not
 * accidentally depending on being at function scope. */
static int loopBefore, loopAfter;

static void
SegueInLoop (COUNT R)
{
	(void) R;

	RESPONSE_BEGIN ();

	if (R == 7)
	{
		++loopBefore;
		RESPONSE_SEGUE (3);
		++loopAfter;
	}

	RESPONSE_END ();
}

/* ------------------------------------------------------------------ */
/* comm.c's ResponseSegue_Frame, driven for real.
 *
 * The loop above is a model; this is the shape of the actual driver, macros
 * and all. It matters for the paired sites (vuxc.c and friends), because the
 * second time round the while loop the UQM_CALL yields with the *same* resume
 * label it used the first time -- the label is a source position, not a loop
 * iteration. If that did not re-enter the inner while correctly, a paired
 * segue would hang or skip its second half.
 *
 * This mirrors comm.c rather than calling it: linking the real one would drag
 * in the whole game. Keep the two in step.
 */
typedef struct
{
	UQM_CORO_FIELDS;
	void (*func) (COUNT R);
	COUNT ref;
} DriverCtx;

static DriverCtx driverCtx;

static int fakeSegueFramesLeft;
static int fakeSegueRuns;

/* Stands in for AlienTalkSegue_Frame: takes a few frames, then finishes. */
static UqmStatus
FakeSegue_Frame (void)
{
	if (fakeSegueFramesLeft > 0)
	{
		--fakeSegueFramesLeft;
		return UQM_PENDING;
	}
	return UQM_DONE;
}

static UqmStatus
DriveResponse_Frame (void)
{
	DriverCtx *c = &driverCtx;

	UQM_BEGIN (c);

	while (seguePending)
	{
		seguePending = 0;
		fakeSegueFramesLeft = 3;
		++fakeSegueRuns;
		lastWait = segueWait;
		UQM_CALL (c, FakeSegue_Frame ());

		(*c->func) (c->ref);
	}

	resumePc = 0;

	UQM_END (c);
}

/* SelectResponse's half: first call is synchronous, and only if it asked for a
 * segue does the task get started. */
static int
RunResponseForReal (void (*func) (COUNT R), COUNT ref)
{
	int frames = 0;

	resumePc = 0;
	seguePending = 0;
	fakeSegueRuns = 0;
	driverCtx.func = func;
	driverCtx.ref = ref;

	func (ref);
	if (!seguePending)
		return 0;

	UQM_INIT (&driverCtx);
	while (DriveResponse_Frame () != UQM_DONE)
	{
		if (++frames > 1000)
		{
			Check (0, "driver terminates");
			break;
		}
	}
	return frames;
}

int
main (void)
{
	{	// The taken branch: everything before the segue runs once, everything
		// after it runs once, and the driver saw exactly one segue.
		beforeCount = afterCount = elseCount = seguesRun = 0;
		RunResponse (OneSegue, 1);
		Check (beforeCount == 1, "code before RESPONSE_SEGUE runs once");
		Check (afterCount == 1, "code after RESPONSE_SEGUE runs once");
		Check (elseCount == 0, "the untaken branch stays untaken on resume");
		Check (seguesRun == 1, "the driver ran one segue");
		Check (lastWait == (COUNT)~0, "the wait_track reached the driver");
		Check (resumePc == 0, "the resume point is cleared afterwards");
	}

	{	// The branch with no segue in it: one pass, no driver involvement.
		beforeCount = afterCount = elseCount = seguesRun = 0;
		RunResponse (OneSegue, 2);
		Check (elseCount == 1, "the no-segue branch runs once");
		Check (beforeCount == 0 && afterCount == 0, "and only that branch");
		Check (seguesRun == 0, "no segue was requested");
	}

	{	// Two segues: three stages, each run exactly once, in order.
		stage0 = stage1 = stage2 = seguesRun = 0;
		RunResponse (TwoSegues, 0);
		Check (stage0 == 1 && stage1 == 1 && stage2 == 1,
				"each stage of a paired segue runs exactly once");
		Check (seguesRun == 2, "the driver ran both segues");
		Check (lastWait == (COUNT)~0, "the second wait_track reached it");
	}

	{	// A function that never segues costs nothing and runs once.
		plainCount = seguesRun = 0;
		RunResponse (NoSegue, 0);
		Check (plainCount == 1, "a plain response function runs once");
		Check (seguesRun == 0, "and asks for no segue");
	}

	{	// Re-running the same function afterwards starts from the top again,
		// which is what happens when the player picks the response twice.
		beforeCount = afterCount = seguesRun = 0;
		RunResponse (OneSegue, 1);
		RunResponse (OneSegue, 1);
		Check (beforeCount == 2 && afterCount == 2,
				"a second dispatch starts from the top");
		Check (seguesRun == 2, "and segues again");
	}

	{	// The label inside a nested block resumes there, not at function
		// scope.
		loopBefore = loopAfter = seguesRun = 0;
		RunResponse (SegueInLoop, 7);
		Check (loopBefore == 1 && loopAfter == 1,
				"a segue nested in an if resumes inside it");
		Check (lastWait == 3, "with its own wait_track");
	}

	{	// The real driver shape, single segue.
		beforeCount = afterCount = 0;
		RunResponseForReal (OneSegue, 1);
		Check (beforeCount == 1 && afterCount == 1,
				"the real driver runs each side of one segue once");
		Check (fakeSegueRuns == 1, "and ran one segue");
		Check (resumePc == 0, "and cleared the resume point");
	}

	{	// The real driver shape, paired segues -- the case where UQM_CALL's
		// resume label is reused on the second turn of the while loop.
		stage0 = stage1 = stage2 = 0;
		RunResponseForReal (TwoSegues, 0);
		Check (stage0 == 1 && stage1 == 1 && stage2 == 1,
				"the real driver runs all three stages of a paired segue");
		Check (fakeSegueRuns == 2, "and ran both segues");
		Check (lastWait == (COUNT)~0, "the second wait_track reached it");
		Check (resumePc == 0, "and cleared the resume point");
	}

	{	// A paired segue must actually take frames on both halves, not
		// collapse into one.
		stage0 = stage1 = stage2 = 0;
		Check (RunResponseForReal (TwoSegues, 0) >= 6,
				"both halves of a paired segue take their frames");
	}

	{	// A response function that never segues never starts the driver.
		plainCount = 0;
		Check (RunResponseForReal (NoSegue, 0) == 0,
				"a plain response function never starts the driver");
		Check (plainCount == 1, "and still runs once");
	}

	{	// A function called from inside a resumed one must start from its own
		// top, not inherit the caller's resume point.
		callerBefore = callerAfter = calleeRuns = 0;
		RunResponse (NestedCaller, 0);
		Check (callerBefore == 1 && callerAfter == 1,
				"the caller runs each side of its segue once");
		Check (calleeRuns == 1,
				"a callee reached after a resume runs its body");
	}

	{	// Same, through the real driver.
		callerBefore = callerAfter = calleeRuns = 0;
		RunResponseForReal (NestedCaller, 0);
		Check (calleeRuns == 1,
				"the real driver does not leak a resume point into a callee");
	}

	if (failures)
		printf ("%d check(s) failed\n", failures);
	else
		printf ("all checks passed\n");
	return failures != 0;
}
