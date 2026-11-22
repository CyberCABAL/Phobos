#include <CellSpread.h>
#include <Helpers/Macro.h>
#include <TunnelLocomotionClass.h>

#include <Ext/Rules/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Unit/Body.h>

namespace UnitUnloadTemp
{
	UnitTypeExt* TypeExtData = nullptr;
}

static bool TurnsToUnload(UnitClass* pTransport)
{
	return TechnoTypeExt::Fetch(pTransport->Type)->TurnToUnload.Get(RulesExt::Global()->TurnToUnload);
}

// Checks a cell next to the transport the same way the passenger does when it gets out (0x73D8CB).
// 2 if the passenger can also move on to the cell behind it, 1 if it can only reach this cell, 0 if it cannot get out there.
static int GetUnloadCellRating(UnitClass* pTransport, FootClass* pPassenger, int direction)
{
	const auto& offset = CellSpread::GetNeighbourOffset(static_cast<size_t>(direction));
	const auto cell = pTransport->GetMapCoords() + offset;
	const auto pCell = MapClass::Instance.GetCellAt(cell);

	if (pCell->ContainsBridge()) // Passengers never get out onto bridge cells
		return 0;

	const auto facing = static_cast<FacingType>(direction);
	const int level = pTransport->GetCellLevel();

	if (pPassenger->IsCellOccupied(pCell, facing, level, nullptr, true) != Move::OK)
		return 0;

	const auto pNextCell = MapClass::Instance.GetCellAt(cell + offset);
	return pPassenger->IsCellOccupied(pNextCell, facing, level, nullptr, true) == Move::OK ? 2 : 1;
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
// Vanilla's free check also missed walls, terrain objects and the like, so free cells are checked again
// the way the next passenger checks them when it gets out.
DEFINE_HOOK(0x740D10, UnitClass_GetUnloadFacing_ScoreCell, 0x5)
{
	enum { CompareScore = 0x740D3A };

	GET(UnitClass* const, pThis, EDI);
	GET(FootClass* const, pEntering, EBX);
	GET(const int, baseScore, ECX); // 128 if the cell is free, -128 if not
	GET(const int, direction, EBP);

	DirStruct preferred;

	if (pEntering) // Facing towards a passenger when loading, the game never uses this
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

	int rating = baseScore > 0 ? 1 : 0;

	if (rating && !pEntering)
	{
		if (const auto pExiting = pThis->Passengers.GetFirstPassenger())
			rating = GetUnloadCellRating(pThis, pExiting, direction);
	}

	// Cells the passenger can move on from score above cells it can only step into, and both score above 0.
	// Blocked cells score below 0. The score is doubled to leave room for a tie-break: a cell clockwise of the rear (on the transport's left)
	// wins over its mirror image, as that is also the side the passengers try first when they get out.
	int score = ((rating ? rating * 0x10000 : -0x10000) - offset) * 2;

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

// TurnToUnload=no: do not turn before unloading, passengers get out through the free cells around the transport.
DEFINE_HOOK(0x73D85E, UnitClass_Mission_Unload_TurnToUnload, 0x6)
{
	enum { SkipTurn = 0x73D86F };

	GET(UnitClass* const, pThis, ESI);

	return TurnsToUnload(pThis) ? 0 : SkipTurn;
}

// Vanilla waits here for a non-spinning turret but not the body, so passengers got out wherever the rear
// pointed mid-turn. Wait for the body instead, as the turret does not affect where passengers get out.
DEFINE_HOOK(0x73D892, UnitClass_Mission_Unload_WaitForTurn, 0x6)
{
	enum { StartUnloading = 0x73D8A0, CheckNextFrame = 0x73D8AA };

	GET(UnitClass* const, pThis, ESI);

	if (TurnsToUnload(pThis) && pThis->PrimaryFacing.IsRotating())
		return CheckNextFrame; // MissionStatus stays 1, so the next update checks again

	return StartUnloading;
}

// Passengers first look for a cell they can get out to and walk on from, starting at the rear, then go around again
// for a cell they can only step into. Vanilla started that second round one direction too far and skipped the rear.
DEFINE_HOOK(0x73DA21, UnitClass_Mission_Unload_SecondRoundFromRear, 0x6)
{
	enum { NextDirection = 0x73D9FA };

	R->Stack<bool>(0x12, false); // First round done
	R->EAX(-1); // Incremented to 0, the rear
	return NextDirection;
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
