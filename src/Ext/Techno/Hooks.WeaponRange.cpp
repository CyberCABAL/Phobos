#include <Ext/WeaponType/Body.h>
#include <Ext/Building/Body.h>

// Reimplements the game function with few changes / optimizations
DEFINE_HOOK(0x7012C2, TechnoClass_WeaponRange, 0x8)
{
	enum { ReturnResult = 0x70138F };

	GET(TechnoClass*, pThis, ECX);
	GET_STACK(const int, weaponIndex, STACK_OFFSET(0x8, 0x4));

	int result = 0;
	auto const pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;

	if (pWeapon)
	{
		result = WeaponTypeExt::GetRangeWithModifiers(pWeapon, pThis);
		auto const pTypeExt = TechnoExt::Fetch(pThis)->TypeExtData;

		if (!pTypeExt->OpenTopped_IgnoreRangefinding.Get(RulesExt::Global()->OpenTopped_IgnoreRangefinding) && pTypeExt->OwnerObject()->OpenTopped)
		{
			int smallestRange = INT32_MAX;
			auto pPassenger = pThis->Passengers.GetFirstPassenger();

			while (pPassenger)
			{
				const int openTWeaponIndex = pPassenger->GetTechnoType()->OpenTransportWeapon;
				const int tWeaponIndex = openTWeaponIndex == -1 ? pPassenger->SelectWeapon(pThis->Target) : openTWeaponIndex;
				auto const pTWeapon = pPassenger->GetWeapon(tWeaponIndex)->WeaponType;

				if (pTWeapon && pTWeapon->FireInTransport)
				{
					const int range = WeaponTypeExt::GetRangeWithModifiers(pTWeapon, pPassenger);

					if (range < smallestRange)
						smallestRange = range;
				}

				pPassenger = abstract_cast<FootClass*>(pPassenger->NextObject);
			}

			if (result > smallestRange)
				result = smallestRange;
		}
	}

	R->EBX(result);
	return ReturnResult;
}

static bool IsChasing(TechnoClass* pThis, AbstractClass* pTarget)
{
	if ((pThis->AbstractFlags & AbstractFlags::Foot) == AbstractFlags::None)
		return false;

	const auto pFootTarget = abstract_cast<FootClass*>(pTarget);

	if (!pFootTarget || !pFootTarget->Locomotor.GetInterfacePtr()->Is_Really_Moving_Now())
		return false;

	return true;
}

static bool IsMovingFire(TechnoClass* pThis)
{
	const auto pFoot = abstract_cast<FootClass*>(pThis);

	if (!pFoot || !pFoot->Locomotor.GetInterfacePtr()->Is_Really_Moving_Now())
		return false;

	return true;
}

static bool IsPrefiring(TechnoClass* pThis, WeaponTypeClass* pWeapon)
{
	const auto pTypeExt = WeaponTypeExt::Fetch(pWeapon);
	const int currentBurst = pThis->CurrentBurstIndex % pWeapon->Burst;

	if (pTypeExt->ExtraRange_Prefiring_IncludeBurst.Get(RulesExt::Global()->ExtraRange_Prefiring_IncludeBurst) && currentBurst != 0)
		return true;

	const auto pTechnoExt = TechnoExt::Fetch(pThis);

	if (pTechnoExt->DelayedFireTimer.InProgress())
		return true;

	switch (pThis->WhatAmI())
	{
	case AbstractType::Unit:
	{
		const auto pUnit = static_cast<UnitClass*>(pThis);
		int syncFrame = -1;

		if (currentBurst == 0)
			syncFrame = pUnit->Type->FiringSyncFrame0;
		else if (currentBurst == 1)
			syncFrame = pUnit->Type->FiringSyncFrame1;

		if (syncFrame == -1)
			return false;

		return pUnit->CurrentFiringFrame >= syncFrame;
	}
	case AbstractType::Aircraft:
	{
		const auto pAircraft = static_cast<AircraftClass*>(pThis);
		const auto status = (AirAttackStatus)pAircraft->MissionStatus;
		return status == AirAttackStatus::FireAtTarget
			|| status == AirAttackStatus::FireAtTarget2
			|| status == AirAttackStatus::FireAtTarget2_Strafe
			|| status == AirAttackStatus::FireAtTarget3_Strafe
			|| status == AirAttackStatus::FireAtTarget4_Strafe
			|| status == AirAttackStatus::FireAtTarget5_Strafe;
	}
	case AbstractType::Building:
	{
		const auto pBuilding = static_cast<BuildingClass*>(pThis);
		const auto pExt = BuildingExt::Fetch(pBuilding);
		return pBuilding->DelayBeforeFiring || pExt->IsFiringNow;
	}
	case AbstractType::Infantry:
	{
		const auto pInfantry = static_cast<InfantryClass*>(pThis);
		return pInfantry->IsFiring;
	}
	default:
		return false;
	}
}

DEFINE_HOOK(0x6F7248, TechnoClass_InRange_WeaponRange, 0x6)
{
	enum { SkipGameCode = 0x6F724E };

	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EBX);
	GET_BASE(AbstractClass*, pTarget, 0xC);

	int range = 0;

	if (const int keepRange = WeaponTypeExt::GetTechnoKeepRange(pWeapon, pThis, false))
	{
		range = keepRange;
	}
	else
	{
		range = WeaponTypeExt::GetRangeWithModifiers(pWeapon, pThis);

		if (range != -512)
		{
			const auto pExt = WeaponTypeExt::Fetch(pWeapon);
			const auto prefiringExtraRange = pExt->ExtraRange_Prefiring.Get(RulesExt::Global()->ExtraRange_Prefiring);

			if (prefiringExtraRange && IsPrefiring(pThis, pWeapon))
				range += prefiringExtraRange;

			const auto targetMovingExtraRange = pExt->ExtraRange_TargetMoving.isset()
				? pExt->ExtraRange_TargetMoving.Get() : (!RulesExt::Global()->ExtraRange_TargetMoving_CloseRangeOnly || pThis->GetTechnoType()->CloseRange
				? RulesExt::Global()->ExtraRange_TargetMoving : Leptons(0));

			if (targetMovingExtraRange && IsChasing(pThis, pTarget))
				range += targetMovingExtraRange;

			const auto firerMovingExtraRange = pExt->ExtraRange_FirerMoving.Get(RulesExt::Global()->ExtraRange_FirerMoving);

			if (firerMovingExtraRange && IsMovingFire(pThis))
				range += firerMovingExtraRange;
		}
	}

	R->EDI(range);

	return SkipGameCode;
}

DEFINE_HOOK(0x6F7294, TechnoClass_InRange_OccupyRange, 0x5)
{
	enum { SkipGameCode = 0x6F729F };

	GET(TechnoClass*, pThis, ESI);
	GET(const int, range, EDI);

	const int occupyRange = WeaponTypeExt::GetRangeWithModifiers(nullptr, pThis) / Unsorted::LeptonsPerCell;

	R->EDI(range + occupyRange);

	return SkipGameCode;
}

// InRange raises targets on the floor to the deck on bridge deck cells, but cells over water say they
// are not on the floor, so range to a deck cell over water was measured at the water.
DEFINE_HOOK(0x6F732F, TechnoClass_InRange_BridgeDeckCellTarget, 0x5)
{
	enum { OnFloor = 0x6F7336, NotOnFloor = 0x6F7375 };

	GET(AbstractClass* const, pTarget, ESI);

	if (pTarget->IsOnFloor())
		return OnFloor;

	const auto pCell = abstract_cast<CellClass*>(pTarget);
	return pCell && pCell->ContainsBridge() ? OnFloor : NotOnFloor;
}

#pragma region ElevationBonus

// Reimplements InRange's range bonus for SubjectToElevation=yes projectiles fired from higher ground.
// Vanilla counted a high bridge only on its middle lane. Objects now count it whenever they are on it,
// and cells on every deck lane, matching where InRange measures to.
namespace ElevationBonus
{
	static int GetLevel(AbstractClass* pAbstract)
	{
		if (const auto pCell = abstract_cast<CellClass*>(pAbstract))
			return pCell->GetLevel(); // Counts the bridge deck on every lane.

		const auto pCell = MapClass::Instance.GetCellAt(pAbstract->GetCoords());
		const auto pObject = abstract_cast<ObjectClass*>(pAbstract);

		// Anything that is not an object keeps the vanilla middle lane check.
		const bool onBridge = pObject ? pObject->OnBridge : (pCell->Flags & CellFlags::BridgeOwner) != CellFlags::Empty;

		return pCell->Level + (onBridge ? CellClass::BridgeLevels : 0);
	}

	// Returns the bonus in whole cells and outputs the level difference it was computed from.
	static int GetBonusCells(TechnoClass* pThis, AbstractClass* pTarget, int& levelDifference)
	{
		levelDifference = 0;

		// Cells count as on the floor even on water, so force-firing at open water gets the bonus a ship there would.
		const bool targetOnFloor = pTarget->WhatAmI() == AbstractType::Cell || pTarget->IsOnFloor();

		if (!pThis->IsOnFloor() || !targetOnFloor)
			return 0;

		const auto pRules = RulesClass::Instance;

		// Vanilla would divide by zero here.
		if (pRules->ElevationIncrement <= 0)
			return 0;

		levelDifference = std::max(GetLevel(pThis) - GetLevel(pTarget), 0);
		const double bonus = (levelDifference / pRules->ElevationIncrement) * pRules->ElevationIncrementBonus;

		return static_cast<int>(std::min(bonus, pRules->ElevationBonusCap));
	}

	// Non-arcing projectiles: InRange measures 3D distance, so the height difference is added too.
	static int __fastcall GetBonus(TechnoClass* pThis, void*, AbstractClass* pTarget)
	{
		int levelDifference = 0;
		const int bonus = GetBonusCells(pThis, pTarget, levelDifference) * Unsorted::LeptonsPerCell;
		const int height = levelDifference * Unsorted::LevelHeight;

		return static_cast<int>(Math::sqrt(static_cast<double>(bonus * bonus + height * height)));
	}

	// Arcing projectiles: InRange measures 2D distance.
	static int __fastcall GetBonusArcing(TechnoClass* pThis, void*, AbstractClass* pTarget)
	{
		int levelDifference = 0;
		return GetBonusCells(pThis, pTarget, levelDifference) * Unsorted::LeptonsPerCell;
	}
}

DEFINE_FUNCTION_JUMP(LJMP, 0x6F6F60, ElevationBonus::GetBonus)
DEFINE_FUNCTION_JUMP(LJMP, 0x6F70E0, ElevationBonus::GetBonusArcing)

#pragma endregion

DEFINE_HOOK(0x6FC3A1, TechnoClass_CanFire_InBunkerRangeCheck, 0x5)
{
	enum { ContinueChecks = 0x6FC3C5, CannotFire = 0x6FC86A };

	GET(TechnoClass*, pThis, EBP);
	GET(WeaponTypeClass*, pWeapon, EDI);

	if (pThis->WhatAmI() == AbstractType::Unit && WeaponTypeExt::GetRangeWithModifiers(pWeapon, pThis) < 384)
		return CannotFire;

	return ContinueChecks;
}

DEFINE_HOOK(0x70CF6F, TechnoClass_ThreatCoefficients_WeaponRange, 0x6)
{
	enum { SkipGameCode = 0x70CF75 };

	GET(TechnoClass*, pThis, EDI);
	GET(WeaponTypeClass*, pWeapon, EBX);

	R->EAX(WeaponTypeExt::GetRangeWithModifiers(pWeapon, pThis));

	return SkipGameCode;
}

DEFINE_HOOK(0x41810F, AircraftClass_MissionAttack_WeaponRangeCheck1, 0x6)
{
	enum { WithinDistance = 0x418117, NotWithinDistance = 0x418131 };

	GET(AircraftClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EDI);
	GET(const int, distance, EAX);

	const int range = WeaponTypeExt::GetRangeWithModifiers(pWeapon, pThis);

	if (distance < range)
		return WithinDistance;

	return NotWithinDistance;
}

DEFINE_HOOK(0x418BA8, AircraftClass_MissionAttack_WeaponRangeCheck2, 0x6)
{
	enum { SkipGameCode = 0x418BAE };

	GET(AircraftClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EAX);

	R->EAX(WeaponTypeExt::GetRangeWithModifiers(pWeapon, pThis));

	return SkipGameCode;
}

DEFINE_HOOK_AGAIN(0x6DC18E, TacticalClass_DrawRadialIndicators_WeaponRange, 0x6)
DEFINE_HOOK(0x6DBE63, TacticalClass_DrawRadialIndicators_WeaponRange, 0x6)
{
	enum { SkipGameCode1 = 0x6DBE6F, SkipGameCode2 = 0x6DC19A };

	GET(ObjectClass*, pObject, ESI);

	int* range = nullptr;
	int originalRange = 0;

	if (auto const pTechno = abstract_cast<TechnoClass*, true>(pObject))
	{
		auto const pWeapon = pTechno->GetPrimaryWeapon()->WeaponType;

		if (pWeapon)
		{
			range = &pWeapon->Range;
			originalRange = *range;
			pWeapon->Range = WeaponTypeExt::GetRangeWithModifiers(pWeapon, pTechno);
		}
	}

	pObject->DrawRadialIndicator(1);

	if (range)
		*range = originalRange;

	return R->Origin() == 0x6DBE63 ? SkipGameCode1 : SkipGameCode2;
}

namespace ApproachTargetTemp
{
	bool FromMaximumRange = true;
	int SearchRange = 0;
}

DEFINE_HOOK(0x4D5FBD, FootClass_ApproachTarget_BeforeSearching, 0xA)
{
	enum { WantAggressiveCrush = 0x4D6892, StartSearching = 0x4D5FE0 };

	GET(TechnoTypeClass*, pType, EAX);
	R->ESI(pType->MovementZone);

	GET_STACK(int, searchRange, STACK_OFFSET(0x158, -0x120));
	ApproachTargetTemp::FromMaximumRange = true;
	ApproachTargetTemp::SearchRange = searchRange;

	if (searchRange <= 204)
		return WantAggressiveCrush;

	GET(FootClass*, pThis, EBX);
	GET_STACK(const bool, inRange, STACK_OFFSET(0x158, -0x146));

	if (!inRange && !abstract_cast<FootClass*, true>(pThis->Target))
	{
		GET_STACK(const int, weaponIdx, STACK_OFFSET(0x158, -0xAC));
		const auto pWeapon = pThis->GetWeapon(weaponIdx)->WeaponType;

		if (pWeapon && pWeapon->Range != -512)
		{
			const int distance = pWeapon->Projectile->Arcing || pThis->WhatAmI() == AircraftClass::AbsID || pThis->IsInAir()
				? pThis->DistanceFrom(pThis->Target)
				: pThis->DistanceFrom3D(pThis->Target);

			if (distance < pWeapon->MinimumRange)
			{
				ApproachTargetTemp::FromMaximumRange = false;
				searchRange = Unsorted::LeptonsPerCell;
			}
		}
	}

	R->ECX(searchRange);
	R->Stack(STACK_OFFSET(0x158, -0xF4), searchRange);
	return StartSearching;
}

DEFINE_HOOK(0x4D6874, FootClass_ApproachTarget_NextRadius, 0xC)
{
	enum { ContinueNextRadius = 0x4D5FE0, BreakOut = 0x4D68E7 };

	REF_STACK(int, searchRadius, STACK_OFFSET(0x158, -0xF4));

	if (ApproachTargetTemp::FromMaximumRange)
	{
		searchRadius -= Unsorted::LeptonsPerCell;
		return searchRadius >= 204 ? ContinueNextRadius : BreakOut;
	}

	searchRadius += Unsorted::LeptonsPerCell;
	return searchRadius <= ApproachTargetTemp::SearchRange ? ContinueNextRadius : BreakOut;
}
