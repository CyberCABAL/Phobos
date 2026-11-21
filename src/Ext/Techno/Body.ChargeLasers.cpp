#include "Body.h"

#include <BuildingClass.h>
#include <TacticalClass.h>

#include <Ext/Rules/Body.h>
#include <Ext/WeaponType/Body.h>

// ChargeLaser: purely cosmetic lasers drawn while a weapon charges up before it fires, looking like lasers fired by
// an IsLaser weapon (ChargeLaser.Weapon) that is never fired itself. A beam is held at full brightness for as long as
// it should be shown. Once it should not (the shot went off, the beam's End frame came, or the charge was interrupted)
// it is let go and the game fades it out over the laser weapon's LaserDuration, as with any fired laser. Held and fading
// beams are both kept on the object.
//
// Lasers are deleted by the game on its own:
// - LaserDrawClass::Update (0x550150, once per frame from LogicClass::Update) adds 1 to Progress.Value and deletes the
//   laser once that reaches Duration. Exactly one such update happens between two frames' object updates.
// - 0x550000 deletes all lasers, but only together with all objects (from 0x534450), so no object is left holding one.
// So a laser can be touched safely as long as this runs every frame: a held one is set back to 0 each time, and a
// fading one seen at value v is still there on the next frame only if v + 1 < Duration.

namespace
{
	// Held beams are set back to 0 every frame, so with a duration of at least 2 a held beam is always still there
	// on the next frame. Shorter LaserDuration values are raised to this.
	constexpr int ChargeLaserMinimumDuration = 2;

	// Whether the charging object could still fire, so its beams may stay lit.
	bool CanShowChargeLasers(TechnoClass* pThis)
	{
		if (!pThis->IsAlive || pThis->InLimbo || !pThis->Target)
			return false;

		// EMP and powerless units (Ares) or deactivated objects cannot fire.
		if (pThis->Deactivated || pThis->IsUnderEMP())
			return false;

		// Being erased by a temporal weapon. Buildings are not updated at all meanwhile (BuildingClass::AI skips
		// to 0x4403D4), which also pauses the game's charge countdown; the beams go out and come back if it resumes.
		if (pThis->TemporalTargetingMe || pThis->IsBeingWarpedOut())
			return false;

		// Do not give a cloaked object away.
		if (pThis->CloakState == CloakState::Cloaked)
			return false;

		if (auto const pBuilding = abstract_cast<BuildingClass*, true>(pThis))
		{
			// The same checks BuildingClass::GetFireError (0x447F10) makes: being sold, and power offline. The game asks
			// IsPowerOnline of every building, not only Powered=yes ones, so whatever switches a building off counts.
			if (pBuilding->GetCurrentMission() == Mission::Selling || !pBuilding->IsPowerOnline())
				return false;
		}

		return true;
	}

	CoordStruct GetChargeLaserCoords(TechnoClass* pThis, int weaponIndex, const CoordStruct& offset, ChargeLaserRelativeTo relativeTo)
	{
		switch (relativeTo)
		{
		case ChargeLaserRelativeTo::Turret:
			return TechnoExt::GetFLHAbsoluteCoords(pThis, offset, pThis->HasTurret());

		case ChargeLaserRelativeTo::Fixed:
			// What GetFLHAbsoluteCoords gives for an object that does not turn (as for buildings).
			return pThis->GetRenderCoords() + CoordStruct { offset.X, -offset.Y, offset.Z };

		case ChargeLaserRelativeTo::Weapon:
			// TechnoClass::GetFLH adds the offset to the weapon's own FLH (burst FLHs, turret and all) before turning it.
			return pThis->GetFLH(weaponIndex, offset);

		default:
			return TechnoExt::GetFLHAbsoluteCoords(pThis, offset, false);
		}
	}

	// Both ends of a beam where the object is now.
	std::pair<CoordStruct, CoordStruct> GetChargeLaserEnds(TechnoClass* pThis, const TechnoExt::ChargeLaserBeam& beam)
	{
		const bool usesWeapon = beam.SourceRelativeTo == ChargeLaserRelativeTo::Weapon || beam.TargetRelativeTo == ChargeLaserRelativeTo::Weapon;
		const int burstIndex = pThis->CurrentBurstIndex;

		// The firer moves on to its next burst (and its burst FLH) after the shot; keep using the one the beam charged for.
		if (usesWeapon)
			pThis->CurrentBurstIndex = beam.BurstIndex;

		auto ends = std::make_pair(GetChargeLaserCoords(pThis, beam.WeaponIndex, beam.Source, beam.SourceRelativeTo),
			GetChargeLaserCoords(pThis, beam.WeaponIndex, beam.Target, beam.TargetRelativeTo));

		if (usesWeapon)
			pThis->CurrentBurstIndex = burstIndex;

		return ends;
	}

	void PlaceChargeLaser(TechnoClass* pThis, const TechnoExt::ChargeLaserBeam& beam)
	{
		const auto [source, target] = GetChargeLaserEnds(pThis, beam);
		beam.Laser->Source = source;
		beam.Laser->Target = target;
	}

	// Mirrors BulletExt::SimulatedFiringLaser, so a beam looks like a laser fired by the laser weapon.
	LaserDrawClass* CreateChargeLaser(TechnoClass* pThis, WeaponTypeClass* pLaserWeapon, const CoordStruct& source, const CoordStruct& target)
	{
		auto const pLaserWeaponExt = WeaponTypeExt::Fetch(pLaserWeapon);
		int zAdjust = pLaserWeaponExt->LaserZAdjust.Get(RulesExt::Global()->LaserZAdjust);

		// Lasers are depth tested per pixel (0x550260): each end's depth is corrected for its own height, and zAdjust is
		// added to the source end only, fading out toward the target end. TechnoClass::CreateLaser (0x6FD210) brings the
		// source of a building's laser forward by how far it is above the building on screen, so the building's own image
		// does not hide where the laser starts. Do exactly the same, so the Source end of a beam is treated like the firing
		// point of a laser fired by the building, and the Target end keeps its natural depth.
		if (pThis->WhatAmI() == AbstractType::Building)
		{
			const int baseY = TacticalClass::Instance->CoordsToClient(pThis->GetRenderCoords()).first.Y;
			const int sourceY = TacticalClass::Instance->CoordsToClient(source).first.Y;

			zAdjust += Math::min(sourceY - baseY, 0);
		}

		const int duration = Math::max(pLaserWeapon->LaserDuration, ChargeLaserMinimumDuration);
		const ColorStruct black { 0, 0, 0 };
		LaserDrawClass* pLaser = nullptr;

		if (pLaserWeapon->IsHouseColor || pLaserWeaponExt->Laser_IsSingleColor)
		{
			auto const& color = (pLaserWeapon->IsHouseColor && pThis->Owner) ? pThis->Owner->LaserColor : pLaserWeapon->LaserInnerColor;
			pLaser = GameCreate<LaserDrawClass>(source, target, zAdjust, (BYTE)1, color, black, black, duration);

			pLaser->IsHouseColor = true;
			pLaser->Thickness = pLaserWeaponExt->LaserThickness;
			pLaser->IsSupported = (pLaser->Thickness > 3);
		}
		else
		{
			pLaser = GameCreate<LaserDrawClass>(source, target, zAdjust, (BYTE)1,
				pLaserWeapon->LaserInnerColor, pLaserWeapon->LaserOuterColor, pLaserWeapon->LaserOuterSpread, duration);

			pLaser->IsHouseColor = false;
			pLaser->Thickness = 3;
			pLaser->IsSupported = false;
		}

		return pLaser;
	}

	// A beam's own value if set, otherwise the weapon's default, otherwise the given fallback.
	template <typename T>
	T PickChargeLaserValue(const Nullable<T>& own, const Nullable<T>& defaults, const T& fallback)
	{
		return own.isset() ? own.Get() : defaults.Get(fallback);
	}
}

// The weapon that is charging up right now, if any, with its index and the frames since the charge began.
WeaponTypeClass* TechnoExt::GetChargingWeapon(int& weaponIndex, int& elapsed) const
{
	auto const pThis = this->OwnerObject();
	weaponIndex = -1;
	elapsed = 0;

	// IsAnimDelayedFire buildings and prism towers: BuildingClass::Mission_Attack (0x44B630) stores the weapon index in
	// PrismTargetCoords.X, and DelayBeforeFiring counts down from DelayedFireDelay to the shot (0x4503F0). Ares replaces
	// both for prism towers but keeps them working the same way for the tower that fires.
	if (auto const pBuilding = abstract_cast<BuildingClass*, true>(pThis))
	{
		if (pBuilding->PrismStage == PrismChargeState::Master)
		{
			weaponIndex = pBuilding->PrismTargetCoords.X;
			elapsed = pBuilding->Type->DelayedFireDelay - static_cast<int>(pBuilding->DelayBeforeFiring);
		}
	}

	// DelayedFire.Duration on the weapon.
	if (weaponIndex < 0 && this->DelayedFireWeaponIndex >= 0 && this->DelayedFireTimer.InProgress())
	{
		weaponIndex = this->DelayedFireWeaponIndex;
		elapsed = Unsorted::CurrentFrame - this->DelayedFireTimer.StartTime;
	}

	if (weaponIndex < 0)
		return nullptr;

	elapsed = Math::max(elapsed, 0);
	auto const pWeapon = pThis->GetWeapon(weaponIndex);

	return pWeapon ? pWeapon->WeaponType : nullptr;
}

void TechnoExt::UpdateChargeLasers()
{
	auto const pThis = this->OwnerObject();
	const int frame = Unsorted::CurrentFrame;

	// If a frame was missed (the object was not updated, e.g. while being warped), the game may have deleted any of
	// the beams since, so they are all forgotten without being touched.
	if (this->ChargeLasers_Frame != frame && this->ChargeLasers_Frame != frame - 1)
	{
		this->ChargeLasers.clear();
		this->ChargeLasers_Fading.clear();
	}

	this->ChargeLasers_Frame = frame;

	auto letGo = [this](ChargeLaserBeam& beam)
	{
		if (beam.Laser)
		{
			this->ChargeLasers_Fading.push_back(beam);
			beam.Laser = nullptr;
		}
	};

	int weaponIndex = -1;
	int elapsed = 0;
	auto const pWeapon = this->GetChargingWeapon(weaponIndex, elapsed);

	// The charge ended, another weapon is charging, or the owner changed (house colour): let go of all held beams.
	if (pWeapon != this->ChargeLasers_Weapon || pThis->Owner != this->ChargeLasers_Owner)
	{
		for (auto& beam : this->ChargeLasers)
			letGo(beam);

		this->ChargeLasers.clear();
		this->ChargeLasers_Weapon = pWeapon;
		this->ChargeLasers_Owner = pThis->Owner;
	}

	auto const pWeaponExt = pWeapon ? WeaponTypeExt::Fetch(pWeapon) : nullptr;

	if (pWeaponExt && !pWeaponExt->ChargeLasers.empty())
	{
		auto const& defaults = pWeaponExt->ChargeLaser;
		auto const& beams = pWeaponExt->ChargeLasers;
		const bool canShow = CanShowChargeLasers(pThis);

		this->ChargeLasers.resize(beams.size());

		for (size_t idx = 0; idx < beams.size(); ++idx)
		{
			auto const& entry = beams[idx];
			auto& beam = this->ChargeLasers[idx];

			auto const pLaserWeapon = PickChargeLaserValue<WeaponTypeClass*>(entry.Weapon, defaults.Weapon, nullptr);
			const int delay = PickChargeLaserValue(entry.Delay, defaults.Delay, 0);
			const int end = PickChargeLaserValue(entry.End, defaults.End, -1); // -1: until the charge ends

			if (!canShow || !pLaserWeapon || !pLaserWeapon->IsLaser || elapsed < delay || (end >= 0 && elapsed >= end))
			{
				letGo(beam);
				continue;
			}

			beam.Source = PickChargeLaserValue(entry.Source, defaults.Source, CoordStruct::Empty);
			beam.SourceRelativeTo = PickChargeLaserValue(entry.Source_RelativeTo, defaults.Source_RelativeTo, ChargeLaserRelativeTo::Body);
			beam.Target = PickChargeLaserValue(entry.Target, defaults.Target, CoordStruct::Empty);
			beam.TargetRelativeTo = PickChargeLaserValue(entry.Target_RelativeTo, defaults.Target_RelativeTo, ChargeLaserRelativeTo::Body);
			beam.WeaponIndex = weaponIndex;
			beam.BurstIndex = pThis->CurrentBurstIndex;

			if (!beam.Laser)
			{
				const auto [source, target] = GetChargeLaserEnds(pThis, beam);
				beam.Laser = CreateChargeLaser(pThis, pLaserWeapon, source, target);
			}
			else
			{
				PlaceChargeLaser(pThis, beam);
			}

			// Hold at full brightness.
			beam.Laser->Progress.Value = 0;
		}
	}

	// Keep fading beams on the object until the game deletes them. Once the object is inside a transport or the like,
	// there is nothing sensible to follow, so they are left where they are.
	if (pThis->InLimbo)
	{
		this->ChargeLasers_Fading.clear();
		return;
	}

	std::erase_if(this->ChargeLasers_Fading, [pThis](const ChargeLaserBeam& beam)
	{
		PlaceChargeLaser(pThis, beam);

		// Deleted by the game on its next update: forget it now.
		return beam.Laser->Progress.Value + 1 >= beam.Laser->Duration;
	});
}
