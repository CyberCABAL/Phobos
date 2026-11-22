#include <Helpers/Macro.h>
#include <TunnelLocomotionClass.h>

#include <Ext/Unit/Body.h>

namespace UnitUnloadTemp
{
	UnitTypeExt* TypeExtData = nullptr;
}

// Prevent subterranean units from deploying while underground.
DEFINE_HOOK(0x73D63B, UnitClass_Mi_Unload_Subterranean, 0x6)
{
	enum { ReturnFromFunction = 0x73DFB0, SkipHarvester = 0x73D694, SkipPassengers = 0x73DCD3, Harvester = 0x73DEE7, Continue = 0x73D6EC };

	GET(UnitClass* const, pThis, ESI);

	if (auto const pLoco = locomotion_cast<TunnelLocomotionClass*>(pThis->Locomotor))
	{
		if (pLoco->State != TunnelLocomotionClass::State::Idle)
			return ReturnFromFunction;
	}

	auto const pType = pThis->Type;
	auto const pTypeExt = UnitTypeExt::Fetch(pType);
	UnitUnloadTemp::TypeExtData = pTypeExt;

	// It should be the highest priority.
	if (pThis->BunkerLinkedItem)
	{
		if (auto const pBuilding = pThis->GetCell()->GetBuilding())
			pBuilding->EmptyBunker();

		// It can fix the issue where mining carts cannot move.
		R->EAX(pType);
		return SkipHarvester;
	}

	// Miners should not be hindered by other deployment actions while unloading minerals.
	if (pType->Harvester || pType->Weeder)
	{
		const bool hasAnyLink = pThis->HasAnyLink();

		if (hasAnyLink || pThis->Unloading)
		{
			R->AL(hasAnyLink);
			return Harvester;
		}
	}

	R->EAX(pType);

	if (pTypeExt->Deploy_SkipPassengerUnload)
		return SkipPassengers;
	else if (pTypeExt->Deploy_NoPassenger && pThis->Passengers.NumPassengers <= 0 && pThis->MissionStatus == 0)
		return SkipPassengers;

	return Continue;
}

DEFINE_HOOK(0x73DEEB, UnitClass_Mi_Unload_SkipHarvester, 0x5)
{
	GET(UnitClass* const, pThis, ESI);
	enum { SkipHarvester = 0x73D694 };

	auto const pTypeExt = UnitUnloadTemp::TypeExtData;

	if (!pThis->Unloading && (!pTypeExt->Deploy_NoTiberium || pThis->Tiberium.GetTotalValue() == 0))
	{
		R->EAX(pThis->Type);
		return SkipHarvester;
	}

	return 0;
}

// Before unloading, the transport turns its rear to the best free cell around it (0x740B60).
// Vanilla scores each cell with +128 if free or -128 if not, minus the angle between the cell and the rear.
// That angle was not wrapped around, so cells on one side could lose up to 255 instead of 128, and a free cell
// could score below zero and be treated as blocked. South cells also lost another 100 for no reason.
// Score the cells again with wrapped 16-bit angles and without the South penalty.
DEFINE_HOOK(0x740D10, UnitClass_GetUnloadFacing_ScoreCell, 0x5)
{
	enum { CompareScore = 0x740D3A };

	GET(UnitClass* const, pThis, EDI);
	GET(FootClass* const, pPassenger, EBX);
	GET(const int, baseScore, ECX); // 128 if the cell is free, -128 if not
	GET(const int, direction, EBP);

	DirStruct preferred;

	if (pPassenger) // Facing towards a passenger when loading, the game never uses this
	{
		GET_STACK(const int, preferred256, 0x24);
		preferred.Raw = static_cast<unsigned short>(preferred256 << 8);
	}
	else // The transport's rear
	{
		preferred.Raw = static_cast<unsigned short>(pThis->PrimaryFacing.Current().Raw + 0x8000);
	}

	const short angle = static_cast<short>((direction << 13) - preferred.Raw);
	const int offset = std::abs(static_cast<int>(angle)); // 0 to 0x8000

	// Free cells always score above 0 and blocked cells below it.
	// The score is doubled to leave room for a tie-break: a cell clockwise of the rear (on the transport's left)
	// wins over its mirror image, as that is also the side the passengers try first when they get out.
	int score = ((baseScore > 0 ? 0x10000 : -0x10000) - offset) * 2;

	if (angle > 0)
		++score;

	R->ECX(score);
	return CompareScore;
}

// If no cell around the transport is free, vanilla cancels the unload and goes back to guard.
// Ask allied units next to it to make room first, and try again a few times before giving up.
DEFINE_HOOK(0x73D800, UnitClass_Mission_Unload_NudgeWhenBlocked, 0x5)
{
	enum { StartUnload = 0x73D81C, CannotUnload = 0x73D87F, RetryLater = 0x73E289 };

	GET(UnitClass* const, pThis, ESI);
	GET_STACK(const CellStruct, cell, 0x20);

	if (cell != CellStruct::Empty)
		return StartUnload;

	constexpr int MaxNudges = 5;
	const auto pExt = UnitExt::Fetch(pThis);

	if (pExt->UnloadNudgeMissionStart != pThis->CurrentMissionStartTime)
	{
		pExt->UnloadNudgeMissionStart = pThis->CurrentMissionStartTime;
		pExt->UnloadNudgeCount = 0;
	}

	if (pExt->UnloadNudgeCount < MaxNudges && UnitExt::ScatterAlliesAroundForUnload(pThis, true))
	{
		++pExt->UnloadNudgeCount;
		return RetryLater; // MissionStatus stays 0, so the next update looks for a cell again
	}

	return CannotUnload;
}

DEFINE_HOOK(0x740015, UnitClass_MouseOverObject_SkipPassengers, 0x6)
{
	enum { SkipPassengers = 0x7400F0 };

	GET(UnitClass* const, pThis, ESI);
	GET(UnitTypeClass* const, pType, EAX);

	auto const pTypeExt = UnitTypeExt::Fetch(pType);

	return pTypeExt->Deploy_SkipPassengerUnload
		|| (pTypeExt->Deploy_NoPassenger && pThis->Passengers.NumPassengers <= 0)
		? SkipPassengers : 0;
}
