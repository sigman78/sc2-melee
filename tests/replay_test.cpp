// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// A replay-similarity harness: 32 seeded battles run on the current sim.
// --record writes a baseline; --compare demands an exact re-match; --similar
// tolerates drift within a threshold; --trace dumps one battle's full
// per-frame state for diffing across builds.

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Collision.hpp"
#include "sim/Damage.hpp"
#include "sim/Field.hpp"
#include "sim/Random.hpp"
#include "sim/Ship.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/ships/Human.hpp"
#include "sim/ships/Ilwrath.hpp"
#include "sim/Trig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace uqm;
using namespace uqm::sim;

namespace {

// --------------------------------------------------------------------------
// Battle setup: mirrors app/melee/Game.cpp's setUpBattle, headless -- no
// content, no stars, no window.

CollisionMask
solid(u32 w, u32 h)
{
	const std::vector<u8> bits(static_cast<usize>(w) * h, 1);
	return CollisionMask(Extent2u{w, h},
			Vec2i{static_cast<i32>(w / 2), static_cast<i32>(h / 2)}, bits);
}

const CollisionMask kShipMask = solid(12, 12);
const CollisionMask kAsteroidMask = solid(8, 8);
const CollisionMask kPlanetMask = solid(28, 28);

// A weapon's mask is normally content-derived (game/Materialize.cpp fills
// ShipSpec::weapon.masks from the loaded sprite before battle); headless,
// there is no sprite, and an empty span leaves every shot's mask null --
// collidable() then excludes it and nothing a ship fires can ever hit
// anything. One synthetic mask, indexed modulo 1, stands in for both ships'
// weapons.
const CollisionMask kWeaponMaskArray[1]{solid(4, 4)};

const ShipSpec &
materializedCruiser() noexcept
{
	static const ShipSpec spec = [] {
		ShipSpec s = earthlingCruiser();
		s.weapon.masks = kWeaponMaskArray;
		return s;
	}();
	return spec;
}

const ShipSpec &
materializedAvenger() noexcept
{
	static const ShipSpec spec = [] {
		ShipSpec s = ilwrathAvenger();
		s.weapon.masks = kWeaponMaskArray;
		return s;
	}();
	return spec;
}

// Minimum ship separation at spawn -- see Game.cpp's setUpBattle.
constexpr i32 kMinSeparation = 1024;

EntityId
addShip(Battle &b, const ShipSpec &spec, Facing facing, i32 player)
{
	Element e;
	e.kind = ElementKind::Ship;
	e.flags = ElementFlags::IgnoreSimilar;
	e.current = Vec2i{0, 0};
	e.next = e.current;
	e.facing = facing;
	e.playerNr = player;
	e.mass = spec.mass;
	e.mask = &kShipMask;
	e.onCollision = solidCollision;
	const EntityId id = b.spawn(Layer::Field, std::move(e));
	b.attach<PlayerShip>(id);
	b.attach<WarpingIn>(id);
	b.attachShip(id, &spec);
	return id;
}

struct Ships
{
	EntityId cruiser;
	EntityId avenger;
};

// Cruiser vs Avenger, one of each fully spawned and placed before the next --
// Game.cpp's order, since placement rejects on overlap with what already
// exists.
Ships
setUpBattle(Battle &b)
{
	Ships s;
	const Facing f0 = Facing(static_cast<int>(b.rng().next() & 0xFFu));
	s.cruiser = addShip(b, materializedCruiser(), f0, 0);
	placeShipAtRandom(b, s.cruiser, kMinSeparation);

	const Facing f1 = Facing(static_cast<int>(b.rng().next() & 0xFFu));
	s.avenger = addShip(b, materializedAvenger(), f1, 1);
	placeShipAtRandom(b, s.avenger, kMinSeparation);

	for (int i = 0; i < kNumAsteroids; ++i)
		(void)spawnAsteroid(b, &kAsteroidMask);
	(void)spawnPlanet(b, &kPlanetMask);
	return s;
}

// --------------------------------------------------------------------------
// Input scripts: one Rng per player, independent of the battle's own stream,
// redrawn every 8 frames.

constexpr u32 kInputSalt = 0x9E3779B9u;
constexpr int kInputPeriod = 8;

Rng
inputRngFor(u32 battleSeed, int playerIndex) noexcept
{
	return Rng(battleSeed ^ (kInputSalt * static_cast<u32>(playerIndex + 1)));
}

ShipInput
wordToInput(u32 w) noexcept
{
	ShipInput in = ShipInput::None;
	if ((w & 1u) != 0u)
		in |= ShipInput::Thrust;
	const u32 turn = (w >> 1) & 3u;
	if (turn == 0)
		in |= ShipInput::Left;
	else if (turn == 1)
		in |= ShipInput::Right;
	// <3 (37.5%, the natural reading of the formula) decided only 12/32
	// battles inside 2400 frames; <5 (62.5%) clears the "at least half"
	// eventfulness bar with room to spare, without touching turn or thrust.
	if (((w >> 3) & 7u) < 5u)
		in |= ShipInput::Weapon;
	if (((w >> 6) & 7u) < 2u)
		in |= ShipInput::Special;
	return in;
}

// --------------------------------------------------------------------------
// The digest: FNV-1a 64-bit, folded over the walk order every frame, then
// over the sequence of per-frame digests for the battle hash. Entity ids are
// never folded -- recycling patterns must not pin the hash.

constexpr u64 kFnvOffset = 0xcbf29ce484222325ull;
constexpr u64 kFnvPrime = 0x100000001b3ull;

void
foldBytes(u64 &h, const u8 *p, usize n) noexcept
{
	for (usize i = 0; i < n; ++i)
	{
		h ^= p[i];
		h *= kFnvPrime;
	}
}

void
foldI32(u64 &h, i32 v) noexcept
{
	u8 b[4];
	std::memcpy(b, &v, sizeof(b));
	foldBytes(h, b, sizeof(b));
}

void
foldU64(u64 &h, u64 v) noexcept
{
	u8 b[8];
	std::memcpy(b, &v, sizeof(b));
	foldBytes(h, b, sizeof(b));
}

std::string
hex16(u64 v)
{
	std::ostringstream oss;
	oss << std::hex << std::setfill('0') << std::setw(16) << v;
	return oss.str();
}

// --------------------------------------------------------------------------
// One battle, run deterministically from its seed.

constexpr int kNumBattles = 32;
constexpr int kMaxSteps = 2400;
constexpr int kDeathBuffer = 48;
constexpr u64 kCheckpointInterval = 64;  // frames per running-hash checkpoint

u32
seedFor(int i) noexcept
{
	return (1000003u * static_cast<u32>(i) + 7u) | 1u;
}

struct BattleResult
{
	u32 seed = 0;
	i32 winner = -1;
	u64 endFrame = 0;
	u64 hash = 0;
	i32 crewLost0 = 0;
	i32 crewLost1 = 0;
	u64 shots = 0;
	u64 collisions = 0;
	std::vector<u64> checkpoints;
};

// Runs one battle. `trace`, if non-null, gets one line per element per frame
// -- see doTrace.
BattleResult
simulateBattle(u32 seed, std::ostream *trace)
{
	BattleResult r;
	r.seed = seed;

	Battle b(seed);
	const Ships ships = setUpBattle(b);
	const EntityId shipId[2]{ships.cruiser, ships.avenger};
	const i32 maxCrew[2]{earthlingCruiser().maxCrew, ilwrathAvenger().maxCrew};

	Rng inputRng[2]{inputRngFor(seed, 0), inputRngFor(seed, 1)};
	u32 word[2]{0, 0};

	u64 battleHash = kFnvOffset;
	i32 deathFrame = -1;

	for (int frame = 0; frame < kMaxSteps; ++frame)
	{
		for (int p = 0; p < 2; ++p)
		{
			if (frame % kInputPeriod == 0)
				word[p] = inputRng[p].next();
			if (Input *in = b.find<Input>(shipId[p]))
				in->buttons = wordToInput(word[p]);
		}

		b.step();

		for (const SpawnEvent &sp : b.spawns())
			if (sp.kind == ElementKind::Weapon)
				++r.shots;
		r.collisions += b.collisions().size();

		u64 frameDigest = kFnvOffset;
		usize walkIndex = 0;
		for (EntityId id = b.front(); id != kNoEntity;
				id = b.next(id), ++walkIndex)
		{
			const Element *e = b.get(id);
			foldI32(frameDigest, e->current.x);
			foldI32(frameDigest, e->current.y);
			foldI32(frameDigest, e->lifeSpan);
			const ShipState *ss = b.ship(id);
			if (ss != nullptr)
			{
				foldI32(frameDigest, ss->crew);
				foldI32(frameDigest, ss->energy);
			}
			if (trace != nullptr)
			{
				*trace << b.frame() << ' ' << walkIndex << ' '
						<< static_cast<int>(e->kind) << ' ' << e->current.x
						<< ' ' << e->current.y << ' ' << e->lifeSpan;
				if (ss != nullptr)
					*trace << ' ' << ss->crew << ' ' << ss->energy;
				*trace << '\n';
			}
		}
		foldU64(battleHash, frameDigest);

		if (b.frame() % kCheckpointInterval == 0)
			r.checkpoints.push_back(battleHash);

		if (deathFrame < 0
				&& (b.get(shipId[0]) == nullptr || b.get(shipId[1]) == nullptr))
			deathFrame = frame;
		if (deathFrame >= 0 && frame >= deathFrame + kDeathBuffer)
			break;
	}

	const bool alive0 = b.get(shipId[0]) != nullptr;
	const bool alive1 = b.get(shipId[1]) != nullptr;
	r.winner = (alive0 && alive1) ? -1 : (alive0 ? 0 : (alive1 ? 1 : 2));
	r.endFrame = b.frame();
	r.hash = battleHash;
	r.crewLost0 = maxCrew[0] - (alive0 ? b.ship(shipId[0])->crew : 0);
	r.crewLost1 = maxCrew[1] - (alive1 ? b.ship(shipId[1])->crew : 0);
	return r;
}

// --------------------------------------------------------------------------
// The baseline file: a header, then one summary line plus one checkpoint
// line per battle.

constexpr char kBaselineHeader[] = "replay-baseline v1";

void
writeBaseline(std::ostream &out, const std::vector<BattleResult> &results)
{
	out << kBaselineHeader << '\n';
	for (const BattleResult &r : results)
	{
		out << r.seed << ' ' << r.winner << ' ' << r.endFrame << ' '
				<< hex16(r.hash) << ' ' << r.crewLost0 << ' ' << r.crewLost1
				<< ' ' << r.shots << ' ' << r.collisions << '\n';
		out << "ck " << r.checkpoints.size();
		for (u64 c : r.checkpoints)
			out << ' ' << hex16(c);
		out << '\n';
	}
}

struct BaselineEntry
{
	u32 seed = 0;
	i32 winner = -1;
	u64 endFrame = 0;
	u64 hash = 0;
	i32 crewLost0 = 0;
	i32 crewLost1 = 0;
	u64 shots = 0;
	u64 collisions = 0;
	std::vector<u64> checkpoints;
};

bool
loadBaseline(const std::string &path, std::vector<BaselineEntry> &out,
		std::string &err)
{
	std::ifstream in(path);
	if (!in)
	{
		err = "cannot open " + path;
		return false;
	}
	std::string header;
	std::getline(in, header);
	if (header != kBaselineHeader)
	{
		err = "unrecognised header: " + header;
		return false;
	}

	std::string line;
	while (std::getline(in, line))
	{
		if (line.empty())
			continue;
		BaselineEntry e;
		std::string hashHex;
		std::istringstream iss(line);
		iss >> e.seed >> e.winner >> e.endFrame >> hashHex >> e.crewLost0
				>> e.crewLost1 >> e.shots >> e.collisions;
		if (!iss)
		{
			err = "malformed battle line: " + line;
			return false;
		}
		e.hash = std::stoull(hashHex, nullptr, 16);

		std::string ckLine;
		if (!std::getline(in, ckLine))
		{
			err = "missing checkpoint line after: " + line;
			return false;
		}
		std::istringstream ck(ckLine);
		std::string tag;
		usize n = 0;
		ck >> tag >> n;
		if (tag != "ck")
		{
			err = "expected a checkpoint line, got: " + ckLine;
			return false;
		}
		for (usize i = 0; i < n; ++i)
		{
			std::string h;
			ck >> h;
			e.checkpoints.push_back(std::stoull(h, nullptr, 16));
		}
		out.push_back(std::move(e));
	}
	return true;
}

// Localizes a hash mismatch to the 64-frame window it first appears in, using
// the running-hash checkpoints either side carries.
void
reportFirstDivergence(
		const std::vector<u64> &baseCk, const std::vector<u64> &actualCk)
{
	const usize n = std::max(baseCk.size(), actualCk.size());
	usize firstDiff = n;
	for (usize k = 0; k < n; ++k)
	{
		const bool haveBase = k < baseCk.size();
		const bool haveActual = k < actualCk.size();
		if (!haveBase || !haveActual || baseCk[k] != actualCk[k])
		{
			firstDiff = k;
			break;
		}
	}

	if (firstDiff == 0 || n == 0)
	{
		std::printf("  first divergence before frame %llu\n",
				static_cast<unsigned long long>(kCheckpointInterval));
	}
	else if (firstDiff < n)
	{
		const u64 hi = (static_cast<u64>(firstDiff) + 1) * kCheckpointInterval;
		const u64 lo = hi - (kCheckpointInterval - 1);
		std::printf("  first divergence in frames %llu..%llu\n",
				static_cast<unsigned long long>(lo),
				static_cast<unsigned long long>(hi));
	}
	else
	{
		std::printf("  first divergence after last checkpoint\n");
	}
}

std::vector<BaselineEntry>
requireBaseline(const std::string &path, bool &ok)
{
	std::vector<BaselineEntry> baseline;
	std::string err;
	if (!loadBaseline(path, baseline, err))
	{
		std::fprintf(stderr, "%s\n", err.c_str());
		ok = false;
		return baseline;
	}
	if (baseline.size() != static_cast<usize>(kNumBattles))
	{
		std::fprintf(stderr, "baseline has %zu battles, expected %d\n",
				baseline.size(), kNumBattles);
		ok = false;
		return baseline;
	}
	ok = true;
	return baseline;
}

// --------------------------------------------------------------------------
// Modes.

int
doRecord(const std::string &path)
{
	std::vector<BattleResult> results;
	results.reserve(kNumBattles);
	for (int i = 0; i < kNumBattles; ++i)
		results.push_back(simulateBattle(seedFor(i), nullptr));

	std::ofstream out(path, std::ios::binary);
	if (!out)
	{
		std::fprintf(stderr, "cannot write %s\n", path.c_str());
		return 1;
	}
	writeBaseline(out, results);
	return 0;
}

int
doCompare(const std::string &path)
{
	bool ok = false;
	const std::vector<BaselineEntry> baseline = requireBaseline(path, ok);
	if (!ok)
		return 1;

	int mismatchedBattles = 0;
	for (int i = 0; i < kNumBattles; ++i)
	{
		const u32 seed = seedFor(i);
		const BattleResult a = simulateBattle(seed, nullptr);
		const BaselineEntry &w = baseline[static_cast<usize>(i)];

		bool printedHeader = false;
		bool battleOk = true;
		const auto field = [&](const char *name, auto wantV, auto gotV) {
			if (!(wantV == gotV))
			{
				if (!printedHeader)
				{
					std::printf("battle %d (seed %u):\n", i, seed);
					printedHeader = true;
				}
				std::ostringstream ws;
				std::ostringstream gs;
				ws << wantV;
				gs << gotV;
				std::printf("  %s: baseline=%s actual=%s\n", name,
						ws.str().c_str(), gs.str().c_str());
				battleOk = false;
			}
		};

		field("winner", w.winner, a.winner);
		field("endFrame", w.endFrame, a.endFrame);
		field("hash", hex16(w.hash), hex16(a.hash));
		field("crewLost0", w.crewLost0, a.crewLost0);
		field("crewLost1", w.crewLost1, a.crewLost1);
		field("shots", w.shots, a.shots);
		field("collisions", w.collisions, a.collisions);

		if (w.hash != a.hash)
			reportFirstDivergence(w.checkpoints, a.checkpoints);

		if (!battleOk)
			++mismatchedBattles;
	}

	if (mismatchedBattles > 0)
	{
		std::printf("%d of %d battles mismatched\n", mismatchedBattles,
				kNumBattles);
		return 1;
	}
	std::printf("all %d battles matched exactly\n", kNumBattles);
	return 0;
}

double
medianU64(std::vector<u64> v)
{
	if (v.empty())
		return 0.0;
	std::sort(v.begin(), v.end());
	const usize mid = v.size() / 2;
	if (v.size() % 2 == 1)
		return static_cast<double>(v[mid]);
	return (static_cast<double>(v[mid - 1]) + static_cast<double>(v[mid]))
			/ 2.0;
}

int
doSimilar(const std::string &path)
{
	bool ok = false;
	const std::vector<BaselineEntry> baseline = requireBaseline(path, ok);
	if (!ok)
		return 1;

	int winnerMatches = 0;
	i64 baseCrewTotal = 0;
	i64 crewDiffSum = 0;
	u64 baseShots = 0;
	u64 actualShots = 0;
	u64 baseCollisions = 0;
	u64 actualCollisions = 0;
	std::vector<u64> baseEndFrames;
	std::vector<u64> endFrameDiffs;

	for (int i = 0; i < kNumBattles; ++i)
	{
		const BaselineEntry &w = baseline[static_cast<usize>(i)];
		const BattleResult a = simulateBattle(seedFor(i), nullptr);

		if (w.winner == a.winner)
			++winnerMatches;

		const i64 baseCrew = static_cast<i64>(w.crewLost0) + w.crewLost1;
		const i64 actualCrew = static_cast<i64>(a.crewLost0) + a.crewLost1;
		const i64 crewDiff = actualCrew - baseCrew;
		baseCrewTotal += baseCrew;
		crewDiffSum += crewDiff < 0 ? -crewDiff : crewDiff;

		baseShots += w.shots;
		actualShots += a.shots;
		baseCollisions += w.collisions;
		actualCollisions += a.collisions;

		baseEndFrames.push_back(w.endFrame);
		endFrameDiffs.push_back(w.endFrame > a.endFrame
						? w.endFrame - a.endFrame
						: a.endFrame - w.endFrame);
	}

	// Provisional thresholds: the first stage with any actual divergence to
	// measure (Collide as a batch pass) will tell us whether these are right.
	bool pass = true;

	std::printf("winner: %d/%d battles agree (need >= 28)\n", winnerMatches,
			kNumBattles);
	if (winnerMatches < 28)
		pass = false;

	const double crewThreshold = static_cast<double>(baseCrewTotal) * 0.25;
	std::printf(
			"crewLost: |diff| sum %lld, baseline total %lld (threshold %.1f)\n",
			static_cast<long long>(crewDiffSum),
			static_cast<long long>(baseCrewTotal), crewThreshold);
	if (static_cast<double>(crewDiffSum) > crewThreshold)
		pass = false;

	const double shotsDiff = std::fabs(
			static_cast<double>(actualShots) - static_cast<double>(baseShots));
	const double shotsThreshold = static_cast<double>(baseShots) * 0.20;
	std::printf("shots: baseline %llu, actual %llu (threshold %.1f)\n",
			static_cast<unsigned long long>(baseShots),
			static_cast<unsigned long long>(actualShots), shotsThreshold);
	if (shotsDiff > shotsThreshold)
		pass = false;

	const double collisionsDiff = std::fabs(static_cast<double>(actualCollisions)
			- static_cast<double>(baseCollisions));
	const double collisionsThreshold =
			static_cast<double>(baseCollisions) * 0.30;
	std::printf("collisions: baseline %llu, actual %llu (threshold %.1f)\n",
			static_cast<unsigned long long>(baseCollisions),
			static_cast<unsigned long long>(actualCollisions),
			collisionsThreshold);
	if (collisionsDiff > collisionsThreshold)
		pass = false;

	const double endFrameMedianDiff = medianU64(endFrameDiffs);
	const double endFrameMedianBase = medianU64(baseEndFrames);
	const double endFrameThreshold = endFrameMedianBase * 0.15;
	std::printf(
			"endFrame: median |diff| %.1f, baseline median %.1f (threshold %.1f)\n",
			endFrameMedianDiff, endFrameMedianBase, endFrameThreshold);
	if (endFrameMedianDiff > endFrameThreshold)
		pass = false;

	std::printf("similarity: %s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int
doTrace(int battleIndex, const std::string &outPath)
{
	if (battleIndex < 0 || battleIndex >= kNumBattles)
	{
		std::fprintf(stderr, "battle index %d out of range [0, %d)\n",
				battleIndex, kNumBattles);
		return 1;
	}
	std::ofstream out(outPath, std::ios::binary);
	if (!out)
	{
		std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
		return 1;
	}
	(void)simulateBattle(seedFor(battleIndex), &out);
	out.flush();
	return 0;
}

}  // namespace

int
main(int argc, char **argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--record") == 0)
		return doRecord(argv[2]);
	if (argc == 3 && std::strcmp(argv[1], "--compare") == 0)
		return doCompare(argv[2]);
	if (argc == 3 && std::strcmp(argv[1], "--similar") == 0)
		return doSimilar(argv[2]);
	if (argc == 4 && std::strcmp(argv[1], "--trace") == 0)
		return doTrace(std::atoi(argv[2]), argv[3]);

	std::fprintf(stderr,
			"usage: replay_test --record <file> | --compare <file> | "
			"--similar <file> | --trace <battleIndex> <file>\n");
	return 1;
}
