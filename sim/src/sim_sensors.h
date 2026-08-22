#pragma once
#include "esp_err.h"

/* Registers desktop stand-ins for rtc, battery, imu, audio and wifi. */
esp_err_t sim_sensors_register(void);
