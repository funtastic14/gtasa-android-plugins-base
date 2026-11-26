# VLS Matrix Textures

Custom matrix textures support transparent RGBA PNG files and ASCII PPM P3 files, with maximum dimensions of 128x128.

Install as `VehicleLEDSystem/textures/<texture-name>.png` or `.ppm`, then enter the filename without extension in the effect's texture field. Names may contain letters, digits, underscore and hyphen, up to 32 characters.

PNG is loaded first and preserves its alpha channel. PPM P3 is supported as an opaque RGB fallback. The renderer scales the bitmap to the LED matrix and samples it only for pixels already enabled by the selected effect. Decoded images are cached; files are not read during per-pixel rendering.

The included `demo.ppm` remains a 24x7 rainbow sample. Strobo Eyes can use a complete two-eye transparent PNG as its artwork; see `eyes/README.md`.
