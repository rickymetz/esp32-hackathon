> Generated: 2026-08-24 | Token-lean format for LLM context

# Types, constants, hardware map

## Core structs

```c
/* app_registry.h */
typedef struct {
    char name[APP_NAME_MAX];   /* display name from the filename/folder */
    char path[APP_PATH_MAX];   /* absolute path to the .lua to load */
    char id[APP_ID_MAX];       /* stable identity: "counter.lua" or "counter" */
    bool in_folder;            /* true when apps/<id>/main.lua */
} app_entry_t;

/* app_timer.c */
typedef struct {
    int64_t  next_us;      /* advanced BY period, not rebased on now */
    int64_t  period_us;    /* 0 == one-shot */
    int      ref;          /* LUA_NOREF when free */
    uint32_t gen;          /* bumped per (re)alloc; detects stale handles */
} app_timer_t;

/* launcher_main.c — serial TAP/SWIPE */
typedef struct { bool active; int x0,y0,x1,y1; int64_t start_us, dur_us; } synth_touch_t;

/* launcher_home.h */
typedef enum { LAUNCHER_VIEW_LIST, LAUNCHER_VIEW_GRID } launcher_view_t;
typedef struct { const char *name, *basename, *icon, *icon_path; } launcher_home_app_t;
```

## Limits

| Constant | Value | Where / why |
|---|---|---|
| `APP_MAX_COUNT` | 32 | registry array |
| `APP_NAME_MAX` | 48 | display name; **names truncate and can collide** |
| `APP_ID_MAX` | 128 | LFN can be long |
| `APP_PATH_MAX` | 320 | mount point + `/apps/` + 255-char LFN |
| `APP_TIMER_MAX` | 16 | per app; a 17th raises |
| `APP_TASK_STACK` | 32 KB | measured 5.5KB used by a simple app |
| `MAX_VISIBLE_ROWS` | 64 | render cap; a truncated list says so |
| `ROW_HEIGHT` | 104 | Wear OS chip height at this panel's 2× |
| `EVENT_PUMP_MS` | 100 | max pump wait |
| `SYNTH_QUEUE` / `SYNTH_GAP_US` | 8 / 90000 | queued taps + enforced release gap |
| `LINE_MAX` / `NAME_MAX` / `PAYLOAD_MAX` | 256 / 128 / 64 KB | serial protocol |
| `SHOT_CHUNK` / `SHOT_B64_MAX` | 720 / 964 | 448×736 frame → 458 lines |
| `BODY_LINE_MIN` | 32 | base64-vs-typo discriminator |
| `CONFIG_LUA_MAXSTACK` | 65536 | default 1e6 masks the real OOM cause |
| `CONFIG_FREERTOS_HZ` | 1000 | `pdMS_TO_TICKS(n) == n` |

## Hardware (board revision **V2**, confirmed on hardware)

CO5300 display + CST816S touch @ I²C `0x15`. V1 is SH8601 + FT3168 @ `0x38` —
**always go through the BSP**, which binds the right driver at runtime.

ESP32-S3R8 · dual core · 8 MB **octal** PSRAM · 16 MB flash · 368×448 QSPI AMOLED.

```
QSPI AMOLED : SDIO0=4  SDIO1=5  SDIO2=6  SDIO3=7  SCLK=11  CS=12
I2C bus     : SDA=15   SCL=14   touch INT=21
ES8311 audio: MCLK=16  BCLK=9   WS=45    DI=10   DO=8    PA=46
SD (SDMMC)  : CLK=2    CMD=1    D0=3
BOOT button : GPIO0 (active low)     — Home; direct GPIO, survives an I2C wedge
```

I²C addresses: AXP2101 `0x34` · PCF85063 `0x51` · QMI8658 `0x6A`/`0x6B` ·
TCA9554 `0x20` · ES8311 `0x18`.

TCA9554 expander lines:

| Line | Use |
|---|---|
| EXIO1 | LCD reset — **pulse before `bsp_display_start()` or the panel stays black** |
| EXIO2 | touch reset — same |
| EXIO4 | PWR button (active high), owned by apps |
| EXIO5 | PMU IRQ |

Brightness is register `0x51`, `0x00`–`0xFF`.

`pin_config.h` names the audio pins **twice and inconsistently**
(`I2S_DI_IO=10 / I2S_DO_IO=8` vs `DOPIN=10 / DIPIN=8`) — named from opposite
ends of the link. Pick one convention.

## Measured figures

| Thing | Value |
|---|---|
| `timer.every` drift | **0.0 ms/tick** at 100ms and 1000ms (was 5.0 / 4.1) |
| Event enqueue → dispatch | median **0.76 ms** (was 24.69) |
| STOP/BOOT ack | median 14.4 ms, max 17.7 |
| SHOT round trip | ~1.54 s (1.445 s of it transfer, ~306 KB/s) |
| PSRAM free at idle | ~4.9 MB (voice model costs ~3 MB) |
| Bare Lua VM | ~15.5 KB PSRAM; real app ~40 KB |
| `lua_app` idle CPU | 3–4‰ |
| `serial_push` stack free after SHOT | ~2.3 KB of 8192 |
| Touch reliability | 240×120 catches every tap; 180×56 dropped ~half |

## Filesystem layout (SD card)

```
/apps/<name>.lua              flat app
/apps/<name>/main.lua         folder app
/apps/<name>/icon.bin         optional, LVGL RGB565, 128px
/state/<id>.json              per-app store
/font_scale.txt               persisted UI font scale (default 0.8–1.0)
```

Card root is `/sdcard` on-device; `D:` to the LVGL filesystem driver.

## Lua module roster (complete)

`lvgl` `timer` `ui` `keyboard` `button` `store` `voice` `audio` `rtc` `imu`
`battery` `wifi`. There are no others, and apps cannot `require` a `.lua` of
their own.

`rtc` / `imu` / `battery` / `wifi` / `voice` **degrade rather than raise** —
they return `nil, "reason"`.
