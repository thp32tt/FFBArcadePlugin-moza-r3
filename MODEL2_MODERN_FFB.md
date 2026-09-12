# Model 2 Modern FFB — Generic DD v2

This branch adds a manufacturer-neutral modern FFB path for Sega Model 2 Emulator driving games. The implementation does not contain MOZA/R3/Fanatec/Simagic/Simucube/Logitech-specific detection or tuning logic. A wheel is handled according to the haptic effects it actually reports through SDL and the user's selected compatibility mode.

## Scope

Supported Model 2 titles are the games already handled by upstream FFB Arcade Plugin: Sega Rally Championship variants, Daytona USA variants, Indianapolis 500, Sega Touring Car Championship, Over Rev, and Super GT 24h.

The default mode still uses the original Model 2 arcade FFB command stream. It does **not** invent vehicle physics or read unverified speed/yaw/slip addresses. A future `Arcade + Physics SAT` mode should only be added after those vehicle-state addresses are validated.

## v2 architecture

The v2 path adds the generic DD lessons learned during the OutRun wheel-FFB work without importing any wheel-brand compatibility layer:

- one Model 2 effect owner/state machine;
- explicit stopping of stale Constant/Spring/Friction/Sine effects on command changes;
- finite effect leases so torque expires even if command refresh stops unexpectedly;
- foreground-window safety: FFB is released on focus loss and resumes after a short warmup;
- separate force build rate and direction-reversal release rate;
- soft saturation that preserves low/mid-range detail and bends only the top of the range;
- independent Constant and Spring inversion controls;
- SDL capability-based Native/ConstantPulse periodic output selection;
- game-specific profile sections for Sega Rally, Daytona, Indy 500, STCC, Over Rev and Super GT;
- optional raw-command/output telemetry;
- upstream legacy path remains available with `Model2ModernFFB=0`.

The legacy `Springi` and Damper calls are suppressed while v2 owns Model 2 FFB. This avoids a permanent artificial center spring or damper fighting the arcade command stream. Disable Modern FFB if the original behavior is required.

## Generic settings

The build package contains these defaults:

```ini
[Settings]
Model2ModernFFB=1

[Model2Modern]
Enabled=1
ConstantGain=100
SpringGain=45
FrictionGain=35
PeriodicGain=30
RumbleGain=20
Gamma=100
BuildRate=18
ReversalReleaseRate=40
SoftClipKnee=75
SoftClipLimit=135
InvertConstant=0
InvertSpring=0
PeriodicMode=0
ConditionMode=0
FocusSafety=1
CommandLeaseMs=180
Telemetry=0
```

`PeriodicMode` values:

- `0` = Auto: use native Sine when the wheel reports it, otherwise use alternating ConstantForce pulses.
- `1` = Native: prefer the native periodic path when supported.
- `2` = ConstantPulse: use the generic ConstantForce pulse fallback regardless of wheel brand.
- `3` = Off.

`ConditionMode` values:

- `0` = Auto / native condition effects when supported.
- `1` = Native condition effects.
- `2` = Off.

`BuildRate` controls how quickly normal structural ConstantForce is allowed to grow per ~60 Hz Model 2 update. `ReversalReleaseRate` controls how quickly stale torque is released toward zero before force is built in the opposite direction. Keeping these separate reduces counter-steer latency without making ordinary corner load unnaturally abrupt.

`SoftClipKnee=75` and `SoftClipLimit=135` keep normal output linear to roughly 75%, then progressively compress headroom above that toward the device limit instead of hard-clipping all high forces.

`InvertConstant` reverses directional ConstantForce only. `InvertSpring` reverses the native condition spring direction only. They are intentionally independent because some wheel/driver combinations disagree only on one effect class.

## Per-game profiles

Each profile inherits the generic section and can override the main feel controls:

```ini
[Model2Modern.SegaRally]
ConstantGain=100

[Model2Modern.Daytona]
ConstantGain=100

[Model2Modern.Indy500]
ConstantGain=100

[Model2Modern.STCC]
ConstantGain=100

[Model2Modern.OverRev]
ConstantGain=100

[Model2Modern.SuperGT]
ConstantGain=100
```

Per-game sections may also override `SpringGain`, `FrictionGain`, `PeriodicGain`, `RumbleGain`, `Gamma`, `BuildRate`, `ReversalReleaseRate`, `InvertConstant`, and `InvertSpring`.

## Command decoder safety fix

The upstream generic Spring range includes raw bytes `0x0A..0x17` while its historical strength expression `(ff - 15) / 8.0` becomes negative for `0x0A..0x0E`. The old condition backend converts a negative coefficient into full-scale positive spring force. v2 preserves the upstream raw-byte range but clamps those negative magnitudes to zero so malformed/low command values cannot become an unintended maximum spring.

## Telemetry

Set `Telemetry=1` to create `Model2ModernFFB.log`. It records the selected game profile, raw FFB byte, decoded command, decoded strength and current smoothed ConstantForce state. SDL haptic capability flags are also reported through the debug output path.

This is intended for hardware reports from any DD wheel. Useful reports should include wheel/base model, driver/firmware, game, relevant INI profile and the telemetry log.

## Compatibility and fallback

Non-Model-2 games continue through the original FFB Arcade Plugin effect functions unchanged. Setting `Model2ModernFFB=0` restores the original Model 2 dispatch path as well.

## License

The repository remains GNU GPL v3 in accordance with upstream. Existing copyright and license notices are retained.
