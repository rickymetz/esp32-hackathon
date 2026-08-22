idf_component_register(
    SRCS "app_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES cap_lua espressif__lua
    PRIV_REQUIRES esp32_s3_touch_amoled_1_8 esp_driver_i2c
)
