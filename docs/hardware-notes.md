# Hardware notes

Constraints and measurements for the Waveshare ESP32-S3-Touch-AMOLED-2.16, the
board this firmware is developed against. Most of them come down to the same
thing: **the device is out of internal RAM, not out of CPU.**

Two serial commands report the live numbers — `lvmem` for the widget pool,
`bench` for frame cost.

## Internal RAM is the binding constraint

The S3 has 8 MB of PSRAM and it is nearly all free (~7.7 MB), but PSRAM cannot
be used for everything. TLS in particular needs internal heap, and the poller
needs roughly **35 KB of internal free** to complete a handshake. Steady state
sits at ~35.6 KB free, so there is essentially nothing spare.

Three rules follow, and breaking any of them has already cost a debugging
session at least once:

1. **The poll task's stack lives in PSRAM** (`poller.cpp` allocates it with
   `MALLOC_CAP_SPIRAM`). Putting it in internal RAM starves lwIP and every
   connection fails with -2 / -5 / -54.
2. **NVS writes must never run on the poll task.** A flash write from a
   PSRAM-stacked task trips `esp_task_stack_is_sane` and boot-loops the device.
   Hence `secrets_set()` only updates the RAM cache and marks the entry dirty;
   `secrets_flush()` does the actual write from the main loop.
3. **LVGL's pool is fixed and nearly full** — see below.

## LVGL widget pool: ~8 KB free, and that caps provider count

LVGL allocates every widget from a fixed pool set by `LV_MEM_SIZE=49152` (48 KB)
in `platformio.ini`. With two providers configured:

```
lvmem: free=8048 frag=16% internal=35600
```

Rebuilding the provider tiles for a **third** provider exhausts that pool. LVGL
aborts, and the device resets (`rst:0xc RTC_SW_CPU_RST`). Note that the panic
text is lost over USB-CDC — decode the ROM's `Saved PC` against the ELF instead
of expecting a backtrace, and even then it often points somewhere useless, so
treat "silent reset right after a UI rebuild" as "the pool ran out."

`MAX_PROVIDERS` in `ui_common.h` is **an array bound, not a capability claim.**
Raising it does not raise the ceiling.

Each provider costs an overview row plus a full card tile. Raising the real
ceiling means one of:

1. Moving the LVGL pool to PSRAM. The S3 has room; the C6 boards have no PSRAM
   at all, so they would keep the internal pool and a lower cap.
2. Building card tiles lazily — only for the visible column — which would also
   roughly halve the per-provider cost.

The pool cannot simply be enlarged: taking another 16 KB of internal RAM for
LVGL would drop TLS below what it needs and break polling.

A related trap: `lv_obj_clean()` on an `lv_tileview` frees the tile the widget
still holds an internal pointer to, and the next swipe walks freed memory.
`ui_rebuild_provider_tiles()` deletes and recreates the tileview for that reason.

## Frame cost: PSRAM bandwidth, not the bus

`bench` forces 20 full-screen redraws and splits the cost into pushing pixels to
the panel versus LVGL rendering them.

| Configuration | ms/frame | push | render |
|---|---|---|---|
| 40 MHz QSPI, 40-line strips | 94 | 57 | 37 |
| 80 MHz QSPI | 83 | 46 | 37 |
| 80 MHz QSPI, 120-line strips | 73 | 47 | 26 |

Both changes are in the tree: `display.cpp` passes 80 MHz to `gfx->begin()`
(the library defaults to 40), and `BUF_LINES` is 120 rather than 40.

The useful result is that **doubling the bus clock bought only 11 ms.** The push
is not clock-bound — it is bound by reading the draw buffer out of PSRAM
(460 KB per full frame, ~10 MB/s effective). That is also why bigger strips paid
off in the *render* column instead: fewer flushes, less per-flush setup.

Getting past ~73 ms means draw buffers in internal RAM, which is not available
for the reason at the top of this file. A real swipe only invalidates the
tileview region (322 of 480 rows), so it runs nearer 20 FPS than the 13.7 the
full-screen benchmark reports.

## Flash is comparatively free

Fonts, icons and splash animations are `const` and live in flash, so type-scale
changes cost nothing in RAM. The 2.16 build sits around 80% of its app partition;
the other boards are near 40% on a larger table. Generating a bigger font cut is
cheap — regenerate from the OTF with `lv_font_conv` using the same options
recorded in the header comment of any existing `font_*.c`.

## Serial diagnostics

| Command | Reports |
|---|---|
| `lvmem` | LVGL pool free / fragmentation, internal heap free |
| `bench` | ms/frame over 20 full redraws, split push vs render |
| `screenshot` | RGB565 framebuffer dump (`./screenshot.sh`) |
| `screen <name>` | Jump to splash / usage / settings / wifi |
| `tile <col> [page]` | Jump the usage tileview |

A sample of `lvmem` taken mid-poll reads ~26 KB internal rather than ~35 KB.
That is a TLS session in flight, not a leak.
