# VLS Custom Bitmap Fonts

Font assets use the `.vlsfont` format: 95 printable ASCII characters (code points 32 through 126), in ascending character order.

Each character occupies one line containing 14 hexadecimal digits: seven 5-bit rows, two hex digits per row. Only values 00 through 1F are valid. Whitespace and line breaks are ignored by the loader.

Install a font as:

`VehicleLEDSystem/fonts/<font-name>.vlsfont`

Then select Running Text and enter the filename without the extension in **Font Name**. Font names may contain letters, digits, underscore, and hyphen, up to 32 characters.

`default.vlsfont` is the included starter bitmap font. Missing or invalid custom files fall back to the built-in font. The renderer caches the selected font and does not read the file per LED pixel.
