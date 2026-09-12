/*This file is part of FFB Arcade Plugin.
FFB Arcade Plugin is free software : you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
FFB Arcade Plugin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with FFB Arcade Plugin.If not, see < https://www.gnu.org/licenses/>.
*/

#include <string>
#include <math.h>
#include "Outrun2Fake.h"
#include "SDL.h"
#include <Windows.h>

// OutRun 2 Modern FFB
//
// GameId 12 keeps the existing "Outrun 2 Special Tours Deluxe Custom"
// integration and memory map, but replaces the old stepped centering force
// with a continuous speed-sensitive SAT-like force plus steering-velocity
// damping. Existing road/collision memory events are retained.
//
// All Modern* values are optional. If they are absent from FFBPlugin.ini the
// defaults below are used, so existing installations continue to work.

static EffectTriggers *myTriggers;
static EffectConstants *myConstants;
static Helpers *myHelpers;
extern SDL_Event e;
extern int EnableDamper;
extern int DamperStrength;
static bool init = false;
static wchar_t *settingsFilename = TEXT(".\\FFBPlugin.ini");

static int ShowButtonNumbersForSetup = GetPrivateProfileInt(TEXT("Settings"), TEXT("ShowButtonNumbersForSetup"), 0, settingsFilename);
static int ChangeGearsViaPlugin = GetPrivateProfileInt(TEXT("Settings"), TEXT("ChangeGearsViaPlugin"), 0, settingsFilename);
static int Gear1 = GetPrivateProfileInt(TEXT("Settings"), TEXT("Gear1"), 0, settingsFilename);
static int Gear2 = GetPrivateProfileInt(TEXT("Settings"), TEXT("Gear2"), 0, settingsFilename);
static int Gear3 = GetPrivateProfileInt(TEXT("Settings"), TEXT("Gear3"), 0, settingsFilename);
static int Gear4 = GetPrivateProfileInt(TEXT("Settings"), TEXT("Gear4"), 0, settingsFilename);
static int Gear5 = GetPrivateProfileInt(TEXT("Settings"), TEXT("Gear5"), 0, settingsFilename);
static int Gear6 = GetPrivateProfileInt(TEXT("Settings"), TEXT("Gear6"), 0, settingsFilename);

// Modern FFB tuning. Percent values are intentionally conservative enough for
// belt/gear wheels but are aimed primarily at modern DD wheels such as MOZA R3.
static int ModernFFBEnable = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernFFBEnable"), 1, settingsFilename);
static int ModernSATStrength = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernSATStrength"), 72, settingsFilename);
static int ModernSATMinStrength = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernSATMinStrength"), 8, settingsFilename);
static int ModernSATSteeringExponent = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernSATSteeringExponent"), 85, settingsFilename); // /100
static int ModernSATSpeedExponent = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernSATSpeedExponent"), 75, settingsFilename);       // /100
static int ModernSpeedReference = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernSpeedReference"), 320, settingsFilename);
static int ModernDynamicDamping = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernDynamicDamping"), 22, settingsFilename);
static int ModernDampingVelocityReference = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernDampingVelocityReference"), 600, settingsFilename); // /100 full-scale/s
static int ModernTorqueResponse = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernTorqueResponse"), 42, settingsFilename);
static int ModernCenterDeadzone = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernCenterDeadzone"), 2, settingsFilename);
static int ModernCollisionStrength = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernCollisionStrength"), 65, settingsFilename);
static int ModernRoadStrength = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernRoadStrength"), 55, settingsFilename);
static int ModernGearKickStrength = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernGearKickStrength"), 12, settingsFilename);
static int ModernDebugTelemetry = GetPrivateProfileInt(TEXT("Settings"), TEXT("ModernDebugTelemetry"), 0, settingsFilename);

static double ClampDouble(double value, double minValue, double maxValue)
{
	if (value < minValue)
		return minValue;
	if (value > maxValue)
		return maxValue;
	return value;
}

static double SignDouble(double value)
{
	if (value > 0.0)
		return 1.0;
	if (value < 0.0)
		return -1.0;
	return 0.0;
}

static void SendSignedConstant(double signedTorque)
{
	signedTorque = ClampDouble(signedTorque, -1.0, 1.0);
	double strength = fabs(signedTorque);

	// Positive signed torque means force toward the right. EffectConstants names
	// describe where the force comes FROM, not where the wheel moves TO.
	if (signedTorque > 0.0)
		myTriggers->Constant(myConstants->DIRECTION_FROM_LEFT, strength);
	else if (signedTorque < 0.0)
		myTriggers->Constant(myConstants->DIRECTION_FROM_RIGHT, strength);
	else
		myTriggers->Constant(myConstants->DIRECTION_FROM_LEFT, 0.0);
}

static double ApplySteeringDeadzone(double steering)
{
	double deadzone = ClampDouble(ModernCenterDeadzone / 100.0, 0.0, 0.20);
	double magnitude = fabs(steering);

	if (magnitude <= deadzone)
		return 0.0;

	// Remap the remaining travel so reaching full lock still produces 1.0.
	magnitude = (magnitude - deadzone) / (1.0 - deadzone);
	return SignDouble(steering) * ClampDouble(magnitude, 0.0, 1.0);
}

static double CalculateSpeedScale(float speed)
{
	double speedReference = ModernSpeedReference > 1 ? (double)ModernSpeedReference : 320.0;
	double normalized = ClampDouble(speed / speedReference, 0.0, 1.0);
	double exponent = ClampDouble(ModernSATSpeedExponent / 100.0, 0.20, 2.50);
	return pow(normalized, exponent);
}

static void LogModernTelemetry(float speed, double steering, double velocity, double baseTorque, double collisionTorque, double finalTorque,
	int ff, int ff3, int ff4, int ff5, int ffwall, float ff7)
{
	if (!ModernDebugTelemetry)
		return;

	static DWORD lastLogTick = 0;
	DWORD now = GetTickCount();
	if ((DWORD)(now - lastLogTick) < 250)
		return;
	lastLogTick = now;

	char buff[512];
	sprintf_s(buff,
		"OR2Modern speed=%.2f steer=%.3f vel=%.3f base=%.3f impact=%.3f out=%.3f ff=%d ff3=%d ff4=%d ff5=%d wall=%d ff7=%.4f",
		speed, steering, velocity, baseTorque, collisionTorque, finalTorque, ff, ff3, ff4, ff5, ffwall, ff7);
	myHelpers->log(buff);
}

static int ThreadLoop()
{
	// Existing OutRun2 Custom memory map.
	int ff = myHelpers->ReadInt32(0x0827A1A0, false);
	int ffwall = myHelpers->ReadInt32(0x08273FAC, false); // retained for telemetry/research
	int ff3 = myHelpers->ReadInt32(0x0827A1DA, false);
	int ff4 = myHelpers->ReadInt32(0x0827A35D, false);
	int ff5 = myHelpers->ReadInt32(0x0827A1D4, false);
	UINT8 rawSteering = myHelpers->ReadByte(0x08670DC8, false);
	float ff7 = myHelpers->ReadFloat32(0x08273AD4, false); // retained for telemetry/research
	UINT8 raceActive = myHelpers->ReadByte(0x08304ADC, false); // 1 while racing
	UINT8 menuActive = myHelpers->ReadByte(0x086749CA, false); // 1 in menu
	UINT8 gear = myHelpers->ReadByte(0x0827A160, false);
	float speed = myHelpers->ReadFloat32(0x08273DF0, false);

	static bool stateInitialized = false;
	static int oldFf3 = 0;
	static int oldFf4 = 0;
	static UINT8 oldGear = 0;
	static double previousSteering = 0.0;
	static double filteredVelocity = 0.0;
	static double filteredBaseTorque = 0.0;
	static DWORD previousTick = 0;

	// The original implementation also allowed a device-native damper effect.
	// Keep it as an optional extra layer. Modern dynamic damping works even when
	// EnableDamper=0 because it is mixed into the signed constant torque.
	if (EnableDamper == 1)
		myTriggers->Damper(ClampDouble(DamperStrength / 100.0, 0.0, 1.0));

	// rawSteering is centered around 0x7F. Positive normalized values are right.
	double steering = (rawSteering <= 127)
		? ((double)rawSteering - 127.0) / 127.0
		: ((double)rawSteering - 127.0) / 128.0;
	steering = ClampDouble(steering, -1.0, 1.0);
	steering = ApplySteeringDeadzone(steering);

	DWORD now = GetTickCount();
	if (!stateInitialized)
	{
		oldFf3 = ff3;
		oldFf4 = ff4;
		oldGear = gear;
		previousSteering = steering;
		previousTick = now;
		stateInitialized = true;
	}

	double dt = (DWORD)(now - previousTick) / 1000.0;
	if (dt < 0.004 || dt > 0.100)
		dt = 0.016;
	previousTick = now;

	// Steering velocity is useful on DD wheels because pure spring/SAT forces can
	// oscillate around center. Low-pass it before calculating dynamic damping.
	double rawVelocity = (steering - previousSteering) / dt;
	previousSteering = steering;
	filteredVelocity += (rawVelocity - filteredVelocity) * 0.35;

	double speedScale = CalculateSpeedScale(speed);
	double satMax = ClampDouble(ModernSATStrength / 100.0, 0.0, 1.0);
	double satMin = ClampDouble(ModernSATMinStrength / 100.0, 0.0, satMax);
	double satGain = satMin + (satMax - satMin) * speedScale;
	double steeringExponent = ClampDouble(ModernSATSteeringExponent / 100.0, 0.30, 2.50);
	double steeringShape = pow(fabs(steering), steeringExponent) * SignDouble(steering);

	// SAT-like force always opposes steering displacement and therefore pulls the
	// wheel back toward center. This is synthetic SAT, not a tire slip-angle value.
	double satTorque = -steeringShape * satGain;

	double dampingReference = ModernDampingVelocityReference / 100.0;
	if (dampingReference < 0.50)
		dampingReference = 6.0;
	double dampingStrength = ClampDouble(ModernDynamicDamping / 100.0, 0.0, 1.0);
	double dampingTorque = -ClampDouble(filteredVelocity / dampingReference, -1.0, 1.0) * dampingStrength;

	double targetBaseTorque = satTorque + dampingTorque;
	targetBaseTorque = ClampDouble(targetBaseTorque, -1.0, 1.0);

	// Smooth only the continuous steering layer. Collision impulses are mixed
	// afterwards so they keep their sharp arcade impact.
	double response = ClampDouble(ModernTorqueResponse / 100.0, 0.05, 1.0);
	filteredBaseTorque += (targetBaseTorque - filteredBaseTorque) * response;

	if (!ModernFFBEnable)
	{
		// Compatibility mode approximates the old Custom centering behavior.
		filteredBaseTorque = -steering;
	}

	double collisionTorque = 0.0;
	double collisionStrength = ClampDouble(ModernCollisionStrength / 100.0, 0.0, 1.0);
	double collisionSpeed = 0.30 + 0.70 * speedScale;

	// Preserve the directional Custom effect events, but mix them with the base
	// steering torque instead of replacing it. ff5 indicates impact side and ff3
	// changes when an impact/event occurs.
	if (oldFf3 != ff3)
	{
		if (ff5 == 2)
			collisionTorque -= collisionStrength * collisionSpeed; // force left
		else if (ff5 == 1)
			collisionTorque += collisionStrength * collisionSpeed; // force right
	}

	double roadScale = ClampDouble(ModernRoadStrength / 100.0, 0.0, 1.0);
	if (oldFf4 != ff4)
	{
		double effectStrength = ClampDouble((0.20 + 0.45 * speedScale) * roadScale, 0.0, 1.0);
		myTriggers->Rumble(effectStrength, effectStrength, 100);
		myTriggers->Sine(200, 200, effectStrength);
	}
	else if (ff == 8 && speed > 0.1f)
	{
		double effectStrength = 0.10 * roadScale;
		myTriggers->Rumble(effectStrength, effectStrength, 100);
		myTriggers->Sine(70, 70, effectStrength);
	}
	else if (ff == 4 && speed > 0.1f)
	{
		double effectStrength = 0.20 * roadScale;
		myTriggers->Rumble(effectStrength, effectStrength, 50);
		myTriggers->Sine(50, 50, effectStrength);
	}
	else if (ff == 16 && speed > 0.1f)
	{
		double effectStrength = 0.20 * roadScale;
		myTriggers->Rumble(effectStrength, effectStrength, 50);
		myTriggers->Sine(100, 50, effectStrength);
	}

	// Add the gear-change kick that existed in the Real implementation but was
	// missing from Custom. It is deliberately a short periodic effect rather than
	// part of the constant steering torque.
	if (oldGear != gear && raceActive == 1 && speed > 0.1f)
	{
		double gearKick = ClampDouble(ModernGearKickStrength / 100.0, 0.0, 1.0);
		if (gearKick > 0.0)
		{
			myTriggers->Sine(240, 320, gearKick);
			myTriggers->Rumble(gearKick, gearKick, 100);
		}
	}

	double finalTorque = 0.0;
	if (raceActive == 1 || menuActive == 1)
		finalTorque = ClampDouble(filteredBaseTorque + collisionTorque, -1.0, 1.0);

	SendSignedConstant(finalTorque);
	LogModernTelemetry(speed, steering, filteredVelocity, filteredBaseTorque, collisionTorque, finalTorque,
		ff, ff3, ff4, ff5, ffwall, ff7);

	oldFf3 = ff3;
	oldFf4 = ff4;
	oldGear = gear;
	return 0;
}

static DWORD WINAPI RunningLoop(LPVOID lpParam)
{
	while (true)
	{
		ThreadLoop();
		Sleep(16); // ~62.5 Hz, matching the existing OutRun2 integration cadence.
	}
}

void OutRun2Fake::FFBLoop(EffectConstants *constants, Helpers *helpers, EffectTriggers* triggers)
{
	if (!init)
	{
		myTriggers = triggers;
		myConstants = constants;
		myHelpers = helpers;
		CreateThread(NULL, 0, RunningLoop, NULL, 0, NULL);
		init = true;
	}

	while (SDL_WaitEvent(&e) != 0)
	{
		UINT8 transmission = helpers->ReadByte(0x082932C2, false); // Auto or Manual
		myTriggers = triggers;
		myConstants = constants;
		myHelpers = helpers;

		if (ShowButtonNumbersForSetup == 1 && e.type == SDL_JOYBUTTONDOWN)
		{
			char buff[100];
			sprintf_s(buff, "Button %d Pressed", e.jbutton.button);
			MessageBoxA(NULL, buff, "", NULL);
		}

		if (e.type == SDL_JOYBUTTONDOWN && ChangeGearsViaPlugin == 1 && transmission == 1)
		{
			if (e.jbutton.button == Gear1)
				helpers->WriteByte(0x0827A160, 0x01, false);
			else if (e.jbutton.button == Gear2)
				helpers->WriteByte(0x0827A160, 0x02, false);
			else if (e.jbutton.button == Gear3)
				helpers->WriteByte(0x0827A160, 0x03, false);
			else if (e.jbutton.button == Gear4)
				helpers->WriteByte(0x0827A160, 0x04, false);
			else if (e.jbutton.button == Gear5)
				helpers->WriteByte(0x0827A160, 0x05, false);
			else if (e.jbutton.button == Gear6)
				helpers->WriteByte(0x0827A160, 0x06, false);
		}
	}
}
