#include "Body.h"

#include <Ext/Techno/Body.h>

WaveExt::ExtContainer WaveExt::ExtMap;

void WaveExt::Initialize() { }

// =============================
// load / save

template <typename T>
void WaveExt::Serialize(T& Stm)
{
	Stm
		.Process(this->Weapon)
		;
}

void WaveExt::LoadFromStream(PhobosStreamReader& Stm)
{
	ObjectExt::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void WaveExt::SaveToStream(PhobosStreamWriter& Stm)
{
	ObjectExt::SaveToStream(Stm);
	this->Serialize(Stm);
}

bool WaveExt::LoadGlobals(PhobosStreamReader& Stm)
{
	return Stm
		.Success();
}

bool WaveExt::SaveGlobals(PhobosStreamWriter& Stm)
{
	return Stm
		.Success();
}

// =============================
// container

WaveExt::ExtContainer::ExtContainer() : Container("WaveClass") { }
WaveExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

// Allocated before the constructor first calculates the wave's shape, which already needs the extension.
// At this point the vtables, Owner and Type are set.
DEFINE_HOOK(0x75EA59, WaveClass_CTOR, 0x5)
{
	GET(WaveClass*, pItem, ESI);

	if (const auto pExt = WaveExt::ExtMap.TryAllocate(pItem))
	{
		// Waves are created from TechnoClass::Fire (sonic waves by the game, laser waves by Ares), after
		// Phobos has recorded the weapon being fired as the owner's LastWeaponType.
		if (const auto pOwnerExt = TechnoExt::TryFetch(pItem->Owner))
			pExt->Weapon = pOwnerExt->LastWeaponType;
	}

	return 0;
}

// Placed after Ares's WaveClass destructor hook at 0x763226.
DEFINE_HOOK(0x76326F, WaveClass_DTOR, 0x6)
{
	GET(WaveClass*, pItem, EDI);

	WaveExt::ExtMap.Remove(pItem);

	return 0;
}
