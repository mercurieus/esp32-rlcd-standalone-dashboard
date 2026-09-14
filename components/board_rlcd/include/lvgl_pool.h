#pragma once

// Where LVGL's own heap comes from.
//
// LVGL's built-in allocator (LV_STDLIB_BUILTIN) normally puts its TLSF pool in
// a static array, which on this target means internal RAM - the one resource
// this board is actually short of. The boot log measures it: about 81 KB free
// after startup with a largest free block of 31 KB, against 7.8 MB of PSRAM
// doing nothing. sdkconfig.defaults documents what that internal RAM is spoken
// for (AirPlay's raop_create() needs 13,468 contiguous bytes, and Wi-Fi buffer
// counts were already trimmed to win back 6 KB), so growing a static pool
// there to fit a chart would break something further away.
//
// LV_MEM_POOL_ALLOC is LVGL's own hook for exactly this: the allocator, its
// statistics and lv_mem_monitor() all stay as they were, only the pool's
// address changes. Wired up in the top-level CMakeLists.txt, which also has
// to add this directory to LVGL's include path.
//
// Widget structs and styles therefore live behind the PSRAM cache. That is
// affordable here and was measured against the alternative: the panel already
// spends ~28 ms converting a frame and ~10.5 ms blocked on its DMA, and the
// draw buffers were already in PSRAM before this change (see board_lvgl's
// "LVGL RGB565 buffers" line) without trouble.

#include <stddef.h>

#include <esp_heap_caps.h>

static inline void* lvgl_pool_alloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
