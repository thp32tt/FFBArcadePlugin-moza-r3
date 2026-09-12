#include <Windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include "SDL.h"
#include "Game.h"

typedef unsigned char U8;

// Model 2 Emulator is GameId 25 in upstream FFB Arcade Plugin.
extern int configGameId;
extern wchar_t* settingsFilename;
extern int configMinForce;
extern int configMaxForce;
extern int configFeedbackLength;
extern int AlternativeFFB;
extern int configAlternativeMinForceLeft;
extern int configAlternativeMaxForceLeft;
extern int configAlternativeMinForceRight;
extern int configAlternativeMaxForceRight;
extern SDL_Haptic* haptic;
extern EffectCollection effects;
extern HWND hWndM2;

static const int MODEL2_GAME_ID = 25;

static EffectConstantFunction model2LegacyConstantFunction = NULL;
static EffectSingleStrengthFunction model2LegacySpringFunction = NULL;
static EffectSingleStrengthFunction model2LegacyFrictionFunction = NULL;
static EffectSineFunction model2LegacySineFunction = NULL;
static EffectRumbleFunction model2LegacyRumbleFunction = NULL;
static EffectSingleStrengthFunction model2LegacySpringInfiniteFunction = NULL;
static EffectSingleStrengthFunction model2LegacyDamperFunction = NULL;

enum Model2ModernCommand
{
    M2CMD_NONE = 0,
    M2CMD_SPRING,
    M2CMD_FRICTION,
    M2CMD_CENTERING,
    M2CMD_UNCENTERING,
    M2CMD_LEFT,
    M2CMD_RIGHT
};

struct Model2ModernSettings
{
    int enabled;
    int constantGain;
    int springGain;
    int frictionGain;
    int periodicGain;
    int rumbleGain;
    int gamma;
    int buildRate;
    int reversalReleaseRate;
    int softClipKnee;
    int softClipLimit;
    int invertConstant;
    int invertSpring;
    int periodicMode;      // 0 Auto, 1 Native, 2 ConstantPulse, 3 Off
    int conditionMode;     // 0 Auto, 1 Native, 2 Off
    int focusSafety;
    int commandLeaseMs;
    int telemetry;
};

static bool model2ModernSettingsLoaded = false;
static Model2ModernSettings model2Global = {
    1, 100, 45, 35, 30, 20, 100, 18, 40, 75, 135, 0, 0, 0, 0, 1, 180, 0
};
static Model2ModernSettings model2Profile = model2Global;
static std::string model2ProfileName = "Generic";
static Model2ModernCommand model2ActiveCommand = M2CMD_NONE;
static double model2ConstantState = 0.0;
static bool model2Suspended = false;
static DWORD model2ResumeAt = 0;
static DWORD model2LastTelemetry = 0;
static UINT8 model2LastRaw = 0;
static unsigned int model2Capabilities = 0;
static bool model2CapabilitiesLogged = false;

static double Model2Clamp(double value, double minValue, double maxValue)
{
    if (!std::isfinite(value))
        return 0.0;
    return std::max(minValue, std::min(maxValue, value));
}

static int Model2ClampInt(int value, int minValue, int maxValue)
{
    return std::max(minValue, std::min(maxValue, value));
}

static double Model2MoveToward(double current, double target, double amount)
{
    amount = Model2Clamp(amount, 0.0, 1.0);
    if (current < target)
        return std::min(target, current + amount);
    if (current > target)
        return std::max(target, current - amount);
    return current;
}

static double Model2SoftSaturate(double value)
{
    if (!std::isfinite(value))
        return 0.0;

    const double sign = value < 0.0 ? -1.0 : 1.0;
    const double x = fabs(value);
    double knee = Model2Clamp(model2Profile.softClipKnee / 100.0, 0.10, 0.95);
    double limit = Model2Clamp(model2Profile.softClipLimit / 100.0, knee + 0.05, 2.50);

    if (x <= knee)
        return value;
    if (x >= limit)
        return sign;

    const double span = limit - knee;
    const double t = (x - knee) / span;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double y = h00 * knee + h10 * span + h01;
    return sign * Model2Clamp(y, 0.0, 1.0);
}

static double Model2Shape(double strength, int gainPercent)
{
    const double sign = strength < 0.0 ? -1.0 : 1.0;
    const double magnitude = Model2Clamp(fabs(strength), 0.0, 1.0);
    const double gamma = Model2Clamp(model2Profile.gamma / 100.0, 0.25, 2.50);
    const double gain = Model2Clamp(gainPercent / 100.0, 0.0, 2.0);
    return sign * pow(magnitude, gamma) * gain;
}

static void Model2LogLine(const std::string& line)
{
    OutputDebugStringA((std::string("Model2ModernFFB: ") + line + "\n").c_str());
    if (!model2Profile.telemetry)
        return;
    std::ofstream ofs("Model2ModernFFB.log", std::ofstream::app);
    if (ofs)
        ofs << line << std::endl;
}

static int Model2ReadGlobalInt(const wchar_t* key, int fallback)
{
    if (settingsFilename == NULL)
        return fallback;
    return GetPrivateProfileInt(TEXT("Model2Modern"), key, fallback, settingsFilename);
}

static int Model2ReadProfileInt(const wchar_t* section, const wchar_t* key, int fallback)
{
    if (settingsFilename == NULL)
        return fallback;
    return GetPrivateProfileInt(section, key, fallback, settingsFilename);
}

static void LoadModel2ModernSettings()
{
    if (model2ModernSettingsLoaded || configGameId != MODEL2_GAME_ID || settingsFilename == NULL)
        return;

    model2ModernSettingsLoaded = true;

    // Keep v1 [Settings] keys as compatibility fallbacks, then let the neutral
    // [Model2Modern] section override them.
    model2Global.enabled = GetPrivateProfileInt(
        TEXT("Settings"), TEXT("Model2ModernFFB"), 1, settingsFilename);
    model2Global.constantGain = GetPrivateProfileInt(
        TEXT("Settings"), TEXT("Model2ModernFFBConstantGain"), 100, settingsFilename);
    model2Global.springGain = GetPrivateProfileInt(
        TEXT("Settings"), TEXT("Model2ModernFFBSpringGain"), 45, settingsFilename);
    model2Global.frictionGain = GetPrivateProfileInt(
        TEXT("Settings"), TEXT("Model2ModernFFBFrictionGain"), 35, settingsFilename);
    model2Global.periodicGain = GetPrivateProfileInt(
        TEXT("Settings"), TEXT("Model2ModernFFBSineGain"), 30, settingsFilename);
    model2Global.rumbleGain = GetPrivateProfileInt(
        TEXT("Settings"), TEXT("Model2ModernFFBRumbleGain"), 20, settingsFilename);
    model2Global.gamma = GetPrivateProfileInt(
        TEXT("Settings"), TEXT("Model2ModernFFBGamma"), 100, settingsFilename);

    model2Global.enabled = Model2ReadGlobalInt(TEXT("Enabled"), model2Global.enabled);
    model2Global.constantGain = Model2ReadGlobalInt(TEXT("ConstantGain"), model2Global.constantGain);
    model2Global.springGain = Model2ReadGlobalInt(TEXT("SpringGain"), model2Global.springGain);
    model2Global.frictionGain = Model2ReadGlobalInt(TEXT("FrictionGain"), model2Global.frictionGain);
    model2Global.periodicGain = Model2ReadGlobalInt(TEXT("PeriodicGain"), model2Global.periodicGain);
    model2Global.rumbleGain = Model2ReadGlobalInt(TEXT("RumbleGain"), model2Global.rumbleGain);
    model2Global.gamma = Model2ReadGlobalInt(TEXT("Gamma"), model2Global.gamma);
    model2Global.buildRate = Model2ReadGlobalInt(TEXT("BuildRate"), 18);
    model2Global.reversalReleaseRate = Model2ReadGlobalInt(TEXT("ReversalReleaseRate"), 40);
    model2Global.softClipKnee = Model2ReadGlobalInt(TEXT("SoftClipKnee"), 75);
    model2Global.softClipLimit = Model2ReadGlobalInt(TEXT("SoftClipLimit"), 135);
    model2Global.invertConstant = Model2ReadGlobalInt(TEXT("InvertConstant"), 0);
    model2Global.invertSpring = Model2ReadGlobalInt(TEXT("InvertSpring"), 0);
    model2Global.periodicMode = Model2ReadGlobalInt(TEXT("PeriodicMode"), 0);
    model2Global.conditionMode = Model2ReadGlobalInt(TEXT("ConditionMode"), 0);
    model2Global.focusSafety = Model2ReadGlobalInt(TEXT("FocusSafety"), 1);
    model2Global.commandLeaseMs = Model2ReadGlobalInt(TEXT("CommandLeaseMs"), 180);
    model2Global.telemetry = Model2ReadGlobalInt(TEXT("Telemetry"), 0);

    model2Global.constantGain = Model2ClampInt(model2Global.constantGain, 0, 200);
    model2Global.springGain = Model2ClampInt(model2Global.springGain, 0, 200);
    model2Global.frictionGain = Model2ClampInt(model2Global.frictionGain, 0, 200);
    model2Global.periodicGain = Model2ClampInt(model2Global.periodicGain, 0, 200);
    model2Global.rumbleGain = Model2ClampInt(model2Global.rumbleGain, 0, 200);
    model2Global.gamma = Model2ClampInt(model2Global.gamma, 25, 250);
    model2Global.buildRate = Model2ClampInt(model2Global.buildRate, 1, 100);
    model2Global.reversalReleaseRate = Model2ClampInt(model2Global.reversalReleaseRate, 2, 100);
    model2Global.softClipKnee = Model2ClampInt(model2Global.softClipKnee, 10, 95);
    model2Global.softClipLimit = Model2ClampInt(
        model2Global.softClipLimit, model2Global.softClipKnee + 5, 250);
    model2Global.periodicMode = Model2ClampInt(model2Global.periodicMode, 0, 3);
    model2Global.conditionMode = Model2ClampInt(model2Global.conditionMode, 0, 2);
    model2Global.commandLeaseMs = Model2ClampInt(model2Global.commandLeaseMs, 60, 500);
    model2Profile = model2Global;
}

static const wchar_t* Model2ProfileSectionForTitle(const char* title, std::string& profileName)
{
    const std::string t = title ? title : "";
    if (t.find("Sega Rally") != std::string::npos)
    {
        profileName = "SegaRally";
        return TEXT("Model2Modern.SegaRally");
    }
    if (t.find("Daytona") != std::string::npos)
    {
        profileName = "Daytona";
        return TEXT("Model2Modern.Daytona");
    }
    if (t.find("Indianapolis") != std::string::npos)
    {
        profileName = "Indy500";
        return TEXT("Model2Modern.Indy500");
    }
    if (t.find("Touring Car") != std::string::npos)
    {
        profileName = "STCC";
        return TEXT("Model2Modern.STCC");
    }
    if (t.find("Over Rev") != std::string::npos)
    {
        profileName = "OverRev";
        return TEXT("Model2Modern.OverRev");
    }
    if (t.find("Super GT") != std::string::npos)
    {
        profileName = "SuperGT";
        return TEXT("Model2Modern.SuperGT");
    }
    profileName = "Generic";
    return TEXT("Model2Modern.Generic");
}

static void Model2UpdateProfile()
{
    LoadModel2ModernSettings();
    if (!model2ModernSettingsLoaded)
        return;

    char title[256] = {};
    if (hWndM2 != NULL)
        GetWindowTextA(hWndM2, title, sizeof(title));

    std::string nextProfile;
    const wchar_t* section = Model2ProfileSectionForTitle(title, nextProfile);
    if (nextProfile == model2ProfileName)
        return;

    model2ProfileName = nextProfile;
    model2Profile = model2Global;
    model2Profile.constantGain = Model2ReadProfileInt(section, TEXT("ConstantGain"), model2Global.constantGain);
    model2Profile.springGain = Model2ReadProfileInt(section, TEXT("SpringGain"), model2Global.springGain);
    model2Profile.frictionGain = Model2ReadProfileInt(section, TEXT("FrictionGain"), model2Global.frictionGain);
    model2Profile.periodicGain = Model2ReadProfileInt(section, TEXT("PeriodicGain"), model2Global.periodicGain);
    model2Profile.rumbleGain = Model2ReadProfileInt(section, TEXT("RumbleGain"), model2Global.rumbleGain);
    model2Profile.gamma = Model2ReadProfileInt(section, TEXT("Gamma"), model2Global.gamma);
    model2Profile.buildRate = Model2ReadProfileInt(section, TEXT("BuildRate"), model2Global.buildRate);
    model2Profile.reversalReleaseRate = Model2ReadProfileInt(
        section, TEXT("ReversalReleaseRate"), model2Global.reversalReleaseRate);
    model2Profile.invertConstant = Model2ReadProfileInt(
        section, TEXT("InvertConstant"), model2Global.invertConstant);
    model2Profile.invertSpring = Model2ReadProfileInt(
        section, TEXT("InvertSpring"), model2Global.invertSpring);

    model2Profile.constantGain = Model2ClampInt(model2Profile.constantGain, 0, 200);
    model2Profile.springGain = Model2ClampInt(model2Profile.springGain, 0, 200);
    model2Profile.frictionGain = Model2ClampInt(model2Profile.frictionGain, 0, 200);
    model2Profile.periodicGain = Model2ClampInt(model2Profile.periodicGain, 0, 200);
    model2Profile.rumbleGain = Model2ClampInt(model2Profile.rumbleGain, 0, 200);
    model2Profile.gamma = Model2ClampInt(model2Profile.gamma, 25, 250);
    model2Profile.buildRate = Model2ClampInt(model2Profile.buildRate, 1, 100);
    model2Profile.reversalReleaseRate = Model2ClampInt(model2Profile.reversalReleaseRate, 2, 100);

    std::ostringstream os;
    os << "profile=" << model2ProfileName
       << " constant=" << model2Profile.constantGain
       << " spring=" << model2Profile.springGain
       << " friction=" << model2Profile.frictionGain
       << " periodic=" << model2Profile.periodicGain
       << " build=" << model2Profile.buildRate
       << " reverseRelease=" << model2Profile.reversalReleaseRate;
    Model2LogLine(os.str());
}

static bool Model2ModernEnabled()
{
    if (configGameId != MODEL2_GAME_ID)
        return false;
    LoadModel2ModernSettings();
    return model2ModernSettingsLoaded && model2Global.enabled != 0;
}

static bool Model2WindowFocused()
{
    if (hWndM2 == NULL || !IsWindow(hWndM2))
        return false;
    HWND foreground = GetForegroundWindow();
    if (foreground == NULL)
        return false;
    HWND gameRoot = GetAncestor(hWndM2, GA_ROOT);
    HWND foregroundRoot = GetAncestor(foreground, GA_ROOT);
    return foreground == hWndM2 || foregroundRoot == gameRoot;
}

static void Model2QueryCapabilities()
{
    if (haptic == NULL)
        return;
    model2Capabilities = SDL_HapticQuery(haptic);
    if (!model2CapabilitiesLogged)
    {
        model2CapabilitiesLogged = true;
        std::ostringstream os;
        os << "SDL haptic caps=0x" << std::hex << model2Capabilities << std::dec
           << " constant=" << ((model2Capabilities & SDL_HAPTIC_CONSTANT) ? 1 : 0)
           << " spring=" << ((model2Capabilities & SDL_HAPTIC_SPRING) ? 1 : 0)
           << " friction=" << ((model2Capabilities & SDL_HAPTIC_FRICTION) ? 1 : 0)
           << " sine=" << ((model2Capabilities & SDL_HAPTIC_SINE) ? 1 : 0);
        Model2LogLine(os.str());
    }
}

static void Model2StopEffect(int effectId)
{
    if (haptic != NULL && effectId >= 0)
        SDL_HapticStopEffect(haptic, effectId);
}

static void Model2StopRumble()
{
    if (model2LegacyRumbleFunction != NULL)
        model2LegacyRumbleFunction(0.0, 0.0, 0.0);
}

static void Model2StopOwnedEffects()
{
    Model2StopEffect(effects.effect_constant_id);
    Model2StopEffect(effects.effect_spring_id);
    Model2StopEffect(effects.effect_friction_id);
    Model2StopEffect(effects.effect_sine_id);
    Model2StopRumble();
    model2ConstantState = 0.0;
    model2ActiveCommand = M2CMD_NONE;
}

static bool Model2EffectSupported(unsigned int type, int effectId)
{
    Model2QueryCapabilities();
    return haptic != NULL && effectId >= 0 && (model2Capabilities & type) != 0;
}

static Uint32 Model2LeaseLength()
{
    const int configured = configFeedbackLength > 0 ? configFeedbackLength : 120;
    return static_cast<Uint32>(std::min(configured, model2Global.commandLeaseMs));
}

static SHORT Model2ScaleSignedLevel(double strength, int direction)
{
    strength = Model2Clamp(strength, 0.0, 1.0);
    int minPct = configMinForce;
    int maxPct = configMaxForce;
    if (AlternativeFFB)
    {
        if (direction == -1)
        {
            minPct = configAlternativeMinForceLeft;
            maxPct = configAlternativeMaxForceLeft;
        }
        else
        {
            minPct = configAlternativeMinForceRight;
            maxPct = configAlternativeMaxForceRight;
        }
    }

    const double minForce = strength > 0.001 ? (minPct / 100.0 * 32767.0) : 0.0;
    const double maxForce = maxPct / 100.0 * 32767.0;
    const double range = maxForce - minForce;
    double level = strength * range + minForce;

    if (range > 0.0 && level < 0.0)
        level = 32767.0;
    else if (range < 0.0 && level > 0.0)
        level = -32767.0;

    return static_cast<SHORT>(Model2Clamp(level, -32767.0, 32767.0));
}

static SHORT Model2ScaleConditionCoeff(double strength)
{
    strength = Model2Clamp(strength, 0.0, 1.0);
    const double minForce = strength > 0.001 ? (configMinForce / 100.0 * 32767.0) : 0.0;
    const double maxForce = configMaxForce / 100.0 * 32767.0;
    double coeff = strength * (maxForce - minForce) + minForce;
    if (coeff < 0.0)
        coeff = 32767.0;
    return static_cast<SHORT>(Model2Clamp(coeff, 0.0, 32767.0));
}

static void Model2OutputConstant(double signedStrength)
{
    if (!Model2EffectSupported(SDL_HAPTIC_CONSTANT, effects.effect_constant_id))
        return;

    if (fabs(signedStrength) <= 0.001)
    {
        Model2StopEffect(effects.effect_constant_id);
        return;
    }

    int direction = signedStrength > 0.0 ? -1 : 1;
    if (model2Profile.invertConstant)
        direction = -direction;

    SDL_HapticEffect effect;
    SDL_memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.constant.direction.dir[0] = direction;
    effect.constant.length = Model2LeaseLength();
    effect.constant.delay = 0;
    effect.constant.level = Model2ScaleSignedLevel(fabs(signedStrength), direction);

    const int update = SDL_HapticUpdateEffect(haptic, effects.effect_constant_id, &effect);
    if (update == 0)
        SDL_HapticRunEffect(haptic, effects.effect_constant_id, 1);
}

static bool Model2ConditionsEnabled()
{
    if (model2Global.conditionMode == 2)
        return false;
    return true;
}

static void Model2OutputSpring(double strength)
{
    if (!Model2ConditionsEnabled() ||
        !Model2EffectSupported(SDL_HAPTIC_SPRING, effects.effect_spring_id))
    {
        Model2StopEffect(effects.effect_spring_id);
        return;
    }

    strength = Model2SoftSaturate(Model2Shape(strength, model2Profile.springGain));
    if (strength <= 0.001)
    {
        Model2StopEffect(effects.effect_spring_id);
        return;
    }

    const SHORT coeff = Model2ScaleConditionCoeff(strength);
    SDL_HapticEffect effect;
    SDL_memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_SPRING;
    effect.condition.type = SDL_HAPTIC_SPRING;
    effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.condition.direction.dir[0] = model2Profile.invertSpring ? -1 : 1;
    effect.condition.length = Model2LeaseLength();
    effect.condition.delay = 0;
    effect.condition.left_coeff[0] = coeff;
    effect.condition.right_coeff[0] = coeff;
    const LONG sat = std::min<LONG>(32767, static_cast<LONG>(coeff) * 2L);
    effect.condition.left_sat[0] = static_cast<Uint16>(sat);
    effect.condition.right_sat[0] = static_cast<Uint16>(sat);
    effect.condition.center[0] = 0;

    const int update = SDL_HapticUpdateEffect(haptic, effects.effect_spring_id, &effect);
    if (update == 0)
        SDL_HapticRunEffect(haptic, effects.effect_spring_id, 1);
}

static void Model2OutputFriction(double strength)
{
    if (!Model2ConditionsEnabled() ||
        !Model2EffectSupported(SDL_HAPTIC_FRICTION, effects.effect_friction_id))
    {
        Model2StopEffect(effects.effect_friction_id);
        return;
    }

    strength = Model2SoftSaturate(Model2Shape(strength, model2Profile.frictionGain));
    if (strength <= 0.001)
    {
        Model2StopEffect(effects.effect_friction_id);
        return;
    }

    const SHORT coeff = Model2ScaleConditionCoeff(strength);
    SDL_HapticEffect effect;
    SDL_memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_FRICTION;
    effect.condition.type = SDL_HAPTIC_FRICTION;
    effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.condition.direction.dir[0] = 1;
    effect.condition.length = Model2LeaseLength();
    effect.condition.delay = 0;
    effect.condition.left_coeff[0] = coeff;
    effect.condition.right_coeff[0] = coeff;
    effect.condition.left_sat[0] = 0xFFFF;
    effect.condition.right_sat[0] = 0xFFFF;

    const int update = SDL_HapticUpdateEffect(haptic, effects.effect_friction_id, &effect);
    if (update == 0)
        SDL_HapticRunEffect(haptic, effects.effect_friction_id, 1);
}

static bool Model2NativePeriodicAvailable()
{
    if (model2Global.periodicMode == 3 || model2Global.periodicMode == 2)
        return false;
    return Model2EffectSupported(SDL_HAPTIC_SINE, effects.effect_sine_id);
}

static void Model2OutputNativePeriodic(double strength)
{
    if (!Model2NativePeriodicAvailable())
        return;

    strength = Model2SoftSaturate(Model2Shape(strength, model2Profile.periodicGain));
    if (strength <= 0.001)
    {
        Model2StopEffect(effects.effect_sine_id);
        return;
    }

    SDL_HapticEffect effect;
    SDL_memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_SINE;
    effect.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.periodic.direction.dir[0] = 1;
    effect.periodic.period = 40;
    effect.periodic.magnitude = Model2ScaleConditionCoeff(strength);
    effect.periodic.length = static_cast<Uint32>(std::min(model2Global.commandLeaseMs, 80));
    effect.periodic.attack_length = 0;
    effect.periodic.fade_length = 0;

    const int update = SDL_HapticUpdateEffect(haptic, effects.effect_sine_id, &effect);
    if (update == 0)
        SDL_HapticRunEffect(haptic, effects.effect_sine_id, 1);
}

static void Model2OutputPeriodicFallback(double strength)
{
    if (model2Global.periodicMode == 3)
        return;

    strength = Model2SoftSaturate(Model2Shape(strength, model2Profile.periodicGain));
    const DWORD phase = (GetTickCount() / 20) & 1;
    const double pulse = phase ? strength : -strength;
    Model2OutputConstant(pulse);
}

static void Model2OutputRumble(double left, double right)
{
    if (model2LegacyRumbleFunction == NULL)
        return;
    left = Model2SoftSaturate(Model2Shape(left, model2Profile.rumbleGain));
    right = Model2SoftSaturate(Model2Shape(right, model2Profile.rumbleGain));
    model2LegacyRumbleFunction(
        Model2Clamp(left, 0.0, 1.0),
        Model2Clamp(right, 0.0, 1.0),
        100.0);
}

static const char* Model2CommandName(Model2ModernCommand command)
{
    switch (command)
    {
    case M2CMD_SPRING: return "spring";
    case M2CMD_FRICTION: return "friction";
    case M2CMD_CENTERING: return "centering";
    case M2CMD_UNCENTERING: return "uncentering";
    case M2CMD_LEFT: return "left";
    case M2CMD_RIGHT: return "right";
    default: return "none";
    }
}

static void Model2BeginCommand(Model2ModernCommand command)
{
    if (command == model2ActiveCommand)
        return;

    const bool oldConstant =
        model2ActiveCommand == M2CMD_LEFT || model2ActiveCommand == M2CMD_RIGHT;
    const bool newConstant = command == M2CMD_LEFT || command == M2CMD_RIGHT;

    Model2StopEffect(effects.effect_spring_id);
    Model2StopEffect(effects.effect_friction_id);
    Model2StopEffect(effects.effect_sine_id);
    Model2StopRumble();

    if (!(oldConstant && newConstant))
    {
        Model2StopEffect(effects.effect_constant_id);
        model2ConstantState = 0.0;
    }

    model2ActiveCommand = command;

    if (model2Profile.telemetry)
    {
        std::ostringstream os;
        os << "command=" << Model2CommandName(command)
           << " raw=0x" << std::hex << static_cast<int>(model2LastRaw) << std::dec;
        Model2LogLine(os.str());
    }
}

static void Model2OutputStructuralConstant(double target)
{
    target = Model2SoftSaturate(Model2Shape(target, model2Profile.constantGain));
    const double buildStep = Model2Clamp(model2Profile.buildRate / 100.0, 0.01, 1.0);
    const double reversalStep =
        Model2Clamp(model2Profile.reversalReleaseRate / 100.0, 0.02, 1.0);

    if (model2ConstantState * target < 0.0 && fabs(model2ConstantState) > 0.001)
    {
        model2ConstantState = Model2MoveToward(model2ConstantState, 0.0, reversalStep);
        if (fabs(model2ConstantState) < 0.001)
            model2ConstantState = 0.0;
    }
    else if (fabs(target) <= 0.001)
    {
        model2ConstantState = Model2MoveToward(model2ConstantState, 0.0, reversalStep);
    }
    else
    {
        model2ConstantState = Model2MoveToward(model2ConstantState, target, buildStep);
    }

    Model2OutputConstant(model2ConstantState);
}

static bool Model2SafetyAllowsOutput()
{
    if (!Model2ModernEnabled())
        return false;

    Model2UpdateProfile();

    if (model2Global.focusSafety && !Model2WindowFocused())
    {
        if (!model2Suspended)
        {
            Model2StopOwnedEffects();
            model2Suspended = true;
            Model2LogLine("output suspended: Model 2 window is not foreground");
        }
        return false;
    }

    if (model2Suspended)
    {
        model2Suspended = false;
        model2ResumeAt = GetTickCount() + 100;
        Model2StopOwnedEffects();
        Model2LogLine("foreground restored: 100 ms FFB warmup");
    }

    if (model2ResumeAt != 0 && static_cast<LONG>(GetTickCount() - model2ResumeAt) < 0)
        return false;
    model2ResumeAt = 0;
    return true;
}

static void Model2Telemetry(UINT8 raw, Model2ModernCommand command, double strength)
{
    if (!model2Profile.telemetry)
        return;

    const DWORD now = GetTickCount();
    if (static_cast<DWORD>(now - model2LastTelemetry) < 250)
        return;
    model2LastTelemetry = now;

    std::ostringstream os;
    os << "profile=" << model2ProfileName
       << " raw=0x" << std::hex << static_cast<int>(raw) << std::dec
       << " command=" << Model2CommandName(command)
       << " strength=" << std::fixed << std::setprecision(3) << strength
       << " constantState=" << model2ConstantState;
    Model2LogLine(os.str());
}

static void Model2ProcessCommand(UINT8 raw, Model2ModernCommand command, double strength)
{
    if (!Model2SafetyAllowsOutput())
        return;

    model2LastRaw = raw;
    strength = Model2Clamp(strength, 0.0, 1.0);
    Model2BeginCommand(command);

    switch (command)
    {
    case M2CMD_SPRING:
    case M2CMD_CENTERING:
        Model2OutputSpring(strength);
        break;

    case M2CMD_FRICTION:
        Model2OutputFriction(strength);
        break;

    case M2CMD_UNCENTERING:
        if (Model2NativePeriodicAvailable())
            Model2OutputNativePeriodic(strength);
        else
            Model2OutputPeriodicFallback(strength);
        Model2OutputRumble(strength, strength);
        break;

    case M2CMD_LEFT:
        Model2OutputStructuralConstant(-strength);
        Model2OutputRumble(0.0, strength);
        break;

    case M2CMD_RIGHT:
        Model2OutputStructuralConstant(strength);
        Model2OutputRumble(strength, 0.0);
        break;

    default:
        Model2StopOwnedEffects();
        break;
    }

    Model2Telemetry(raw, command, strength);
}

static void Model2ProcessGeneric(UINT8 raw)
{
    if ((raw > 0x09) && (raw < 0x18))
    {
        // Preserve the upstream command range but clamp its historically
        // negative 0x0A..0x0E mapping to zero instead of letting the backend
        // turn a negative condition coefficient into full-scale spring force.
        const double strength = Model2Clamp((raw - 15) / 8.0, 0.0, 1.0);
        Model2ProcessCommand(raw, M2CMD_SPRING, strength);
    }
    else if ((raw > 0x1F) && (raw < 0x28))
    {
        Model2ProcessCommand(raw, M2CMD_FRICTION, (raw - 31) / 8.0);
    }
    else if ((raw > 0x2F) && (raw < 0x3D))
    {
        Model2ProcessCommand(raw, M2CMD_CENTERING, (raw - 47) / 13.0);
    }
    else if ((raw > 0x3F) && (raw < 0x48))
    {
        Model2ProcessCommand(raw, M2CMD_UNCENTERING, (raw - 63) / 8.0);
    }
    else if ((raw > 0x4F) && (raw < 0x58))
    {
        Model2ProcessCommand(raw, M2CMD_LEFT, (raw - 79) / 8.0);
    }
    else if ((raw > 0x5F) && (raw < 0x68))
    {
        Model2ProcessCommand(raw, M2CMD_RIGHT, (raw - 95) / 8.0);
    }
    else
    {
        Model2ProcessCommand(raw, M2CMD_NONE, 0.0);
    }
}

static void Model2ProcessSegaRally(UINT8 raw)
{
    if (raw >= 0xC0 && raw <= 0xDF)
    {
        Model2ProcessCommand(raw, M2CMD_LEFT, (raw - 191) / 32.0);
    }
    else if (raw >= 0x80 && raw <= 0x9F)
    {
        Model2ProcessCommand(raw, M2CMD_RIGHT, (raw - 127) / 32.0);
    }
    else
    {
        Model2ProcessCommand(raw, M2CMD_NONE, 0.0);
    }
}

static void Model2ModernObserveReadByte(INT_PTR offset, bool isRelativeOffset, UINT8 value)
{
    if (!Model2ModernEnabled())
        return;

    if (isRelativeOffset && offset == 0x174CF4)
    {
        Model2ProcessSegaRally(value);
        return;
    }

    if ((!isRelativeOffset && offset == 0x0057285B) ||
        (isRelativeOffset && offset == 0x17285B))
    {
        Model2ProcessGeneric(value);
    }
}

// Trigger wrappers: Model 2 Modern FFB owns these effects. Everything else is
// forwarded exactly as upstream did.
EffectConstantTriggerSlot& EffectConstantTriggerSlot::operator=(EffectConstantFunction value)
{
    function = value;
    model2LegacyConstantFunction = value;
    return *this;
}

void EffectConstantTriggerSlot::operator()(int direction, double strength) const
{
    if (Model2ModernEnabled())
        return;
    if (function != NULL)
        function(direction, strength);
}

EffectSpringTriggerSlot& EffectSpringTriggerSlot::operator=(EffectSingleStrengthFunction value)
{
    function = value;
    model2LegacySpringFunction = value;
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
    model2LegacyFrictionFunction = value;
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
    model2LegacySineFunction = value;
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
    model2LegacyRumbleFunction = value;
    return *this;
}

void EffectRumbleTriggerSlot::operator()(double lowfrequency, double highfrequency, double length) const
{
    if (Model2ModernEnabled())
        return;
    if (function != NULL)
        function(lowfrequency, highfrequency, length);
}

EffectConditionTriggerSlot& EffectConditionTriggerSlot::operator=(EffectSingleStrengthFunction value)
{
    function = value;
    // Springi and Damper both use this wrapper type. Store the first assignment
    // only for diagnostics/fallback parity; forwarding uses the slot itself.
    if (model2LegacySpringInfiniteFunction == NULL)
        model2LegacySpringInfiniteFunction = value;
    else
        model2LegacyDamperFunction = value;
    return *this;
}

void EffectConditionTriggerSlot::operator()(double strength) const
{
    if (Model2ModernEnabled())
        return;
    if (function != NULL)
        function(strength);
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

    if (len < 0)
        len = static_cast<int>(sizeof(buffer)) - 2;
    else if (len > static_cast<int>(sizeof(buffer)) - 2)
        len = static_cast<int>(sizeof(buffer)) - 2;
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
    LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
    ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(int), &read);
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
    LPVOID trueOffset = (isRelativeOffset ? GetTranslatedOffset(offset) : (LPVOID)offset);
    ReadProcessMemory(GetCurrentProcess(), trueOffset, &val, sizeof(float), &read);
    return val;
};

void Game::FFBLoop(EffectConstants * constants, Helpers * helpers, EffectTriggers * triggers)
{
    return;
}
