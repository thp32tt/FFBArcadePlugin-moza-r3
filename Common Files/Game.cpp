#include <Windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <math.h>
#include "Game.h"

typedef unsigned char U8;

// Model 2 Emulator is GameId 25 in the upstream FFB Arcade Plugin config.
// The fork keeps all original game code intact and intercepts only the common
// EffectTriggers dispatch plus the two FFB-byte addresses used by Model 2.
extern int configGameId;
extern wchar_t* settingsFilename;

static const int MODEL2_GAME_ID = 25;

static EffectConstantFunction model2ConstantFunction = NULL;
static EffectSingleStrengthFunction model2SpringFunction = NULL;
static EffectSingleStrengthFunction model2FrictionFunction = NULL;
static EffectSineFunction model2SineFunction = NULL;
static EffectRumbleFunction model2RumbleFunction = NULL;

static bool model2ModernSettingsLoaded = false;
static int model2ModernFFB = 1;
static int model2ModernConstantGain = 100;
static int model2ModernSpringGain = 45;
static int model2ModernFrictionGain = 35;
static int model2ModernSineGain = 30;
static int model2ModernRumbleGain = 20;
static int model2ModernGamma = 85;
static int model2ModernSmoothing = 35;
static double model2ModernConstantState = 0.0;

static double Model2Clamp(double value, double minValue, double maxValue)
{
	if (value < minValue)
		return minValue;
	if (value > maxValue)
		return maxValue;
	return value;
}

static void LoadModel2ModernSettings()
{
	if (model2ModernSettingsLoaded || configGameId != MODEL2_GAME_ID)
		return;

	model2ModernSettingsLoaded = true;
	// Keep the fork-safe defaults if initialization reaches this helper before
	// the global INI path is ready. The normal Model 2 path has it initialized.
	if (settingsFilename == NULL)
		return;

	model2ModernFFB = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFB"), 1, settingsFilename);
	model2ModernConstantGain = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFBConstantGain"), 100, settingsFilename);
	model2ModernSpringGain = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFBSpringGain"), 45, settingsFilename);
	model2ModernFrictionGain = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFBFrictionGain"), 35, settingsFilename);
	model2ModernSineGain = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFBSineGain"), 30, settingsFilename);
	model2ModernRumbleGain = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFBRumbleGain"), 20, settingsFilename);
	model2ModernGamma = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFBGamma"), 85, settingsFilename);
	model2ModernSmoothing = GetPrivateProfileInt(TEXT("Settings"), TEXT("Model2ModernFFBSmoothing"), 35, settingsFilename);
}

static bool Model2ModernEnabled()
{
	if (configGameId != MODEL2_GAME_ID)
		return false;

	LoadModel2ModernSettings();
	return model2ModernFFB != 0;
}

static double Model2ModernCurve(double strength, int gainPercent)
{
	double sign = strength < 0.0 ? -1.0 : 1.0;
	double magnitude = Model2Clamp(fabs(strength), 0.0, 1.0);
	double gamma = Model2Clamp(model2ModernGamma / 100.0, 0.25, 2.50);
	double gain = Model2Clamp(gainPercent / 100.0, 0.0, 2.0);
	return sign * Model2Clamp(pow(magnitude, gamma) * gain, 0.0, 1.0);
}

static void Model2ModernSetConstantSigned(double signedTarget)
{
	if (model2ConstantFunction == NULL)
		return;

	signedTarget = Model2Clamp(signedTarget, -1.0, 1.0);
	double smoothing = Model2Clamp(model2ModernSmoothing / 100.0, 0.0, 1.0);
	double response = 1.0 - (0.75 * smoothing); // 1.00 at 0, 0.25 at 100
	model2ModernConstantState += (signedTarget - model2ModernConstantState) * response;

	if (fabs(model2ModernConstantState) < 0.002)
		model2ModernConstantState = 0.0;

	// Haptic direction is the direction the force originates from:
	// -1 (from left) pushes the wheel right, +1 pushes it left.
	if (model2ModernConstantState > 0.0)
		model2ConstantFunction(-1, model2ModernConstantState);
	else if (model2ModernConstantState < 0.0)
		model2ConstantFunction(1, -model2ModernConstantState);
	else
		model2ConstantFunction(-1, 0.0);
}

static void Model2ModernResetNonConstantEffects()
{
	if (model2SpringFunction != NULL)
		model2SpringFunction(0.0);
	if (model2FrictionFunction != NULL)
		model2FrictionFunction(0.0);
	if (model2SineFunction != NULL)
		model2SineFunction(40, 0, 0.0);
	if (model2RumbleFunction != NULL)
		model2RumbleFunction(0.0, 0.0, 100.0);
}

static void Model2ModernProcessGenericFFB(UINT8 ff)
{
	Model2ModernResetNonConstantEffects();
	double constantTarget = 0.0;

	if ((ff > 0x09) && (ff < 0x18))
	{
		// Spring command. Keep the upstream signed mapping, then apply the
		// DD-oriented curve/gain without changing the command semantics.
		double percentForce = (ff - 15) / 8.0;
		if (model2SpringFunction != NULL)
			model2SpringFunction(Model2ModernCurve(percentForce, model2ModernSpringGain));
	}
	else if ((ff > 0x1F) && (ff < 0x28))
	{
		// Friction / clutch command.
		double percentForce = (ff - 31) / 8.0;
		if (model2FrictionFunction != NULL)
			model2FrictionFunction(Model2ModernCurve(percentForce, model2ModernFrictionGain));
	}
	else if ((ff > 0x2F) && (ff < 0x3D))
	{
		// Centering command.
		double percentForce = (ff - 47) / 13.0;
		if (model2SpringFunction != NULL)
			model2SpringFunction(Model2ModernCurve(percentForce, model2ModernSpringGain));
	}
	else if ((ff > 0x3F) && (ff < 0x48))
	{
		// Uncentering / vibration command.
		double percentForce = (ff - 63) / 8.0;
		double sineForce = Model2ModernCurve(percentForce, model2ModernSineGain);
		double rumbleForce = Model2ModernCurve(percentForce, model2ModernRumbleGain);
		if (model2SineFunction != NULL)
			model2SineFunction(40, 0, sineForce);
		if (model2RumbleFunction != NULL)
			model2RumbleFunction(rumbleForce, rumbleForce, 100.0);
	}
	else if ((ff > 0x4F) && (ff < 0x58))
	{
		// Roll left: upstream uses DIRECTION_FROM_RIGHT.
		double percentForce = (ff - 79) / 8.0;
		double force = Model2ModernCurve(percentForce, model2ModernConstantGain);
		double rumbleForce = Model2ModernCurve(percentForce, model2ModernRumbleGain);
		constantTarget = -force;
		if (model2RumbleFunction != NULL)
			model2RumbleFunction(0.0, rumbleForce, 100.0);
	}
	else if ((ff > 0x5F) && (ff < 0x68))
	{
		// Roll right: upstream uses DIRECTION_FROM_LEFT.
		double percentForce = (ff - 95) / 8.0;
		double force = Model2ModernCurve(percentForce, model2ModernConstantGain);
		double rumbleForce = Model2ModernCurve(percentForce, model2ModernRumbleGain);
		constantTarget = force;
		if (model2RumbleFunction != NULL)
			model2RumbleFunction(rumbleForce, 0.0, 100.0);
	}

	// Updating every read lets the signed smoothing converge and also releases
	// stale constant force when the arcade command changes to another mode.
	Model2ModernSetConstantSigned(constantTarget);
}

static void Model2ModernProcessSegaRallyFFB(UINT8 ff)
{
	Model2ModernResetNonConstantEffects();
	double constantTarget = 0.0;

	if (ff >= 0xC0 && ff <= 0xDF)
	{
		double percentForce = (ff - 191) / 32.0;
		double force = Model2ModernCurve(percentForce, model2ModernConstantGain);
		double rumbleForce = Model2ModernCurve(percentForce, model2ModernRumbleGain);
		constantTarget = -force;
		if (model2RumbleFunction != NULL)
			model2RumbleFunction(0.0, rumbleForce, 100.0);
	}
	else if (ff >= 0x80 && ff <= 0x9F)
	{
		double percentForce = (ff - 127) / 32.0;
		double force = Model2ModernCurve(percentForce, model2ModernConstantGain);
		double rumbleForce = Model2ModernCurve(percentForce, model2ModernRumbleGain);
		constantTarget = force;
		if (model2RumbleFunction != NULL)
			model2RumbleFunction(rumbleForce, 0.0, 100.0);
	}

	Model2ModernSetConstantSigned(constantTarget);
}

static void Model2ModernObserveReadByte(INT_PTR offset, bool isRelativeOffset, UINT8 value)
{
	if (!Model2ModernEnabled())
		return;

	// Sega Rally Championship has a separate FFB byte from the other Model 2
	// racing titles supported by the plugin.
	if (isRelativeOffset && offset == 0x174CF4)
	{
		Model2ModernProcessSegaRallyFFB(value);
		return;
	}

	// Daytona USA uses an absolute address while Indy 500, STCC, Over Rev and
	// Super GT 24h use the same relative FFB offset.
	if ((!isRelativeOffset && offset == 0x0057285B) ||
		(isRelativeOffset && offset == 0x17285B))
	{
		Model2ModernProcessGenericFFB(value);
	}
}

EffectConstantTriggerSlot& EffectConstantTriggerSlot::operator=(EffectConstantFunction value)
{
	function = value;
	model2ConstantFunction = value;
	return *this;
}

void EffectConstantTriggerSlot::operator()(int direction, double strength) const
{
	// Model2ModernObserveReadByte already emitted the modern effect for this
	// frame. Suppress the legacy decoder's duplicate call only for Model 2.
	if (Model2ModernEnabled())
		return;
	if (function != NULL)
		function(direction, strength);
}

EffectSpringTriggerSlot& EffectSpringTriggerSlot::operator=(EffectSingleStrengthFunction value)
{
	function = value;
	model2SpringFunction = value;
	return *this;
}

void EffectSpringTriggerSlot::operator()(double strength) const
{
	if (Model2ModernEnabled())
		return;
	if (function != NULL)
		function(strength);
}

EffectFrictionTriggerSlot& EffectFrictionTriggerSlot::operator=(EffectSingleStrengthFunction value)
{
	function = value;
	model2FrictionFunction = value;
	return *this;
}

void EffectFrictionTriggerSlot::operator()(double strength) const
{
	if (Model2ModernEnabled())
		return;
	if (function != NULL)
		function(strength);
}

EffectSineTriggerSlot& EffectSineTriggerSlot::operator=(EffectSineFunction value)
{
	function = value;
	model2SineFunction = value;
	return *this;
}

void EffectSineTriggerSlot::operator()(UINT16 period, UINT16 fadePeriod, double strength) const
{
	if (Model2ModernEnabled())
		return;
	if (function != NULL)
		function(period, fadePeriod, strength);
}

EffectRumbleTriggerSlot& EffectRumbleTriggerSlot::operator=(EffectRumbleFunction value)
{
	function = value;
	model2RumbleFunction = value;
	return *this;
}

void EffectRumbleTriggerSlot::operator()(double lowfrequency, double highfrequency, double length) const
{
	if (Model2ModernEnabled())
		return;
	if (function != NULL)
		function(lowfrequency, highfrequency, length);
}

bool Helpers::fileExists(char *filename)
{
	std::ifstream ifile(filename);
	return !ifile.fail();
}

void Helpers::log(char *msg) {
	if (enableLogging == 0) { return; }
	std::ofstream ofs("FFBlog.txt", std::ofstream::app);
	ofs << msg << std::endl;
	ofs.close();
}

void Helpers::logInt(int value) {
	std::string njs = std::to_string(value);
	log((char *)njs.c_str());
}

void Helpers::logInit(char *msg) {
	if (enableLogging == 0) { return; }
	std::ofstream ofs("FFBlog.txt", std::ofstream::out);
	ofs << msg << std::endl;
	ofs.close();
}

void Helpers::info(const char* format, ...)
{
	va_list args;
	char buffer[1024];

	va_start(args, format);
	int len = _vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	buffer[len] = '\n';
	buffer[len + 1] = '\0';

	OutputDebugStringA(buffer);
}

// reading memory
LPVOID Helpers::GetTranslatedOffset(INT_PTR offset)
{
	return reinterpret_cast<LPVOID>((INT_PTR)GetModuleHandle(NULL) + offset);
}

UINT8 Helpers::ReadByte(INT_PTR offset, bool isRelativeOffset)
{
	UINT8 val = 0;
	SIZE_T read;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(UINT8), &read);
	Model2ModernObserveReadByte(offset, isRelativeOffset, val);
	return val;
}

float Helpers::WriteFloat32(INT_PTR offset, float val, bool isRelativeOffset)
{
	//val = 0.0f;
	SIZE_T written;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	WriteProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(float), &written);
	return val;
};

UINT8 Helpers::WriteByte(INT_PTR offset, UINT8 val, bool isRelativeOffset)
{
	SIZE_T written;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	WriteProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(UINT8), &written);
	return val;
}

WORD Helpers::WriteWord(INT_PTR offset, WORD val, bool isRelativeOffset)
{
	SIZE_T written;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	WriteProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(WORD), &written);
	return val;
}

INT_PTR Helpers::WriteIntPtr(INT_PTR offset, INT_PTR val, bool isRelativeOffset)
{
	SIZE_T written;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	WriteProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(INT_PTR), &written);
	return val;
};

UINT8 Helpers::WriteNop(INT_PTR offset, int countBytes, bool isRelativeOffset)
{
	U8 nop = 0x90;
	SIZE_T written;
	for (int i = 0; i < countBytes; i++)
	{
		offset = offset + i;
		LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
		WriteProcessMemory(GetCurrentProcess(), trueOffset, &nop, 1, &written);
		offset = offset - i;
	}
	return nop;
}

int Helpers::ReadInt32(INT_PTR offset, bool isRelativeOffset)
{
	int val = 0;
	SIZE_T read;
	//log("going to try to RPM");
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(int), &read);
	//log("RPM");
	//char text[256];
	//sprintf_s(text, "%16X / %16X\n", offset, trueOffset);
	//log(text);
	return val;
}

INT_PTR Helpers::ReadIntPtr(INT_PTR offset, bool isRelativeOffset)
{
	SIZE_T read;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	INT_PTR val;
	ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(INT_PTR), &read);
	return val;
};

WORD Helpers::ReadWord(INT_PTR offset, bool isRelativeOffset)
{
	SIZE_T read;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	WORD val;
	ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(WORD), &read);
	return val;
};

long long Helpers::ReadLong(INT_PTR offset, bool isRelativeOffset)
{
	SIZE_T read;
	LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
	long long val;
	ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(long long), &read);
	return val;
};

float Helpers::ReadFloat32(INT_PTR offset, bool isRelativeOffset)
{
		
		float val = 0.0f;
		SIZE_T read;
		//log("going to try to RPM");
		LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
		ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(float), &read);
		//char text[256];
		//sprintf_s(text, "%16X / %16X\n", offset, trueOffset);
		//log(text);
		//log("RPM");
		return val;
		
};

void Game::FFBLoop(EffectConstants * constants, Helpers * helpers, EffectTriggers * triggers)
{
	return;
}
