# RAM / visual-glitch audit

A static (code-read) investigation into reported visual artifacts and suspected
RAM pressure while using apps. **No board was available**, so everything here is
evidence from source plus a procedure to confirm each item on hardware. Findings
are ordered by how likely they are to explain a visible glitch.

**Refreshed against `main` after PR #5** (performance instrumentation, build
fixes, firmware releases). Both high-severity findings below were re-verified
and still hold on current `main`. PR #5 is complementary, not overlapping: it
built the *instruments*, this audit names the *suspects*. Read
[`PERF_DEBUG.md`](PERF_DEBUG.md) first for how to drive `STATS`, the FPS
overlay and JTAG -- the procedures here use those rather than the older `MEM`.

Two things PR #5 changed for the better here:

- **`STATS` replaces `MEM`** as the measurement of record. It reports all-time
  low-water marks (`psram min`, `internal min`) and `internal largest`, which
  is exactly the fragmentation signature finding 1 predicts. `MEM` reports only
  the instantaneous figure and cannot show either.
- **The on-screen FPS overlay makes finding 1 directly observable.**
  `LV_USE_PERF_MONITOR` paints FPS into the corner and `SHOT` captures it, so
  the icon-decode cost can be measured by screenshot rather than inferred.

It also fixed the device build on ESP-IDF v5.5.5 / GCC 14 and published measured
hardware baselines (PSRAM free 5,082,680; internal free 192,151; largest
contiguous internal 73,728) -- so the device C in this repo is now known to
compile and run, which it previously was not.

---

## 1. Image cache is OFF while RAM-load is ON — 32 KB alloc + full SD read *per icon, per draw*

**Severity: high. Most likely explanation for stutter/jank on the home screen.**

Two settings combine badly:

| Setting | Value | Where |
| --- | --- | --- |
| `LV_CACHE_DEF_SIZE` | **0 (caching disabled)** | LVGL Kconfig default; the device sets no override |
| `CONFIG_LV_BIN_DECODER_RAM_LOAD` | **y** | `launcher/sdkconfig.defaults` |

With RAM-load on, `decode_rgb()` allocates a full-image buffer
(`lv_draw_buf_create_ex`) and reads the entire file into it. For a 128x128
RGB565 card icon that is **32,768 bytes** plus a whole-file SD read.

With the cache disabled, that work is **not reused**. `lv_bin_decoder.c` returns
early before caching:

```c
if(!lv_image_cache_is_enabled()) return LV_RESULT_OK;   /* decoder keeps ownership */
```

so every open -> decode -> close cycle re-reads the file and re-allocates the
buffer. The close path *is* correct — `lv_bin_decoder_close()` calls
`free_decoder_data()` — so **this is not a leak**. It is repeated churn:

- repeated SD-card I/O on every redraw of a screen containing card icons
- repeated 32 KB alloc/free in PSRAM -> fragmentation over time
- worst exactly where card icons live: the launcher home screen, while scrolling

It scales with the number of card icons. `build_grid()` builds **all** pages
eagerly (up to 16 pages), and the list renders up to `MAX_VISIBLE_ROWS` (64).

**Regression note:** `LV_BIN_DECODER_RAM_LOAD=y` was added deliberately to fix
card icons being scaled with visible clipping/aliasing. It fixed that, but with
the cache off it traded the artifact for per-draw allocation + I/O. The two
settings need to be enabled together.

**Fix:** set an image cache large enough for the working set, e.g.
`CONFIG_LV_CACHE_DEF_SIZE` sized for ~8 icons (8 x 32 KB = 256 KB, in PSRAM).
Each icon then decodes once and is reused, which removes both the repeated SD
reads and the allocation churn, and makes RAM-load cheap rather than costly.

**Confirm on board:** `STATS` before and after ~20 home Refreshes. The
fragmentation signature is `internal largest` (and PSRAM headroom) degrading
while free totals look healthy -- and `psram min` is the number that actually
bounds it, which is why `STATS` and not `MEM`. Then compare the **FPS overlay**
on the home screen with a card that has folder apps (card icons present)
against one with only flat `.lua` apps (no card icons): if finding 1 is the
cause, FPS should drop measurably with icons present and while scrolling, and
`SHOT` records it.

---

## 2. `buffer_lines`, `tick_ms`, `task_period_ms` are silently no-ops

**Severity: high (correctness of the documented API).**

`lua_lvgl_runtime.c` collects these and packs them into
`display_service_session_config_t.display_config`:

```c
.display_config = { .buffer_lines = (uint32_t)buffer_lines, ... }
```

`display_service.c` copies `owner_name`, `mode`, `flags`, `cleanup_cb` and
`user_ctx` out of that config — and **never reads `display_config` at all**
(verified: no occurrence of `display_config` in the implementation). The real
display and its draw buffer come from the BSP's `bsp_display_start()` in
`app_main()`, which no app can influence.

This matters here because `docs/APP_CONTRACT.md` tells app authors:

> `buffer_lines` sizes LVGL's render buffer, in screen rows... the only reason to
> change it is a full-screen animation that visibly tears (raise it)

**The documented remedy for visible tearing does nothing.** Anyone chasing a
rendering artifact by raising `buffer_lines` is changing a value that is thrown
away.

**Fix:** either wire the config through to a real draw-buffer resize, or drop
the three fields from the API and the contract. Since the contract is currently
changeable, removing them is the honest, smaller change; wiring them through is
the larger one and only worth it if per-app buffer sizing is genuinely wanted.

---

## 3. `SHOT` allocates a 330 KB snapshot

**Severity: low (developer tooling only).**

`handle_shot()` calls `lv_snapshot_take(..., LV_COLOR_FORMAT_RGB565)` on the
active screen: 368 x 448 x 2 = **329,728 bytes**, from PSRAM, freed after the
transfer. Fine in isolation, but it is a large transient spike taken *while an
app is running and holding its own memory*. If a glitch is ever observed
specifically right after a screenshot, this is the reason. Not a suspect for
normal use.

---

## 4. App launch/exit cleanup looks sound

**Severity: none found statically.**

`lua_app_task()`'s teardown is thorough and unconditional: `app_timer_reset`,
`app_button_reset`, `app_voice_reset`, `app_audio_reset`,
`lua_lvgl_force_unlock_if_held`, `launcher_lua_run_exit_cleanup`, `lua_close`.
No leak is visible from reading it. Note the repo's claim that repeated
launch/exit returns PSRAM to an identical figure was measured *before* card
icons, RAM-load and the app-info sheet landed, so it is worth re-measuring.

**Confirm on board:** `STATS` across ~20 launch/exit cycles of the same app.
Free totals returning to their starting values is necessary but not sufficient
-- watch `psram min` / `internal min` and `internal largest`, since a cycle that
dipped close to the floor and recovered looks identical to one that never did on
a free-total-only reading.

---

## What could not be checked without hardware

- **Where the BSP puts the draw buffer** (internal DMA-capable RAM vs PSRAM).
  `managed_components/` is fetched at build time and is not in the repo, so the
  BSP's allocation could not be inspected. A draw buffer in PSRAM is the other
  classic cause of tearing on this SoC, and it remains unverified.
- Any timing-dependent effect (QSPI flush vs refresh, cache coherency).

## Suggested order on the board

Uses the instrumentation PR #5 added -- see [`PERF_DEBUG.md`](PERF_DEBUG.md).

1. **`STATS` soak:** 20 home Refreshes, then 20 app launch/exit cycles. Record
   `psram free`/`min`, `internal free`/`min` and `internal largest` at each
   step. Fragmentation shows as `internal largest` falling while totals hold.
2. **FPS test:** with the overlay on, compare home-screen frame rate scrolling a
   card that has folder apps (card icons) against one with only flat apps.
   `SHOT` captures the number, so this is a screenshot diff, not a judgement.
3. Apply the cache fix (finding 1), repeat 1 and 2, compare against the PR #5
   baselines (PSRAM free 5,082,680 / internal free 192,151 / largest 73,728).
4. If artifacts persist with the cache on, inspect the BSP draw-buffer caps --
   the one thing neither this audit nor PR #5 has been able to see.
