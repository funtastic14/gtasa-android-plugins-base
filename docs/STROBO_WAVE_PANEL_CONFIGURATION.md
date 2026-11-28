# Strobo Wave Panel Stack

Strobo Wave uses a dedicated vertical stack of 3, 5, or 7 sub-panels. Each sub-panel has an enable switch and phase offset. Pattern, direction, and density behavior depends on the selected composition mode. The complete stack is rendered as one matrix/shared canvas.

The in-game editor exposes **Strobo Panel Count**, panel width/height, physical gap rows, flash period, **Panel Motion Composition**, and **Configure Panel**. Select a panel there to edit its individual settings.

## Default layout

- 5 panels
- 21 columns × 7 rows per panel
- 2 blank rows between panels
- Total matrix: 21 × 43
- Default directions: panel 1 left→right, panel 2 right→left, panel 3 top→bottom, panel 4 right→left, panel 5 left→right

Placement dimensions are still controlled by the LED placement transform. The matrix grid and panel gap determine how that placement is divided into visible panel regions.

## JSON fields

```json
{
  "stroboPanelStackEnabled": true,
  "stroboPanelCount": 5,
  "stroboPanelColumns": 21,
  "stroboPanelRows": 7,
  "stroboPanelGapRows": 2,
  "stroboCompositionMode": 0,
  "stroboFlashEnabled": false,
  "stroboFlashPeriodMs": 500,
  "stroboPanels": [
    { "enabled": true, "motionPattern": 0, "direction": 0, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.0 },
    { "enabled": true, "motionPattern": 0, "direction": 1, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.5 },
    { "enabled": true, "motionPattern": 0, "direction": 2, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.25 },
    { "enabled": true, "motionPattern": 0, "direction": 1, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.75 },
    { "enabled": true, "motionPattern": 0, "direction": 0, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.0 },
    { "enabled": true, "motionPattern": 0, "direction": 3, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.5 },
    { "enabled": true, "motionPattern": 0, "direction": 0, "densityMode": 0, "densityX": 1.0, "densityY": 1.0, "phaseOffset": 0.25 }
  ]
}
```

- `stroboPanelCount`: 3, 5, or 7.
- `stroboPanelColumns`: columns per panel; e.g. 21.
- `stroboPanelRows`: rows per panel; e.g. 7.
- `stroboPanelGapRows`: blank matrix rows between panels, 0–16.
- `stroboCompositionMode`: 0 Unified Geometry, 1 Coordinated Motion, 2 Independent Motion.
- `stroboFlashEnabled`: optional hard flash overlay; false by default.
- `stroboFlashPeriodMs`: strobe cycle, 100–5000 ms.
- `stroboPanels`: fixed seven-entry array; only the first selected panel count is rendered.

## Panel motion composition

| Value | Mode | Behavior |
|---:|---|---|
| 0 | Unified Geometry | Every panel samples the main effect motion pattern and one shared phase in a global coordinate field. Radial geometry is aspect-corrected for the full stack. Per-panel pattern, direction, density, and phase offset are bypassed to prevent seams from breaking a spiral or radial front. |
| 1 | Coordinated Motion | Panels may use different patterns. Radial, spiral, orbit, circle, burst, and related center-based patterns share the full stack's global coordinates, center, and phase. Directional patterns retain their local panel direction, density, and phase offset. |
| 2 | Independent Motion | Each panel uses its own local pattern, direction, and density without requiring continuity with neighboring panels. |

New profiles default to Unified Geometry. Older profiles without this field migrate to Coordinated Motion so their saved per-panel pattern and direction choices continue to work.

## Density modes

| Value | Mode | Behavior |
|---:|---|---|
| 0 | Adaptive | Pattern-specific density; detail patterns scale while broad sweeps retain spacing. |
| 1 | Fixed | Keep base density regardless of panel resolution. |
| 2 | Scalable | Scale density relative to 21 × 7. |
| 3 | Custom | Use `densityX` and `densityY` (0.25–4.0). |

## Direction values

| Value | Direction |
|---:|---|
| 0 | Left → Right |
| 1 | Right → Left |
| 2 | Top → Bottom |
| 3 | Bottom → Top |
| 4 | Diagonal Down |
| 5 | Diagonal Up |
| 6 | Center Out |
| 7 | Edges to Center |

Motion pattern values follow the stable `LedMotionPattern` enum order in `src/vehicle_profile.h`. On multi-row matrix canvases, **Center Out** and **Edges to Center** use a true two-dimensional radial distance; one-row strips retain their left/right center sweep.

## Compatibility and performance

Legacy Strobo Wave profiles without a composition setting retain their previous per-panel behavior through Coordinated Motion. Existing customized panel arrays are preserved; old arrays that still contain the original seven identical untouched defaults are migrated to the distinct direction layout. Global geometry uses fixed-size scalar calculations and does not allocate on every rendered frame.
