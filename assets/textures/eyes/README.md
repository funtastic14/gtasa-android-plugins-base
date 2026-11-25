# Strobo Eyes PNG Examples

These are source examples for the planned PNG eye-texture system. **The current in-game texture loader still accepts PPM P3 only; PNG examples are not yet selectable in-game.** PNG runtime support and full texture replacement must be implemented before these files can be installed as working assets.

All examples use a 96x32 RGBA canvas with both eyes in one image. Transparent pixels are intentional and must remain transparent.

## Folders

- `static/eyes.png`: one complete static eye design.
- `layered/base.png`, `brows.png`, `pupils.png`: separated transparent layers, intended for independent movement.
- `frames/eyes_000.png` ... `eyes_003.png`: complete frames for a simple animation sequence.

## Planned device locations

The plugin will use the AML internal storage directory:

`VehicleLEDSystem/textures/eyes/`

The examples in this repository are design references. Copying them to the device will not make them work until PNG decoding, RGBA sampling, and the new texture mode are integrated.

## Planned behavior

- Static: use one full-canvas PNG; all eye parts move together.
- Layered: composite base, brows, and pupils independently; each layer can have its own offset/motion.
- Frames: play numbered full-canvas PNGs in order with a configurable frame duration and loop setting.

Keep the canvas dimensions consistent across layers and frames. Use transparent RGBA PNGs; avoid huge images. The first runtime implementation will impose conservative dimension and memory limits for Android devices.
