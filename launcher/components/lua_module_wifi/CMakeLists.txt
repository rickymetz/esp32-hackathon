idf_component_register(
    SRCS "app_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES cap_lua espressif__lua lua_module_sensors
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip esp_timer
)
