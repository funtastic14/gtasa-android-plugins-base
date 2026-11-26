# Strobo Eyes Texture Assets

Strobo Eyes now uses texture artwork instead of procedural iris, expression, eyebrow, or motif drawing. The runtime supports one complete RGBA PNG or P3 PPM texture per effect. The three shape options (Original/Default/Normal, Round, Sleepy) act only as a geometric mask over the selected artwork.

## Static PNG

- Source example: `static/eyes.png`
- Canvas: 96x32 RGBA.
- Transparent pixels remain unlit.
- Copy the file to the device as `VehicleLEDSystem/textures/eyes.png`.
- In the Strobo Eyes controls, enter `eyes` in **Custom Eye Texture (PNG/PPM)** (without the extension).

The PNG loader preserves alpha and scales the complete two-eye image across the matrix. Runtime image dimensions are limited to 128x128 to keep memory use bounded on Android devices.

## Layered and frame examples

- `layered/base.png`, `brows.png`, `pupils.png`
- `frames/eyes_000.png` through `eyes_003.png`

These remain source assets/reference examples only. Independent layer motion/compositing and frame-sequence playback are not implemented yet; do not expect those folders to be loaded automatically.

Keep transparent areas truly transparent. Avoid flattening the PNG onto a black background, because black opaque pixels are still part of the artwork.
