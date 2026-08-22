#include <string.h>
#include "app_sensors.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "lauxlib.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"

static const char *TAG = "app_sensors";

/* I2C addresses, from CLAUDE.md's pin map. */
#define ADDR_PCF85063  0x51
#define ADDR_QMI8658   0x6B
#define ADDR_AXP2101   0x34

#define I2C_HZ         400000
#define I2C_TIMEOUT_MS 100

/* ---- PCF85063A registers (datasheet 8.3) ---- */
#define PCF_CTRL1      0x00
#define PCF_SECONDS    0x04   /* seconds..years run 0x04-0x0A */
#define PCF_SEC_VL     0x80   /* bit 7 of seconds: clock integrity lost */

/* ---- QMI8658 registers ---- */
#define QMI_WHO_AM_I   0x00   /* == 0x05 */
#define QMI_CTRL1      0x02
#define QMI_CTRL2      0x03   /* accel: range + ODR */
#define QMI_CTRL3      0x04   /* gyro:  range + ODR */
#define QMI_CTRL7      0x08   /* enable accel|gyro */
#define QMI_TEMP_L     0x33
#define QMI_AX_L       0x35   /* ax,ay,az,gx,gy,gz little-endian int16 */

/* ---- AXP2101 registers ---- */
#define AXP_COMM_STAT0 0x00   /* bit 5: VBUS present */
#define AXP_COMM_STAT1 0x01   /* bits 6:5 charge state, 01 = charging */
#define AXP_VBAT_H     0x34   /* 14-bit battery voltage, mV */
#define AXP_BAT_PCT    0xA4   /* fuel gauge percentage */

static i2c_master_dev_handle_t s_rtc_dev;
static i2c_master_dev_handle_t s_imu_dev;
static i2c_master_dev_handle_t s_pmu_dev;

/* ---- register helpers ---- */

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

/* Every module returns nil + message rather than raising: a dead sensor
 * must not kill an app (the "modules degrade, never raise" rule). */
static int push_unavailable(lua_State *L, const char *what)
{
    lua_pushnil(L);
    lua_pushstring(L, what);
    return 2;
}

/* ---- rtc: PCF85063A ---- */

static int bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(int d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static int l_rtc_now(lua_State *L)
{
    uint8_t r[7];

    if (reg_read(s_rtc_dev, PCF_SECONDS, r, sizeof(r)) != ESP_OK) {
        return push_unavailable(L, "rtc not responding");
    }
    if (r[0] & PCF_SEC_VL) {
        /* The oscillator stopped (fresh board, dead backup cell): the
         * registers hold garbage. Say so instead of returning a
         * plausible-looking wrong time. */
        return push_unavailable(L, "rtc not set");
    }

    lua_newtable(L);
    lua_pushinteger(L, bcd2dec(r[0] & 0x7F)); lua_setfield(L, -2, "sec");
    lua_pushinteger(L, bcd2dec(r[1] & 0x7F)); lua_setfield(L, -2, "min");
    lua_pushinteger(L, bcd2dec(r[2] & 0x3F)); lua_setfield(L, -2, "hour");
    lua_pushinteger(L, bcd2dec(r[3] & 0x3F)); lua_setfield(L, -2, "day");
    lua_pushinteger(L, r[4] & 0x07);          lua_setfield(L, -2, "wday");
    lua_pushinteger(L, bcd2dec(r[5] & 0x1F)); lua_setfield(L, -2, "month");
    lua_pushinteger(L, 2000 + bcd2dec(r[6])); lua_setfield(L, -2, "year");
    return 1;
}

/* rtc.set{year=,month=,day=,hour=,min=,sec=,wday=} -- writing also
 * clears the integrity flag, which is the only way to make rtc.now()
 * trust the chip again. */
static int l_rtc_set(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    struct { const char *k; int def; int lo; int hi; int v; } f[] = {
        { "sec",   0, 0, 59,   0 },
        { "min",   0, 0, 59,   0 },
        { "hour",  0, 0, 23,   0 },
        { "day",   1, 1, 31,   0 },
        { "wday",  0, 0, 6,    0 },
        { "month", 1, 1, 12,   0 },
        { "year",  2026, 2000, 2099, 0 },
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        lua_getfield(L, 1, f[i].k);
        int v = lua_isnil(L, -1) ? f[i].def : (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        if (v < f[i].lo || v > f[i].hi) {
            return luaL_error(L, "rtc.set: %s out of range", f[i].k);
        }
        f[i].v = v;
    }

    uint8_t w[8];
    w[0] = PCF_SECONDS;
    w[1] = dec2bcd(f[0].v);            /* clearing bit 7 clears VL */
    w[2] = dec2bcd(f[1].v);
    w[3] = dec2bcd(f[2].v);
    w[4] = dec2bcd(f[3].v);
    w[5] = (uint8_t)f[4].v;
    w[6] = dec2bcd(f[5].v);
    w[7] = dec2bcd(f[6].v - 2000);

    if (s_rtc_dev == NULL ||
        i2c_master_transmit(s_rtc_dev, w, sizeof(w), I2C_TIMEOUT_MS) != ESP_OK) {
        return push_unavailable(L, "rtc not responding");
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg rtc_funcs[] = {
    {"now", l_rtc_now},
    {"set", l_rtc_set},
    {NULL, NULL},
};
static int luaopen_rtc(lua_State *L) { luaL_newlib(L, rtc_funcs); return 1; }

/* ---- imu: QMI8658 ---- */

static int16_t le16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

/* Scales derived from MEASUREMENT, not from a recalled datasheet.
 *
 * Accel: at rest the total vector must be exactly 1 g whatever the
 * orientation. With 8192 LSB/g the board read |a| = 0.50 g lying still,
 * so the chip is in +/-8g (4096 LSB/g) -- the CTRL2 value below selects
 * a different range than assumed. 4096 makes |a| = 1.00 g, which is
 * checkable physics rather than a guess.
 *
 * Gyro: NOT CALIBRATED. What is established on hardware is the resting
 * bias: 7-9 deg/s on X, drifting (Z accumulated -11.7 deg while the
 * board sat still). That alone rules out integrating this into angles.
 *
 * The SCALE below remains unverified rather than disproven. Attempts to
 * check it by hand were confounded by the USB cable: it limits how far
 * the board can turn (a "90 degree" turn measured 64) and makes any
 * motion compound rather than about a fixed axis -- and componentwise
 * integration is only valid for a fixed axis, so the axis mismatch
 * those runs showed proves nothing about the sensor. A clean check
 * needs either an untethered board or the QMI8658 datasheet's gyro
 * register base and full-scale encoding. */
#define ACCEL_LSB_PER_G    4096.0
#define GYRO_LSB_PER_DPS   64.0   /* assumed; see above */

static int imu_read_triplet(lua_State *L, uint8_t reg, double scale)
{
    uint8_t r[6];

    if (reg_read(s_imu_dev, reg, r, sizeof(r)) != ESP_OK) {
        return push_unavailable(L, "imu not responding");
    }
    lua_pushnumber(L, (lua_Number)(le16(r + 0) / scale));
    lua_pushnumber(L, (lua_Number)(le16(r + 2) / scale));
    lua_pushnumber(L, (lua_Number)(le16(r + 4) / scale));
    return 3;
}

static int l_imu_accel(lua_State *L) { return imu_read_triplet(L, QMI_AX_L, ACCEL_LSB_PER_G); }
static int l_imu_gyro(lua_State *L)  { return imu_read_triplet(L, QMI_AX_L + 6, GYRO_LSB_PER_DPS); }

/* DIE temperature, not ambient: this is the QMI8658's own silicon,
 * sitting on a powered board next to the S3, PSRAM and display.
 * Measured against a 18.3C room it reads ~7.6C high and holds within
 * 0.07C over 25s -- silicon at equilibrium, not air. Named die_temp so
 * nobody builds a room thermometer on it. */
static int l_imu_die_temp(lua_State *L)
{
    uint8_t r[2];

    if (reg_read(s_imu_dev, QMI_TEMP_L, r, sizeof(r)) != ESP_OK) {
        return push_unavailable(L, "imu not responding");
    }
    lua_pushnumber(L, (lua_Number)(le16(r) / 256.0));
    return 1;
}

static const luaL_Reg imu_funcs[] = {
    {"accel", l_imu_accel},
    {"gyro", l_imu_gyro},
    {"die_temp", l_imu_die_temp},
    {NULL, NULL},
};
static int luaopen_imu(lua_State *L) { luaL_newlib(L, imu_funcs); return 1; }

/* ---- battery: AXP2101 ---- */

static int l_bat_percent(lua_State *L)
{
    uint8_t v;

    if (reg_read(s_pmu_dev, AXP_BAT_PCT, &v, 1) != ESP_OK) {
        return push_unavailable(L, "pmu not responding");
    }
    if (v > 100) {
        /* The gauge reports 0xFF until it has settled. */
        return push_unavailable(L, "gauge not ready");
    }
    lua_pushinteger(L, v);
    return 1;
}

static int l_bat_volts(lua_State *L)
{
    uint8_t r[2];

    if (reg_read(s_pmu_dev, AXP_VBAT_H, r, sizeof(r)) != ESP_OK) {
        return push_unavailable(L, "pmu not responding");
    }
    /* 14-bit, big-endian, millivolts. */
    int mv = (((int)r[0] & 0x3F) << 8) | r[1];
    lua_pushnumber(L, (lua_Number)mv / 1000.0);
    return 1;
}

static int l_bat_charging(lua_State *L)
{
    uint8_t v;

    if (reg_read(s_pmu_dev, AXP_COMM_STAT1, &v, 1) != ESP_OK) {
        return push_unavailable(L, "pmu not responding");
    }
    lua_pushboolean(L, ((v >> 5) & 0x03) == 0x01);
    return 1;
}

static int l_bat_external(lua_State *L)
{
    uint8_t v;

    if (reg_read(s_pmu_dev, AXP_COMM_STAT0, &v, 1) != ESP_OK) {
        return push_unavailable(L, "pmu not responding");
    }
    lua_pushboolean(L, (v & 0x20) != 0);
    return 1;
}

static const luaL_Reg bat_funcs[] = {
    {"percent", l_bat_percent},
    {"volts", l_bat_volts},
    {"charging", l_bat_charging},
    {"external", l_bat_external},
    {NULL, NULL},
};
static int luaopen_battery(lua_State *L) { luaL_newlib(L, bat_funcs); return 1; }

/* ---- probe + registration ---- */

static i2c_master_dev_handle_t attach(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;

    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        return NULL;
    }
    return dev;
}

esp_err_t app_sensors_register(void)
{
    /* The BSP's bus, never a second one on the same pins: a second
     * handle on GPIO14/15 produces intermittent touch failures that look
     * like hardware faults. */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "no I2C bus; rtc/imu/battery unavailable");
    } else {
        s_rtc_dev = attach(bus, ADDR_PCF85063);
        s_imu_dev = attach(bus, ADDR_QMI8658);
        s_pmu_dev = attach(bus, ADDR_AXP2101);

        /* Wake the IMU: accel +/-4g @ 250Hz, gyro +/-512dps @ 250Hz,
         * both enabled. Without CTRL7 the data registers stay zero,
         * which reads exactly like a working sensor lying flat. */
        uint8_t who = 0;
        if (reg_read(s_imu_dev, QMI_WHO_AM_I, &who, 1) == ESP_OK && who == 0x05) {
            reg_write(s_imu_dev, QMI_CTRL1, 0x60);   /* auto-increment reads */
            reg_write(s_imu_dev, QMI_CTRL2, 0x24);   /* accel 4g, 250Hz */
            reg_write(s_imu_dev, QMI_CTRL3, 0x54);   /* gyro 512dps, 250Hz */
            reg_write(s_imu_dev, QMI_CTRL7, 0x03);   /* enable both */
        } else {
            ESP_LOGW(TAG, "QMI8658 not found (WHO_AM_I=0x%02x); imu unavailable", who);
            s_imu_dev = NULL;
        }

        /* READ-ONLY on the PMU, deliberately. The AXP2101 controls every
         * power rail on this board; an earlier version wrote what it
         * believed was an ADC-enable bit at 0x30 and the board stopped
         * booting -- the register meaning had been assumed, not read
         * from the datasheet. Never write a PMU register without one.
         * The fuel gauge at 0xA4 works without any configuration. */

        ESP_LOGI(TAG, "sensors: rtc=%s imu=%s pmu=%s",
                 s_rtc_dev ? "ok" : "-", s_imu_dev ? "ok" : "-", s_pmu_dev ? "ok" : "-");
    }

    esp_err_t err = cap_lua_register_module("rtc", luaopen_rtc);
    if (err != ESP_OK) return err;
    err = cap_lua_register_module("imu", luaopen_imu);
    if (err != ESP_OK) return err;
    return cap_lua_register_module("battery", luaopen_battery);
}
