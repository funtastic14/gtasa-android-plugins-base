# VLS Matrix Textures

Custom matrix textures use ASCII PPM P3 files (RGB, no alpha), maximum dimensions 128x128.

Install as `VehicleLEDSystem/textures/<texture-name>.ppm`, then select Running Text and enter the filename without extension in **Texture Name**. Names may contain letters, digits, underscore, and hyphen, up to 32 characters.

The texture is scaled to the LED matrix and colors only pixels that are already lit by the selected text motion/font. It does not change text geometry or movement. The renderer caches the decoded bitmap; it does not read files per pixel.

`demo.ppm` is an included 24x7 rainbow sample.
