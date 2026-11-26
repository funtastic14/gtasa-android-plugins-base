# Strobo Eyes Texture Assets

Strobo Eyes supports both Procedural Eye and Custom Eye. With no texture selected, VLS generates the eye using independent sclera, iris, and pupil colors. With a texture selected, the runtime uses one complete RGBA PNG or P3 PPM as source artwork. The three shape options (Original/Default/Normal, Round, Sleepy) act as geometric masks.

## Static PNG

- Source example: `static/eyes.png`
- Canvas: 96x32 RGBA.
- Transparent pixels remain unlit.
- Copy the file to the device as `VehicleLEDSystem/textures/eyes.png`.
- In the Strobo Eyes controls, enter `eyes` in **Custom Eye Texture (PNG/PPM)** (without the extension).

The PNG loader preserves alpha and scales the complete two-eye image across the matrix. The mapper treats the left and right halves as separate eye regions and uses bilinear UV sampling for gaze and blink. In the editor, open **Custom Eye Mapping** to set normalized iris and pupil centers/radii; VLS remaps those regions while keeping the surrounding eye artwork in place. The mapping values are saved in the vehicle profile, so no sidecar file or extra animation frames are required. Runtime image dimensions are limited to 128x128 to keep memory use bounded on Android devices. The default mapping is centered; adjust it if the iris/pupil in your artwork is offset.

## Layered and frame examples

- `layered/base.png`, `brows.png`, `pupils.png`
- `frames/eyes_000.png` through `eyes_003.png`

These remain source assets/reference examples only. Independent layer motion/compositing, metadata-driven iris/pupil segmentation, and frame-sequence playback are not implemented yet; do not expect those folders to be loaded automatically.

Keep transparent areas truly transparent. Avoid flattening the PNG onto a black background, because black opaque pixels are still part of the artwork.
