# Strobo Wave Panel Stack

The dedicated panel stack is opt-in so older Strobo Wave profiles keep their original behavior.

Add these fields inside a Strobo Wave effect object:

```json
{
  "stroboPanelStackEnabled": true,
  "stroboPanelCount": 5,
  "stroboPanelColumns": 21,
  "stroboPanelRows": 7,
  "stroboPanelGapRows": 1,
  "stroboFlashPeriodMs": 500,
  "stroboPanels": [
    { "enabled": true, "motionPattern": 0, "direction": 0, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.0 },
    { "enabled": true, "motionPattern": 1, "direction": 1, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.0 },
    { "enabled": true, "motionPattern": 2, "direction": 2, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.0 },
    { "enabled": true, "motionPattern": 1, "direction": 1, "densityMode": 1, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.25 },
    { "enabled": true, "motionPattern": 0, "direction": 0, "densityMode": 3, "densityX": 1.5, "densityY": 1.0, "phaseOffset": 0.5 },
    { "enabled": true, "motionPattern": 0, "direction": 0, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.0 },
    { "enabled": true, "motionPattern": 0, "direction": 0, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.0 }
  ]
}
```

## Values

- `stroboPanelCount`: 3, 5, or 7.
- `stroboPanelColumns`: columns per panel; example 21.
- `stroboPanelRows`: rows per panel; example 7.
- `stroboPanelGapRows`: blank matrix rows between panels, 0–16.
- `stroboFlashPeriodMs`: strobe cycle, 100–5000 ms.
- `stroboPanels`: fixed seven-entry array; only the first selected panel count is rendered.

### Density mode

| Value | Mode | Behavior |
|---:|---|---|
| 0 | Adaptive | Pattern-specific density; detail patterns scale while broad sweeps retain space. |
| 1 | Fixed | Keep the base density regardless of panel resolution. |
| 2 | Scalable | Scale density with panel dimensions relative to 21 × 7. |
| 3 | Custom | Use `densityX` and `densityY` (0.25–4.0). |

### Direction values

| Value | Direction |
|---:|---|
| 0 | Left → Right |
| 1 | Right → Left |
| 2 | Top → Bottom |
| 3 | Bottom → Top |
| 4 | Diagonal Down |
| 5 | Diagonal Up |
| 6 | Center Out |
| 7 | Edge To Center |

### Motion pattern values

The `motionPattern` integer follows the stable `LedMotionPattern` enum order in `src/vehicle_profile.h`. For example: 0 = LeftToRight, 1 = RightToLeft, 2 = CenterOut, 3 = EdgeToCenter, 10 = Wave, 11 = Diagonal.

## Canvas and geometry

The loader automatically enables and sizes the matrix when `stroboPanelStackEnabled` is true and no conflicting per-pixel matrix is supplied. Five panels of 21 × 7 with one blank row between panels produce a 21 × 39 logical grid. Panel gaps are excluded from the shared motion coordinate space, so waves and directional movement remain continuous across the stack.

Legacy profiles without `stroboPanelStackEnabled` are not converted automatically. The renderer uses fixed-size panel configuration storage and performs no per-frame heap allocation for panel state.
