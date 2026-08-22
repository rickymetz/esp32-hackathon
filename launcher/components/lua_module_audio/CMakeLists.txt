idf_component_register(
    SRCS "app_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES cap_lua espressif__lua esp_timer lua_module_lvgl
    PRIV_REQUIRES esp32_s3_touch_amoled_1_8 espressif__esp_codec_dev
)
