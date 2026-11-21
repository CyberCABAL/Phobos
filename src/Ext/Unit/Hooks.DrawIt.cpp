#include <Ext/UnitType/Body.h>
#include <Ext/Rules/Body.h>
#include <Utilities/AresHelper.h>
#include <Conversions.h>

DEFINE_HOOK(0x73C7AC, UnitClass_DrawAsSHP_DrawTurret_TintFix, 0x6)
{
	enum { SkipDrawCode = 0x73CE00 };

	GET(UnitClass*, pThis, EBP);

	const auto pThisType = pThis->Type;
	const VoxelStruct barrelVoxel = pThisType->BarrelVoxel;

	if (barrelVoxel.VXL && barrelVoxel.HVA)
		return 0;

	GET(UnitTypeClass*, pType, ECX);
	GET(SHPStruct*, pShape, EDI);
	GET(const int, bodyFrameIdx, EBX);
	REF_STACK(Point2D, location, STACK_OFFSET(0x128, 0x4));
	REF_STACK(RectangleStruct, bounds, STACK_OFFSET(0x128, 0xC));
	GET_STACK(const int, extraLight, STACK_OFFSET(0x128, 0x1C));

	const bool tooBigToFitUnderBridge = pType->TooBigToFitUnderBridge
		&& reinterpret_cast<bool(__thiscall*)(TechnoClass*)>(0x703B10)(pThis) && !reinterpret_cast<int(__thiscall*)(TechnoClass*)>(0x703E70)(pThis);
	const int zAdjust = tooBigToFitUnderBridge ? -16 : 0;
	const ZGradient zGradient = tooBigToFitUnderBridge ? ZGradient::Ground : pThis->GetZGradient();

	pThis->Draw_A_SHP(pShape, bodyFrameIdx, &location, &bounds, 0, 256, zAdjust, zGradient, 0, extraLight, 0, 0, 0, 0, 0, 0);

	const auto pTurretShape = UnitTypeExt::Fetch(pType)->TurretShape;
	const int StartFrame = pTurretShape ? 0 : (pType->WalkFrames * pType->Facings);

	if (pTurretShape)
		pShape = pTurretShape;

	const auto secondaryDir = pThis->SecondaryFacing.Current();
	const int frameIdx = secondaryDir.GetFacing<32>(4) + StartFrame;

	const auto primaryDir = pThis->PrimaryFacing.Current();
	const double bodyRad = primaryDir.GetRadian<32>();
	Matrix3D mtx = Matrix3D::GetIdentity();
	mtx.RotateZ(static_cast<float>(bodyRad));

	TechnoTypeExt::ApplyTurretOffset(pThisType, &mtx);
	const double turretRad = pType->Turret ? secondaryDir.GetRadian<32>() : bodyRad;
	mtx.RotateZ(static_cast<float>(turretRad - bodyRad));

	const auto res = mtx.GetTranslation();
	const auto offset = CoordStruct { static_cast<int>(res.X), static_cast<int>(-res.Y), static_cast<int>(res.Z) };
	Point2D drawPoint = location + TacticalClass::Instance->CoordsToScreen(offset);

	const bool originalDrawShadow = std::exchange(Game::bDrawShadow, false);
	pThis->Draw_A_SHP(pShape, frameIdx, &drawPoint, &bounds, 0, 256, static_cast<DWORD>(-32), zGradient, 0, extraLight, 0, 0, 0, 0, 0, 0);
	Game::bDrawShadow = originalDrawShadow;
	return SkipDrawCode;
}

DEFINE_HOOK(0x73CCF4, UnitClass_DrawSHP_FacingsB_TurretShape, 0xA)
{
	enum { SkipGameCode = 0x73CD06 };

	GET(UnitClass*, pThis, EBP);
	GET(UnitTypeClass*, pType, ECX);

	const auto pTurretShape = UnitTypeExt::Fetch(pType)->TurretShape;
	const int StartFrame = pTurretShape ? 0 : (pType->WalkFrames * pType->Facings);
	const int frameIdx = pThis->SecondaryFacing.Current().GetFacing<32>(4) + StartFrame;

	if (pTurretShape)
		R->EDI(pTurretShape);

	R->ECX(pThis);
	R->EAX(frameIdx);
	return 0x73CD06;
}

#pragma region TerrainDisguise

// Whether the vehicle is being drawn as the TerrainType it is disguised as (e.g. a Mirage Tank's tree).
static bool IsDrawingTerrainDisguise(UnitClass* pThis, SHPStruct* pShape)
{
	auto const pDisguise = pThis->Disguise;

	return pShape && pThis->IsDisguised() && pDisguise
		&& pDisguise->WhatAmI() == AbstractType::TerrainType && pShape == pDisguise->Image;
}

// The body facing frame index the game (Facings=8) or Ares (other Facings) picks for an SHP vehicle that is not disguised.
static int GetBodyFacingIndex(UnitClass* pThis)
{
	const int facings = pThis->Type->Facings;

	if (facings <= 0 || (facings != 8 && !AresHelper::CanUseAres))
		return 0;

	// Ares uses the highest power of two that fits in Facings.
	const unsigned int bits = Conversions::Int2Highest(facings);

	if (bits < 3 || bits > 15)
		return 0;

	// Frame 0 faces up on the screen, an eighth of a turn from the game's direction 0.
	const unsigned int raw = pThis->PrimaryFacing.Current().Raw + 0x2000u;
	return static_cast<int>((((raw >> (15 - bits)) + 1) >> 1) & ((1u << bits) - 1));
}

// Frame selection for a disguised vehicle drawn as an SHP. EBX holds the body facing index here.
DEFINE_HOOK(0x73C64B, UnitClass_DrawAsSHP_DisguiseFrame, 0x6)
{
	enum { SkipFrameSelection = 0x73C725 };

	GET(UnitClass*, pThis, EBP);
	GET(SHPStruct*, pShape, EDI);

	if (!RulesExt::Global()->StillDisguise_DrawFix || !pThis->IsDisguised())
		return 0;

	// The game would pick the tree's frame from the vehicle's own firing, idle and walking animations,
	// cycling through the tree's normal and shadow frames.
	if (IsDrawingTerrainDisguise(pThis, pShape))
	{
		R->EBX(0);
		R->ECX(pThis->Type);
		return SkipFrameSelection;
	}

	// The game (and Ares) use facing 0 for any disguised vehicle, which is meant for its disguise's image.
	// Without this, the owner, who sees the vehicle itself most of the time, sees it always facing north.
	auto const pDisguise = pThis->Disguise;

	if (pDisguise && pShape != pDisguise->Image)
		R->EBX(GetBodyFacingIndex(pThis));

	return 0;
}

// The game draws the tree at the vehicle's height, so a hovering vehicle's tree would float and bob above its shadow.
DEFINE_HOOK(0x73CE0D, UnitClass_DrawAsSHP_TerrainDisguisePosition, 0x6)
{
	GET(UnitClass*, pThis, EBP);
	GET(SHPStruct*, pShape, EDI);
	REF_STACK(Point2D, location, STACK_OFFSET(0x128, 0x4));

	// The shadow point is the vehicle's height above the ground (or bridge) in pixels, zero for ground vehicles.
	if (RulesExt::Global()->StillDisguise_DrawFix && IsDrawingTerrainDisguise(pThis, pShape))
		location += pThis->Locomotor->Shadow_Point();

	return 0;
}

// The owner sees a disguised vehicle fade between itself and its disguise. Voxels drawn without the voxel cache
// (e.g. DisableVoxelCache=yes) would get that fade twice: once when drawn into the vehicle's image, and again when
// the image is drawn, which changes their colours while fading. Cached voxels and SHPs only get it from the image.
DEFINE_HOOK(0x707090, TechnoClass_RenderVoxel_DisguiseFadeOnce, 0x6)
{
	enum { SkipDisguiseFade = 0x70709E };

	GET(TechnoClass*, pThis, EBP);

	if (!RulesExt::Global()->StillDisguise_DrawFix || pThis->WhatAmI() != AbstractType::Unit)
		return 0;

	// YRpp's TerrainPalette is really the flag UnitClass::DrawAsVXL and DrawAsSHP set while they draw into the vehicle's image.
	return static_cast<UnitClass*>(pThis)->TerrainPalette ? SkipDisguiseFade : 0;
}

#pragma endregion
