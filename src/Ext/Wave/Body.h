#pragma once

// WaveClass.h relies on ObjectClass and CellClass being declared before it is included.
#include <Ext/Object/Body.h>
#include <CellClass.h>
#include <WaveClass.h>

class WeaponTypeClass;

class WaveExt final : public ObjectExt
{
public:
	using base_type = WaveClass;

	static constexpr DWORD Canary = 0x5A7E5A7E;

public:
	// typed owner accessor
	WaveClass* OwnerObject() const
	{
		return static_cast<WaveClass*>(this->GetAttachedObject());
	}

	// The weapon that fired this wave, if known. Set when the wave is created.
	WeaponTypeClass* Weapon;

	WaveExt(WaveClass* OwnerObject) : ObjectExt(OwnerObject)
		, Weapon { nullptr }
	{ }

	virtual ~WaveExt() = default;
	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;
	virtual void Initialize() override;

private:
	template <typename T>
	void Serialize(T& Stm);

public:
	class ExtContainer final : public Container<WaveExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static WaveExt* Fetch(const WaveClass* pThis)
	{
		return AbstractExt::Fetch<WaveExt>(pThis);
	}

	static WaveExt* TryFetch(const WaveClass* pThis)
	{
		return AbstractExt::TryFetch<WaveExt>(pThis);
	}

	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);
};
