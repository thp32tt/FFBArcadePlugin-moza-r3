# Model 2 Modern FFB (MOZA / DD wheel fork)

This fork keeps the original FFB Arcade Plugin behavior available while adding an opt-in-compatible modern processing path for Sega Model 2 Emulator driving games. The goal is to preserve the arcade FFB commands and make their delivery feel cleaner on modern direct-drive wheels such as the MOZA R3.

## Current scope

The first implementation targets the Model 2 games already supported by the upstream plugin: Sega Rally Championship variants, Daytona USA variants, Indianapolis 500, Sega Touring Car Championship, Over Rev, and Super GT 24h.

The implementation does **not** invent vehicle physics or use unverified vehicle-memory addresses. It modernizes the existing arcade FFB command stream first. Dynamic SAT based on verified steering/speed/slip telemetry is intentionally left for a later phase.

## What v1 changes

- Keeps processing limited to upstream `GameId=25` (M2 Emulator).
- Reads the same Model 2 FFB bytes already used by the upstream plugin.
- Applies separate gains for constant, spring, friction, sine, and rumble effects.
- Adds a configurable response curve (`Gamma`).
- Smooths signed constant force so left/right reversals pass through a controlled transition instead of snapping instantly.
- Refreshes the output on each observed FFB byte read and releases stale constant force when the command changes to a non-constant effect.
- Clears stale non-constant effects before applying the next arcade command.
- Leaves the upstream `Springi` and damper options untouched.

## Settings

Add any of these keys to the `[Settings]` section of the Model 2 `FFBPlugin.ini`. All keys are optional; the values below are the fork defaults.

```ini
Model2ModernFFB=1
Model2ModernFFBConstantGain=100
Model2ModernFFBSpringGain=45
Model2ModernFFBFrictionGain=35
Model2ModernFFBSineGain=30
Model2ModernFFBRumbleGain=20
Model2ModernFFBGamma=85
Model2ModernFFBSmoothing=35
```

`Model2ModernFFB=0` restores the original upstream effect dispatch path.

Gain values are percentages. The implementation clamps individual gains internally to a safe 0-200% processing range and clamps the resulting normalized effect strength to 0-100% before the existing plugin-wide MinForce/MaxForce scaling is applied.

`Model2ModernFFBGamma=100` is linear. Values below 100 make low/mid-strength arcade commands more prominent; the default 85 is intended as a conservative DD-wheel starting point.

`Model2ModernFFBSmoothing=0` gives the fastest constant-force response. Higher values soften force transitions; the default 35 is a starting point for MOZA R3 testing rather than a final universal tune.

## Compatibility / fallback

The routing layer preserves the existing `EffectTriggers` call syntax and forwards effects unchanged for every non-Model-2 `GameId`. When Modern FFB is disabled, Model 2 calls are also forwarded to the original upstream functions unchanged.

## License

This repository remains licensed under GNU GPL v3 in accordance with the upstream project. Existing copyright and license notices are retained. Modified behavior is documented in this file and in source comments.
