#include "ToggleSmoothGroundLighting.h"

#include <HouseClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <RulesClass.h>

#include <Utilities/GeneralUtils.h>

const char* ToggleSmoothGroundLightingCommandClass::GetName() const
{
	return "Toggle Smooth Ground Lighting";
}

const wchar_t* ToggleSmoothGroundLightingCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_SMOOTH_GROUND_LIGHTING", L"Toggle Smooth Ground Lighting");
}

const wchar_t* ToggleSmoothGroundLightingCommandClass::GetUICategory() const
{
	return CATEGORY_INTERFACE;
}

const wchar_t* ToggleSmoothGroundLightingCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_SMOOTH_GROUND_LIGHTING_DESC", L"Blend ground lighting smoothly between cells, or use the original blocky lighting.");
}

void ToggleSmoothGroundLightingCommandClass::Execute(WWKey eInput) const
{
	Phobos::Config::SmoothGroundLighting = !Phobos::Config::SmoothGroundLighting;

	// Repaint every tile so the change shows at once.
	MapClass::Instance.MarkNeedsRedraw(2);

	const wchar_t* message = Phobos::Config::SmoothGroundLighting
		? GeneralUtils::LoadStringUnlessMissing("MSG:SmoothGroundLightingOn", L"Smooth ground lighting: on")
		: GeneralUtils::LoadStringUnlessMissing("MSG:SmoothGroundLightingOff", L"Smooth ground lighting: off");

	if (HouseClass::CurrentPlayer)
		MessageListClass::Instance.PrintMessage(message, RulesClass::Instance->MessageDelay, HouseClass::CurrentPlayer->ColorSchemeIndex, true);
}
