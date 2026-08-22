# Extended icon fonts (`lv_font_icons_*`)

The `lv_font_lexend_*` faces carry the text glyphs plus LVGL's built-in
`LV_SYMBOL_*` FontAwesome range. Apps often want more glyphs (a microphone, a
clock, a heart…), so an **extended icon pack** ships alongside them as a
separate face per size — `lv_font_icons_24/26/32/40/48.c` — wired in as each
Lexend face's **fallback**:

```c
// in lv_font_lexend_32.c
extern const lv_font_t lv_font_icons_32;
...
    .fallback = &lv_font_icons_32,
```

LVGL resolves a missing glyph through `.fallback`, so a label using
`lvgl.font(32)` renders these icons with no per-app work. Keeping the icons in a
separate face means the Lexend text bitmaps stay **byte-identical** when the
icon set changes — only the two-line fallback wiring above lives in the Lexend
files, and re-running `lv_font_conv` for the Lexend faces must re-apply it.

The glyphs are exposed to apps as `lvgl.symbol.*` (see
`components/lua_module_lvgl/src/lua_module_lvgl.c`) and listed in
`docs/APP_CONTRACT.md`.

## Regenerate / add an icon

The icon faces are made from LVGL's bundled FontAwesome 5 with `lv_font_conv`
(`npm i lv_font_conv`). `FA` is
`managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff`
(same file under the LVGL clone). `CODEPOINTS` is the decimal FontAwesome list:

```
61442,61744,61463,61555,61444,61445,61829,61830,62153,62194,62405,61447,61488,61549,61528,61557,61634,61982
# search, microphone, clock, calendar, heart, star, sun, moon, thermometer,
# stopwatch, location(map-marker-alt), user, camera, fire, check-circle,
# comment, cloud, heartbeat
```

```bash
for sz in 24 26 32 40 48 60; do
  lv_font_conv --bpp 4 --size $sz --font "$FA" -r $CODEPOINTS \
    --format lvgl --no-compress --no-prefilter --force-fast-kern-format \
    --lv-include lvgl.h -o lv_font_icons_$sz.c --lv-font-name lv_font_icons_$sz
done
```

To add a glyph: find its FontAwesome 5 codepoint, append the decimal value to
`CODEPOINTS`, regenerate, add a `lvgl.symbol.<name>` entry in the binding, and
document it in the contract. `--no-compress` is required (the launcher builds
with `LV_USE_FONT_COMPRESSED` off).
