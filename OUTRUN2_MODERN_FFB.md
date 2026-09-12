# OutRun 2 Modern FFB (GameId 12)

This branch modernizes the existing **Outrun 2 Special Tours Deluxe Custom** implementation without changing its GameId or memory map. Existing TeknoParrot/AutoSetup configurations that use GameId 12 continue to select `OutRun2Fake`, but that class now runs the Modern FFB mixer.

## What changed

- Continuous speed-sensitive SAT-like steering torque instead of the old 10/20/30/... stepped speed table.
- Steering-velocity based dynamic damping to reduce DD-wheel oscillation.
- Small center deadzone and nonlinear steering response.
- Directional collision force is mixed on top of the steering torque instead of simply replacing it.
- Existing road/grass/surface periodic effects are retained and independently scalable.
- Gear-change kick from the Real implementation is added to the Custom path.
- Optional telemetry exposes the previously unused `ffwall` and `ff7` values for later reverse-engineering.
- Main loop remains at the existing ~62.5 Hz (`Sleep(16)`) cadence for compatibility.

> The current centering force is **SAT-like**, not true tire-physics SAT. It is derived from steering displacement and vehicle speed. The telemetry option is intended to help identify additional vehicle-state values (yaw/lateral/slip-related data) for a later physics-informed version.

## Default tuning

No new keys are required. If these values are absent, the code uses the defaults shown below.

Add any values you want to override under `[Settings]` in the generated `FFBPlugin.ini`:

```ini
ModernFFBEnable=1
ModernSATStrength=72
ModernSATMinStrength=8
ModernSATSteeringExponent=85
ModernSATSpeedExponent=75
ModernSpeedReference=320
ModernDynamicDamping=22
ModernDampingVelocityReference=600
ModernTorqueResponse=42
ModernCenterDeadzone=2
ModernCollisionStrength=65
ModernRoadStrength=55
ModernGearKickStrength=12
ModernDebugTelemetry=0
```

## Meaning of the controls

- `ModernSATStrength` - maximum steering self-centering/SAT-like force, percent.
- `ModernSATMinStrength` - low-speed baseline centering force, percent.
- `ModernSATSteeringExponent` - steering-angle curve exponent x100. Lower values give more force near center; 100 is approximately linear.
- `ModernSATSpeedExponent` - speed curve exponent x100. Lower values build steering weight earlier.
- `ModernSpeedReference` - speed value at which speed scaling reaches 100%. Default 320 follows the useful range observed in the old OutRun2 implementation.
- `ModernDynamicDamping` - maximum synthetic steering-velocity damping, percent.
- `ModernDampingVelocityReference` - full-scale wheel velocity reference x100. Default 600 means 6.0 normalized wheel-travel units per second.
- `ModernTorqueResponse` - low-pass response percentage per update. Higher = faster/sharper; lower = smoother.
- `ModernCenterDeadzone` - steering deadzone around center, percent.
- `ModernCollisionStrength` - directional collision impulse strength, percent.
- `ModernRoadStrength` - road/grass/surface sine/rumble multiplier, percent.
- `ModernGearKickStrength` - gear-change kick, percent.
- `ModernDebugTelemetry` - set to `1` to write an OutRun2 Modern FFB telemetry line about four times per second when plugin logging is enabled.

The existing settings remain valid:

```ini
EnableDamper=0
DamperStrength=100
```

`EnableDamper=1` adds the plugin/device native damper on top of Modern Dynamic Damping. For a MOZA R3, start with the native damper disabled and tune `ModernDynamicDamping` first so it is clear which layer is producing resistance.

## MOZA R3 first-test preset

Use the defaults first. If the wheel still feels too light in long corners, try:

```ini
ModernSATStrength=82
ModernSATMinStrength=10
ModernSATSteeringExponent=80
ModernDynamicDamping=24
```

If the wheel oscillates on straights, increase `ModernDynamicDamping` in steps of 3-5. If it feels sluggish during counter-steer, reduce it. If steering feels too heavy around center but fine at large angles, raise `ModernSATSteeringExponent` toward 100-110.

## Test order

1. Confirm direction at low speed: turn left and the wheel must pull right toward center; turn right and it must pull left.
2. Confirm speed build-up: steering weight should increase smoothly with speed, with no abrupt force jumps at fixed speed thresholds.
3. On a straight, briefly disturb the wheel and confirm damping reduces oscillation without making the wheel sticky.
4. Hit left/right walls and verify the impulse direction feels correct. If an arcade event is direction-inverted, record which side/event triggered it before changing global FFB direction.
5. Drive grass/rough sections and verify road effects are distinct from the continuous steering force.
6. Enable `Logging=1` and `ModernDebugTelemetry=1` only for data collection; inspect `ffwall`/`ff7` changes during cornering, drift, collisions and surface transitions.

## Next reverse-engineering target

For v2, collect telemetry while performing repeatable maneuvers:

- steady left/right corner at constant speed,
- lift-off and throttle-on mid-corner,
- normal corner vs drift,
- transition road -> grass -> road,
- left/right wall contact.

If `ffwall`, `ff7`, or another nearby memory value correlates with yaw/lateral acceleration/slip rather than steering input, it can be blended into the base torque to make the force react to vehicle state rather than acting mainly as a speed-sensitive spring.
