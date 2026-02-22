# Hue Integration TODO

This document tracks Hue-facing behaviors that are missing, partial, or intentionally limited in the current firmware.

## 1) Full Color Control Exposure (RGB/Hue/Sat)

Status: Not implemented (currently CT-only exposure).

Current code:
- `main/esp_zb_light.c`: endpoint is configured as color-temperature-focused (`color_capabilities = (1 << 4)` and color mode `0x02`).

TODO:
- Decide product direction:
  - Keep CT-only for WWA hardware (recommended if the device should not pretend to be RGB-capable), or
  - Expose extended color and implement full color attribute behavior.
- If CT-only is intentional, document this as a deliberate Hue limitation in `README.md`.

Acceptance:
- Hue app capabilities match real hardware behavior (no misleading controls).

## 2) Hue Effects / Entertainment-Style Dynamic Features

Status: Not implemented.

Current code:
- `main/esp_zb_light.c`: only standard set-attribute flow is handled for On/Off, Level, and Color Control attributes.

TODO:
- Explicitly decide whether to support dynamic Hue effect/entertainment features.
- If unsupported, document as out-of-scope.
- If supported, add protocol/cluster handling and a non-blocking renderer path.

Acceptance:
- Feature is either implemented end-to-end, or clearly documented as unsupported.

## 3) Transition-Time Semantics (Command-Driven Durations)

Status: Partial / custom only.

Current code:
- `light_driver/src/light_driver.c`: fixed local fade timing (`POWER_ON_FADE_DURATION_MS = 1000`, `POWER_ON_FADE_FRAMES = 20`).
- `main/esp_zb_light.c`: attribute handlers apply state directly; no command-specific transition-duration handling in app logic.

TODO:
- Support transition durations requested by Hue/Zigbee commands (level/color temp transitions).
- Keep fixed 1s fade as fallback when no transition time is provided.
- Ensure command transition time overrides local default when present.

Acceptance:
- Hue-requested transition durations are visibly honored.

## 4) Hue UI State Sync Reliability

Status: Partial, can desync in edge cases.

Current code:
- `main/esp_zb_light.c`: local control paths attempt Zigbee attribute writes only if `esp_zb_lock_acquire(...)` succeeds, but still update LEDs locally.
- This can allow physical output and Hue UI state to diverge.

TODO:
- Create a single state-apply path that guarantees:
  - Zigbee attribute update success (or retry/fail handling), and
  - driver update,
  - plus explicit sync/report on recovery.
- Avoid updating local output when Zigbee-side state update fails, unless a recovery sync is scheduled.

Acceptance:
- Hue app state and actual LED output remain consistent after button actions and network jitter.

## 5) Startup Behavior (Hue Power-On Behavior Semantics)

Status: Partial.

Current code:
- `main/esp_zb_light.c`: startup CT attribute is added, but boot behavior is primarily custom NVS restore (`deferred_driver_init()` applying persisted power/level/ct).

TODO:
- Implement clear startup precedence compatible with Hue expectations:
  - startup attributes (On/Off, Level, CT) when explicitly configured,
  - previous-state fallback where Zigbee spec indicates “use previous”,
  - NVS restore only as compatible fallback.
- Ensure boot-applied state is reflected back to Zigbee attributes so Hue UI matches after reboot.

Acceptance:
- Hue “Power-on behavior” settings produce predictable on-device startup behavior.

## Recommended Execution Order

1. State sync reliability (Item 4)
2. Startup behavior semantics (Item 5)
3. Transition-time semantics (Item 3)
4. Capability direction decision and docs (Item 1)
5. Effects/entertainment scope decision (Item 2)
