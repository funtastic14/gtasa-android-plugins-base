# Layered texture

The PNGs are separate transparent layers on the same 96x32 canvas:

- `base.png`: eye body / lower crescent
- `brows.png`: eyebrows
- `pupils.png`: iris and pupil details

The intended compositor draws the base first, then brows, then pupils. Independent offsets and motion can be applied to each layer. This is a planned format, not yet supported by the current runtime.
