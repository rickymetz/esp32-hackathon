idf_component_register(
    SRCS "lv_font_lexend_24.c" "lv_font_lexend_26.c" "lv_font_lexend_32.c"
         "lv_font_lexend_40.c" "lv_font_lexend_48.c" "lv_font_lexend_60.c"
         # Icon fallback faces (FontAwesome glyphs) each Lexend face points at.
         "lv_font_icons_24.c" "lv_font_icons_26.c" "lv_font_icons_32.c"
         "lv_font_icons_40.c" "lv_font_icons_48.c" "lv_font_icons_60.c"
         "lv_font_lexend_72.c" "lv_font_lexend_120.c"
         "lv_font_icons_72.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
)
