#include "Body.h"

#include <OverlayTypeClass.h>

#include <Ext/Building/Body.h>
#include <Ext/Cell/Body.h>

// Art.Facings: one BuildingType whose image is picked per building from the facing it was placed with
// (the facing field of map [Structures] lines, or the facing a trigger or the player placed it with).

// The game reloads a type's own image here (rules reading, theater change, savegame loading),
// so the variant images are reloaded on their next use too.
DEFINE_HOOK(0x45FA76, BuildingTypeClass_Load2DArt_ArtFacings, 0x7)
{
	GET(BuildingTypeClass*, pThis, EBP);

	if (auto const pTypeExt = BuildingTypeExt::TryFetch(pThis))
		pTypeExt->Art_Facings_ImagesTheater.reset();

	return 0;
}

// BuildingClass::Unlimbo, right before it calls TechnoClass::Unlimbo with the final facing.
DEFINE_HOOK(0x440B07, BuildingClass_Unlimbo_ArtFacings, 0x5)
{
	GET(BuildingClass*, pThis, ESI);
	GET_STACK(const DWORD, facing, 0x4C);

	if (auto const pExt = BuildingExt::TryFetch(pThis))
		pExt->ArtFacingIndex = BuildingTypeExt::Fetch(pThis->Type)->GetArtFacingIndex(static_cast<DirType>(facing & 0xFF));

	return 0;
}

static SHPStruct* GetArtFacingImage(BuildingTypeClass* pType, int index)
{
	return index >= 0 ? BuildingTypeExt::Fetch(pType)->GetArtFacingImage(index) : nullptr;
}

// Replaces BuildingClass::GetImage (0x4513D0), which every building body draw uses.
static SHPStruct* __fastcall BuildingClass_GetImage_ArtFacings(BuildingClass* pThis)
{
	auto const pType = pThis->Type;

	// vanilla: the buildup while under construction or not placed yet
	if (pThis->BState == static_cast<int>(BStateType::Construction) || !pThis->ActuallyPlacedOnMap)
		return pType->LoadBuildup();

	if (auto const pExt = BuildingExt::TryFetch(pThis))
	{
		if (auto const pImage = GetArtFacingImage(pType, pExt->ArtFacingIndex))
			return pImage;
	}

	return pType->GetImage();
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E3F28, BuildingClass_GetImage_ArtFacings)

// BuildingClass::Update calls the damage fire spawner (0x43C0D0), which reads Type->DamageFireOffset.
// Show it the Art.Facings entry's offsets for this one call.
DEFINE_HOOK(0x43FC92, BuildingClass_AI_DamageFires_ArtFacings, 0x5)
{
	enum { SkipGameCode = 0x43FC97 };

	GET(BuildingClass*, pThis, ESI);

	auto const pExt = BuildingExt::TryFetch(pThis);

	if (!pExt || pExt->ArtFacingIndex < 0)
		return 0;

	auto const pOffsets = BuildingTypeExt::Fetch(pThis->Type)->GetArtFacingDamageFireOffsets(pExt->ArtFacingIndex);

	if (!pOffsets)
		return 0;

	auto& typeOffsets = pThis->Type->DamageFireOffset;
	Point2D savedOffsets[std::extent_v<decltype(BuildingTypeClass::DamageFireOffset)>];
	std::copy(std::begin(typeOffsets), std::end(typeOffsets), savedOffsets);
	std::copy(pOffsets, pOffsets + std::size(typeOffsets), typeOffsets);

	reinterpret_cast<void(__fastcall*)(BuildingClass*, void*)>(0x43C0D0)(pThis, nullptr);

	std::copy(std::begin(savedOffsets), std::end(savedOffsets), typeOffsets);

	return SkipGameCode;
}

#pragma region Rubble

// A destroyed building with LeaveRubble=yes hands its rubble to its top-left cell: CellClass::Rubble becomes its
// type, and the cell draws that type's rubble from then on (GetRubbleShape 0x45F160 / 0x45F1D0): the type's
// separate Rubble= image if it has one, otherwise frame 3 of the type's own image. The variant is kept on the cell
// too, and the cell's rubble drawing gets the variant's image instead of the type's own.
static_assert(offsetof(BuildingTypeClass, Rubble) == 0x14F4);

// BuildingClass::AfterDestruction (0x441F60), right where the type is stored on the cell.
DEFINE_HOOK(0x442079, BuildingClass_AfterDestruction_RubbleArtFacing, 0x6)
{
	GET(BuildingClass*, pThis, EBX);
	GET(CellClass*, pCell, EDI);

	if (auto const pCellExt = CellExt::TryFetch(pCell))
	{
		auto const pExt = BuildingExt::TryFetch(pThis);
		pCellExt->RubbleArtFacingIndex = pExt ? pExt->ArtFacingIndex : -1;
	}

	return 0;
}

// The variant image to use instead of the rubble shape the game picked for a cell, or nullptr.
static SHPStruct* GetRubbleArtFacingImage(CellClass* pCell, SHPStruct* pShape)
{
	auto const pType = pCell->Rubble;

	// a separate Rubble= image is shared by all variants
	if (!pType || !pShape || pType->Rubble)
		return nullptr;

	const int overlayIndex = pCell->OverlayTypeIndex;

	if (overlayIndex < 0 || overlayIndex >= OverlayTypeClass::Array.Count || !OverlayTypeClass::Array[overlayIndex]->IsRubble)
		return nullptr;

	auto const pCellExt = CellExt::TryFetch(pCell);

	if (!pCellExt || pCellExt->RubbleArtFacingIndex < 0 || pShape != pType->GetImage())
		return nullptr;

	return BuildingTypeExt::Fetch(pType)->GetArtFacingImage(pCellExt->RubbleArtFacingIndex);
}

// CellClass overlay draw (0x47F6A0), after GetRubbleShape succeeded.
DEFINE_HOOK(0x47FAFD, CellClass_DrawOverlay_RubbleArtFacing, 0x5)
{
	GET(CellClass*, pCell, ESI);
	REF_STACK(SHPStruct*, pShape, 0x10);

	if (auto const pImage = GetRubbleArtFacingImage(pCell, pShape))
		pShape = pImage;

	return 0;
}

// CellClass overlay shadow draw (0x47F510), after GetRubbleShape succeeded.
DEFINE_HOOK(0x47F665, CellClass_DrawOverlayShadow_RubbleArtFacing, 0x6)
{
	GET(CellClass*, pCell, ESI);
	REF_STACK(SHPStruct*, pShape, 0x30);

	if (auto const pImage = GetRubbleArtFacingImage(pCell, pShape))
		pShape = pImage;

	return 0;
}

// CellClass overlay redraw area (0x47FB90), where all its overlay kinds have picked their shape.
DEFINE_HOOK(0x47FCB1, CellClass_GetOverlayRect_RubbleArtFacing, 0x8)
{
	GET(CellClass*, pCell, ESI);
	REF_STACK(SHPStruct*, pShape, 0x10);

	if (auto const pImage = GetRubbleArtFacingImage(pCell, pShape))
		pShape = pImage;

	return 0;
}

#pragma endregion

#pragma region FogOfWar

// FoggedObjectClass is not in YRpp. A fogged building keeps draw records at +0x60, 0x10 bytes each;
// the first one is the building body (see 0x4D0EF0 and 0x4D1890).
struct FoggedDrawRecord
{
	ObjectTypeClass* Type;
	int Frame;
	bool Flag;
	int Extra; // the game only reads this for the anim records, never for the body record
};
static_assert(sizeof(FoggedDrawRecord) == 0x10);

// FoggedObjectClass ctor for a building: keep the variant in the body record's unused field (index + 1).
DEFINE_HOOK(0x4D1167, FoggedObjectClass_CTOR_Building_ArtFacings, 0x6)
{
	GET(BuildingClass*, pBuilding, EBX);

	if (auto const pExt = BuildingExt::TryFetch(pBuilding))
	{
		if (pExt->ArtFacingIndex >= 0)
			R->EAX(pExt->ArtFacingIndex + 1); // stored to the record by the stolen bytes
	}

	return 0;
}

// FoggedObjectClass draw, building body: the image the game got from the record's type is in EAX.
DEFINE_HOOK(0x4D1B2E, FoggedObjectClass_DrawAll_Building_ArtFacings, 0x6)
{
	GET(const DWORD, pFogged, EBX);

	auto const pRecord = *reinterpret_cast<FoggedDrawRecord* const*>(pFogged + 0x60);

	if (pRecord && pRecord->Extra > 0)
	{
		if (auto const pType = abstract_cast<BuildingTypeClass*>(pRecord->Type))
		{
			if (auto const pImage = GetArtFacingImage(pType, pRecord->Extra - 1))
				R->EAX(pImage);
		}
	}

	return 0;
}

#pragma endregion
