#include "ui_fonts.hpp"

LV_FONT_DECLARE(rlcd_cjk_14)
LV_FONT_DECLARE(rlcd_cjk_20)
LV_FONT_DECLARE(rlcd_cjk_28)
LV_FONT_DECLARE(rlcd_digits_128)
LV_FONT_DECLARE(rlcd_weather_39)
LV_FONT_DECLARE(rlcd_weather_30)
LV_FONT_DECLARE(rlcd_weather_19)

namespace ui {
namespace {

// Copies rather than references: LVGL's built-in faces are const, so the
// fallback pointer cannot be attached to them in place.
lv_font_t g_small;
lv_font_t g_medium;
lv_font_t g_large;
bool g_ready = false;

}  // namespace

void fonts_init() {
  if (g_ready) return;
  g_small = lv_font_montserrat_14;
  g_small.fallback = &rlcd_cjk_14;
  g_medium = lv_font_montserrat_20;
  g_medium.fallback = &rlcd_cjk_20;
  g_large = lv_font_montserrat_28;
  g_large.fallback = &rlcd_cjk_28;
  g_ready = true;
}

// Each falls back to plain Montserrat before fonts_init(), so an early caller
// renders Latin correctly instead of dereferencing a zeroed lv_font_t.
const lv_font_t* font_small() {
  return g_ready ? &g_small : &lv_font_montserrat_14;
}
const lv_font_t* font_medium() {
  return g_ready ? &g_medium : &lv_font_montserrat_20;
}
const lv_font_t* font_large() {
  return g_ready ? &g_large : &lv_font_montserrat_28;
}
const lv_font_t* font_hero() { return &rlcd_digits_128; }

// No fallback on any of these and no fonts_init() dependency: each holds
// exactly seven glyphs in a private-use range nothing else draws from, so
// there is no script to fall back to and nothing to build at runtime.
const lv_font_t* font_weather_large() { return &rlcd_weather_39; }
const lv_font_t* font_weather_medium() { return &rlcd_weather_30; }
const lv_font_t* font_weather_small() { return &rlcd_weather_19; }

}  // namespace ui
