#include "Body.h"

#include <Ext/WeaponType/Body.h>

#include <FootClass.h>
#include <TechnoClass.h>

#include <algorithm>
#include <cmath>

// In this file the "firer end" of a wave is what YRpp calls its start, and the "target end" is what YRpp calls its end.

// Returns the extension of the weapon that fired a wave, or nullptr when the weapon is unknown.
static WeaponTypeExt* GetWaveWeaponExt(WaveClass* pThis)
{
	const auto pExt = WaveExt::TryFetch(pThis);

	return pExt && pExt->Weapon ? WeaponTypeExt::Fetch(pExt->Weapon) : nullptr;
}

#pragma region MinimumLength

// The WaveClass constructor removes the wave right away when its two ends are less than 240 leptons apart horizontally.
// No WaveType's drawing needs that much, only a wave longer than zero with room for its tips. The check is only made when
// the wave is created: an existing wave is redrawn every frame between its firer and its target and is never removed for
// being short, so short waves are already drawn whenever the target comes closer later. Sonic drawing looks its colors up
// by each pixel's screen distance from the start of the wave, which a shorter wave only makes smaller. So Wave.MinimumLength
// replaces the 240 for every wave fired by a weapon. At this address the game has compared the distance with 240 and is
// about to branch on the result, so the test is redone here with the weapon's minimum.
DEFINE_HOOK(0x75EA8C, WaveClass_CTOR_MinimumLength, 0x5)
{
	enum { KeepWave = 0x75EAA3, RemoveWave = 0x75EA93 };

	// The game's own minimum (0x7E5F78).
	constexpr int DefaultMinimumLength = 240;

	GET(WaveClass* const, pThis, ESI);
	GET(const CoordStruct* const, pFrom, EBP);
	GET_STACK(const CoordStruct* const, pTo, 0x18);

	// WaveClass_CTOR has already given the wave its extension and weapon. The value was limited to
	// WeaponTypeExt::WaveMinimumLengthMinValue - WaveMinimumLengthMaxValue when read.
	const auto pWeaponExt = GetWaveWeaponExt(pThis);
	const long long minimumLength = pWeaponExt ? pWeaponExt->Wave_MinimumLength.Get(DefaultMinimumLength) : DefaultMinimumLength;

	// Compared squared, so no square root is needed.
	const long long dx = pFrom->X - pTo->X;
	const long long dy = pFrom->Y - pTo->Y;

	return dx * dx + dy * dy < minimumLength * minimumLength ? RemoveWave : KeepWave;
}

#pragma endregion

#pragma region LaserWaves

// Returns the extension of the weapon that fired a laser wave, or nullptr for other waves or when the weapon is unknown.
// Sonic wave drawing indexes fixed-size color data by a pixel's distance from the wave (see the crash fixes in
// Misc/Hooks.BugFixes.cpp), so Wave.Shape and Wave.ExtraDraw only apply to laser waves.
static WeaponTypeExt* GetLaserWaveWeaponExt(WaveClass* pThis)
{
	if (pThis->Type != WaveType::Laser && pThis->Type != WaveType::BigLaser)
		return nullptr;

	return GetWaveWeaponExt(pThis);
}

// WaveClass::Draw_NonMagnetic does not draw the beam between the two points it is given (the firer's FLH and the target).
// It first moves both ends outward along the line between them by a fraction of the distance between them, read from a
// per-WaveType table at 0x7F6BB4 (5% for laser waves), so the beam starts behind the FLH and ends past the target, more so
// the further away the target is. At this address both moved ends are on the stack, and the game is about to derive the
// beam's length, direction and outline from them, so replacing them here moves the whole beam.
DEFINE_HOOK(0x761ADF, WaveClass_DrawNonMagnetic_ExtraDraw, 0x6)
{
	GET(WaveClass* const, pThis, EDI);

	const auto pWeaponExt = GetLaserWaveWeaponExt(pThis);

	if (!pWeaponExt || pWeaponExt->Wave_ExtraDraw.empty())
		return 0;

	// The points the game was asked to draw the wave between.
	GET_BASE(const CoordStruct*, pFirer, 0x8);
	GET_BASE(const CoordStruct*, pTarget, 0xC);

	// The moved ends: firerEnd becomes the wave's location and the tip of its firer end, targetEnd the tip of its target end.
	REF_STACK(CoordStruct, firerEnd, 0x28);
	REF_STACK(CoordStruct, targetEnd, 0x34);

	// Wave.ExtraDraw=<firer end>,<target end>; a missing second value repeats the first.
	// The values were limited to 0 - WeaponTypeExt::WaveExtraDrawMaxValue when read.
	const auto& extraDraw = pWeaponExt->Wave_ExtraDraw;
	const double firerExtra = extraDraw[0];
	const double targetExtra = extraDraw.size() > 1 ? extraDraw[1] : firerExtra;

	const auto extend = [](const CoordStruct& from, const CoordStruct& other, double extra)
	{
		return CoordStruct
		{
			static_cast<int>(std::lround(from.X + (from.X - other.X) * extra)),
			static_cast<int>(std::lround(from.Y + (from.Y - other.Y) * extra)),
			static_cast<int>(std::lround(from.Z + (from.Z - other.Z) * extra))
		};
	};

	firerEnd = extend(*pFirer, *pTarget, firerExtra);
	targetEnd = extend(*pTarget, *pFirer, targetExtra);

	// The game code at this address continues with these registers: it stores ESI as the X distance between the ends and
	// subtracts EAX from EBX for the Y distance.
	R->ESI(firerEnd.X - targetEnd.X);
	R->EBX(firerEnd.Y);
	R->EAX(targetEnd.Y);

	return 0;
}

// WaveClass::Draw_NonMagnetic builds the outline of non-magnetron waves as a six-point polygon: one tip on the beam's
// center line at each end (the "middles") and two side corners per end. The corners come from a hardcoded table per
// WaveType and are expressed in a local frame where X runs along the beam from the firer end (X = 0) to the target end
// (X = length) and Y is the sideways offset from the center line. For laser waves the corners sit 34 (WaveType 1) or 27
// (WaveType 2) leptons inside the tips and 44 or 34 leptons out from the center line. Note that Ares 3.0 creates
// WaveType 1 for Wave.IsLaser and WaveType 2 for Wave.IsBigLaser, the reverse of the YRpp enum names.
// At this address the corners are on the stack and are still in that local frame: the game next derives each corner's
// height from its X and then rotates the corners into world space, so changing them here reshapes the whole beam.
// The pixels inside the outline all get the same tint, so the outline alone defines the look of the beam.
DEFINE_HOOK(0x761CD1, WaveClass_DrawNonMagnetic_Shape, 0x8)
{
	GET(WaveClass* const, pThis, EDI);

	const auto pWeaponExt = GetLaserWaveWeaponExt(pThis);

	if (!pWeaponExt || pWeaponExt->Wave_Shape.empty())
		return 0;

	const auto& shape = pWeaponExt->Wave_Shape;

	GET_STACK(const int, length, 0x18);

	// Corners in the local beam frame.
	REF_STACK(Vector3D<float>, firerSide1, 0x4C); // becomes WaveStartSide1
	REF_STACK(Vector3D<float>, firerSide2, 0x58); // becomes WaveStartSide2
	REF_STACK(Vector3D<float>, targetSide1, 0x64); // becomes WaveEndSide1
	REF_STACK(Vector3D<float>, targetSide2, 0x40); // becomes WaveEndSide2

	// Copies of each corner's X that the game reads to derive the corner's height.
	REF_STACK(float, firerSide1HeightX, 0x8C);
	REF_STACK(float, firerSide2HeightX, 0x0C);
	REF_STACK(float, targetSide1HeightX, 0x74);
	REF_STACK(float, targetSide2HeightX, 0x80);

	// Wave.Shape=<firer width>,<target width>,<firer tip>,<target tip>; a missing second value of a pair repeats the first.
	// The values were limited to 0 - WeaponTypeExt::WaveShapeMaxValue when read.
	const auto getValue = [&shape](size_t index, size_t fallbackIndex)
	{
		return static_cast<float>(shape[index < shape.size() ? index : fallbackIndex]);
	};

	const float firerHalfWidth = getValue(0, 0) / 2.0f;
	const float targetHalfWidth = getValue(1, 0) / 2.0f;

	// Keep each corner on its original side of the center line.
	firerSide1.Y = std::copysign(firerHalfWidth, firerSide1.Y);
	firerSide2.Y = std::copysign(firerHalfWidth, firerSide2.Y);
	targetSide1.Y = std::copysign(targetHalfWidth, targetSide1.Y);
	targetSide2.Y = std::copysign(targetHalfWidth, targetSide2.Y);

	if (length <= 0)
		return 0;

	// Tip lengths: from Wave.Shape if given, otherwise the wave type's default read back from the corners.
	const float beamLength = static_cast<float>(length);
	float firerTip = shape.size() > 2 ? getValue(2, 2) : firerSide1.X;
	float targetTip = shape.size() > 2 ? getValue(3, 2) : beamLength - targetSide1.X;

	// If the tips are together longer than the beam, the corners of the two ends would cross and the outline could not
	// be drawn, so shrink both tips in proportion to fit.
	const float totalTip = firerTip + targetTip;

	if (totalTip > beamLength)
	{
		const float scale = beamLength / totalTip;
		firerTip *= scale;
		targetTip *= scale;
	}

	// The game fills the outline row by row and can only do that when every screen row crosses it once, which a convex
	// outline guarantees at every beam angle. A tapered beam stops being convex when the corners of its narrow end sit
	// inside the straight line from the wide end's corner to the narrow end's tip, so shorten such a tip until the corner
	// is on that line. Shortening one tip only makes the condition easier for the other end, so one pass is enough.
	if (firerHalfWidth > 0.0f)
		targetTip = std::min(targetTip, targetHalfWidth * (beamLength - firerTip) / firerHalfWidth);

	if (targetHalfWidth > 0.0f)
		firerTip = std::min(firerTip, firerHalfWidth * (beamLength - targetTip) / targetHalfWidth);

	const float targetX = beamLength - targetTip;

	firerSide1.X = firerSide1HeightX = firerTip;
	firerSide2.X = firerSide2HeightX = firerTip;
	targetSide1.X = targetSide1HeightX = targetX;
	targetSide2.X = targetSide2HeightX = targetX;

	return 0;
}

// WaveClass::Draw_NonMagnetic builds outlines of six points.
constexpr int MaxOutlinePoints = 6;

// Returns whether walking around the outline only goes down once, from its top to its bottom, which is what the game's
// polygon fill needs (see WaveClass_RasterizePolygon_NonMonotoneFix in Misc/Hooks.BugFixes.cpp, which uses the same test).
static bool IsMonotoneInY(const Point2D* pPoints, int count)
{
	int minY = pPoints[0].Y;
	int maxY = pPoints[0].Y;
	long long totalDown = 0;

	for (int i = 0; i < count; ++i)
	{
		const int y = pPoints[i].Y;
		const int nextY = pPoints[(i + 1) % count].Y;

		minY = std::min(minY, y);
		maxY = std::max(maxY, y);

		if (nextY > y)
			totalDown += nextY - y;
	}

	return totalDown == static_cast<long long>(maxY) - minY;
}

// Writes the convex hull of up to MaxOutlinePoints points to pHull, in order around it, and returns how many points it has,
// or 0 if all the points are in one line. pHull must have room for count points. Points on the hull's edges are left out.
static int GetConvexHull(const Point2D* pPoints, int count, Point2D* pHull)
{
	Point2D sorted[MaxOutlinePoints];
	std::copy(pPoints, pPoints + count, sorted);
	std::sort(sorted, sorted + count, [](const Point2D& a, const Point2D& b) { return a.X != b.X ? a.X < b.X : a.Y < b.Y; });
	count = static_cast<int>(std::unique(sorted, sorted + count) - sorted);

	if (count < 3)
		return 0;

	// Greater than 0 if o -> a -> b turns one way, less than 0 if it turns the other way, 0 if the points are in line.
	const auto turn = [](const Point2D& o, const Point2D& a, const Point2D& b)
	{
		return static_cast<long long>(a.X - o.X) * (b.Y - o.Y) - static_cast<long long>(a.Y - o.Y) * (b.X - o.X);
	};

	// Andrew's monotone chain: build one half of the hull left to right and the other half back, keeping only points where
	// the outline turns the same way. The last point of each half is the first point of the other, so it is dropped.
	Point2D hull[MaxOutlinePoints * 2];
	int size = 0;

	for (int i = 0; i < count; ++i)
	{
		while (size >= 2 && turn(hull[size - 2], hull[size - 1], sorted[i]) <= 0)
			--size;

		hull[size++] = sorted[i];
	}

	for (int i = count - 2, lowerSize = size + 1; i >= 0; --i)
	{
		while (size >= lowerSize && turn(hull[size - 2], hull[size - 1], sorted[i]) <= 0)
			--size;

		hull[size++] = sorted[i];
	}

	--size;

	if (size < 3)
		return 0;

	std::copy(hull, hull + size, pHull);
	return size;
}

// Returns twice the signed area of an outline. Its sign tells which way round the points go.
static long long GetDoubleSignedArea(const Point2D* pPoints, int count)
{
	long long area = 0;

	for (int i = 0; i < count; ++i)
	{
		const auto& a = pPoints[i];
		const auto& b = pPoints[(i + 1) % count];
		area += static_cast<long long>(a.X) * b.Y - static_cast<long long>(b.X) * a.Y;
	}

	return area;
}

// The game rounds each point of the wave's outline to whole screen pixels. On a very flat end, such as a short tip or none
// at all (Wave.Shape), the tip can then end up a pixel inside the line between its side corners. That bends the outline
// inward, and if it bends up and back down, WaveClass_RasterizePolygon_NonMonotoneFix refuses to fill it and the beam is
// not drawn at all. This happens at the same few angles every time, mostly when the beam points straight up on the screen.
// Before rounding, a laser wave's outline is convex (Wave.Shape keeps it so), so such an outline is filled here as the
// convex hull of its points instead. The hull contains every point of the rounded outline and differs from it by about a
// pixel at most, and a convex outline can always be filled. Outlines the game can already fill are left exactly as they are.
// The outline stored in the wave is not changed, because the wave's constructor and its first update rewrite some of its
// points by position after Draw_NonMagnetic builds it. The hull is only handed to the polygon fill, every frame.
// At this address the laser draw (0x7609E0, only called for WaveType 1 and 2) is about to pass the outline to the polygon
// fill (0x63F080) in ECX. EDX already holds where the fill writes its result, in the wave either way.
DEFINE_HOOK(0x760A71, WaveClass_DrawLaser_RoundingFix, 0x6)
{
	enum { CallFill = 0x760A77 };

	GET(WaveClass* const, pThis, EBX);

	// The outline as the polygon fill reads it: { int Count; Point2D* Points; }, stored at PointData_Counter.
	struct Outline
	{
		int Count;
		Point2D* Points;
	};

	// The replaced instruction, lea ecx, [ebx+148h].
	R->ECX(reinterpret_cast<Outline*>(&pThis->PointData_Counter));

	// When both ends are at the same horizontal position the wave has no direction, and its side corners are not a real
	// shape (see WaveClass_RasterizePolygon_NonMonotoneFix). Leave those to be skipped.
	if (pThis->LimboCoords.X == pThis->Pos0.X && pThis->LimboCoords.Y == pThis->Pos0.Y)
		return CallFill;

	const auto pPoints = reinterpret_cast<const Point2D*>(pThis->PointData_Pointer);
	const int count = pThis->PointData_Counter;

	if (!pPoints || count < 3 || count > MaxOutlinePoints || IsMonotoneInY(pPoints, count))
		return CallFill;

	// Only used until the fill returns, so one buffer for all waves is enough.
	static Point2D hull[MaxOutlinePoints];
	static Outline hullOutline;

	const int hullCount = GetConvexHull(pPoints, count, hull);

	if (!hullCount)
		return CallFill;

	// Keep the points going round the same way as the game's outline.
	if ((GetDoubleSignedArea(pPoints, count) < 0) != (GetDoubleSignedArea(hull, hullCount) < 0))
		std::reverse(hull, hull + hullCount);

	hullOutline = { hullCount, hull };
	R->ECX(&hullOutline);

	return CallFill;
}

#pragma endregion

#pragma region Magnetron

// WaveClass::Draw_Magnetic(From, To) builds the outline of magnetron waves, the only WaveType that uses it. With Ares, From is
// the firer's FLH and To the target's center, or the other way round when one of the weapon's Wave.ReverseAgainst* applies
// (Wave.ReverseAgainstVehicles is on by default for IsMagBeam weapons, as vanilla drew such waves from vehicles to the firer).
// The outline is a strip 100 leptons wide from From to To, after To has been raised by 50 leptons. Its corners come from a
// table in a local frame where X runs along the ground from From (X = 0) to To (X = D, the ground distance between them).
// Each corner then gets the height From.Z + X * (To.Z - From.Z) / L, but L is the full 3D distance between the two ends,
// not D. So the To end only rises by the height difference times D / L, the cosine of the beam's slope, and the beam ends
// below To, the more so the steeper it is. A unit lifted by a locomotor warhead close to its firer gets a beam that barely
// rises at all.
// At this address the game is about to round D (a float at [esp+0x40]) for the corners' X. L (an integer at [esp+0x24]) is
// only used for the corner heights after this, so it is replaced with D here as well, and the To end of the strip reaches To.
DEFINE_HOOK(0x762633, WaveClass_DrawMagnetic_HeightFix, 0x9)
{
	enum { SkipGameCode = 0x76263C };

	GET_STACK(const float, groundDistance, 0x40);

	// The game rounds towards zero here as well (0x7C5F00).
	const int distance = static_cast<int>(groundDistance);

	// When To is straight above From, D is 0 and so is every corner's X, so all corners get From's height whatever the
	// divisor is. Only avoid dividing by zero.
	R->Stack(0x24, std::max(distance, 1));
	R->EAX(distance);

	return SkipGameCode;
}

// A locomotor warhead (IsLocomotor) keeps hold of its target whether or not the target stays in weapon range: the target is
// lifted up into the air, then pulled towards its firer, often closer than the weapon's MinimumRange. Vanilla draws the
// magnetron beam for as long as its firer keeps that target. Ares (WaveClass_Update_Beam, 0x762B62) also ends a magnetron
// beam in any frame where Owner->IsCloseEnough(Target, weapon) fails, a range and MinimumRange check in 3D, so the beam
// disappears while the target is still held. While a magnetron wave whose firer holds its target with a locomotor warhead is
// updated, that firer's range check against that target passes, so the beam lasts as long as the hold, as in vanilla.
namespace MagnetronBeamTemp
{
	TechnoClass* pHoldingOwner = nullptr;
}

// In the wave update (0x762AF0), where both of its first branches meet, before Ares's WaveClass_Update_Beam.
DEFINE_HOOK(0x762B29, WaveClass_AI_MagnetronHoldSet, 0x6)
{
	GET(WaveClass* const, pThis, ESI);

	const auto pOwner = pThis->Owner;
	const bool holding = pThis->Type == WaveType::Magnetron && pOwner && pThis->Target
		&& pOwner->LocomotorTarget == pThis->Target;

	MagnetronBeamTemp::pHoldingOwner = holding ? pOwner : nullptr;

	return 0;
}

// The beam has been checked and drawn (Ares always continues at 0x762D57, where WaveClass_AI_TargetUnset is).
DEFINE_HOOK(0x762D5D, WaveClass_AI_MagnetronHoldUnset, 0x6)
{
	MagnetronBeamTemp::pHoldingOwner = nullptr;

	return 0;
}

// TechnoClass::IsCloseEnough(AbstractClass* pTarget, int weaponIndex), after it has returned true for no target.
DEFINE_HOOK(0x6F77C9, TechnoClass_IsCloseEnough_MagnetronHold, 0x6)
{
	enum { ReturnTrue = 0x6F77BF };

	GET(TechnoClass* const, pThis, ESI);
	GET(AbstractClass* const, pTarget, EBX);

	const auto pOwner = MagnetronBeamTemp::pHoldingOwner;

	return pOwner && pThis == pOwner && pTarget == pThis->LocomotorTarget ? ReturnTrue : 0;
}

#pragma endregion
