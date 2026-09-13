#include "ui_app.hpp"
#include "ui_fonts.hpp"

#include <algorithm>
#include <cstdio>
#include <new>
#include <string>

namespace ui {
namespace {

// Not font_hero(): that is the clock's 128px face and it contains ten digits
// and a colon. A temperature carries a decimal point, a space and a unit, none
// of which have glyphs there - which is exactly how this page lost its reading
// until a screenshot showed an empty box where the number should be.
const lv_font_t* hero_font() { return font_large(); }
const lv_font_t* medium_font() { return font_medium(); }
const lv_font_t* small_font() { return font_small(); }

void release_points(lv_event_t* event) {
  auto* points = static_cast<lv_point_precise_t*>(lv_event_get_user_data(event));
  delete[] points;
}

// Draws only the points that were actually recorded. The array's unused tail
// is zeros, and a line through those is a shape made of readings nobody took -
// the same objection this project has to a flat intraday chart drawn from a
// repeated close.
void solid_series(lv_obj_t* parent, const Rect bounds,
                  const std::array<ChartPoint, 8>& normalized,
                  std::size_t count) {
  auto* points = new (std::nothrow) lv_point_precise_t[count];
  if (points == nullptr) return;
  for (std::size_t index = 0; index < count; ++index) {
    points[index] = {normalized[index].x - bounds.x,
                     normalized[index].y - bounds.y};
  }
  lv_obj_t* line = lv_line_create(parent);
  if (line == nullptr) {
    delete[] points;
    return;
  }
  apply_surface(line);
  lv_obj_set_pos(line, bounds.x, bounds.y);
  lv_obj_set_size(line, bounds.width, bounds.height);
  lv_obj_set_style_line_color(line, lv_color_black(), 0);
  lv_obj_set_style_line_width(line, kDataLineWidth, 0);
  lv_obj_set_style_line_rounded(line, false, 0);
  lv_line_set_points(line, points, static_cast<uint32_t>(count));
  lv_obj_add_event_cb(line, release_points, LV_EVENT_DELETE, points);
}

// The second series, dashed so it can be told from the first one on a panel
// with no colour and no grey to spend.
//
// Walked as short rectangles rather than drawn as an lv_line: LVGL 9 has no
// dash style, and render_market.cpp's dotted_grid already establishes
// hand-stepped dashes as how this project draws a broken line. Each dash is
// kDataLineWidth tall so it carries the same weight as the solid series -
// a 1 px dash would speckle away on this glass.
void dashed_series(lv_obj_t* parent,
                   const std::array<ChartPoint, 8>& normalized,
                   std::size_t count) {
  constexpr int kDashLength = 4;
  constexpr int kDashGap = 3;
  for (std::size_t index = 0; index + 1 < count; ++index) {
    const ChartPoint from = normalized[index];
    const ChartPoint to = normalized[index + 1];
    const int span = to.x - from.x;
    if (span <= 0) continue;
    for (int offset = 0; offset < span; offset += kDashLength + kDashGap) {
      const int length = std::min(kDashLength, span - offset);
      // Linear interpolation along the segment, so a dash sits on the line
      // it belongs to rather than on the chord between sample columns.
      const int y = from.y + (to.y - from.y) * (offset + length / 2) / span;
      line_segment(parent, from.x + offset, y, length, kDataLineWidth);
    }
  }
}

// Solid bar or three dashes, matching how each series is drawn in the plot.
// This is what binds a stroke style to a measure; without it the two lines
// are two anonymous shapes.
void legend_swatch(lv_obj_t* parent, const Rect bounds, bool dashed) {
  const int y = bounds.y + bounds.height / 2 - kDataLineWidth / 2;
  if (!dashed) {
    line_segment(parent, bounds.x, y, bounds.width, kDataLineWidth);
    return;
  }
  constexpr int kDashLength = 4;
  constexpr int kDashGap = 3;
  for (int offset = 0; offset < bounds.width;
       offset += kDashLength + kDashGap) {
    const int length = std::min(kDashLength, bounds.width - offset);
    line_segment(parent, bounds.x + offset, y, length, kDataLineWidth);
  }
}

// Swatch, measure icon and trend arrow, in that order.
constexpr int kLegendClusterWidth = 46;
constexpr int kLegendClusterGap = 10;

// The two readings one end of the chart carries, e.g. "24.8°C 57%". Either
// half is omitted when its series has no point to report, so a page with one
// working measure still says something true rather than printing a
// placeholder for the other.
std::string readings_text(bool has_temperature, double celsius,
                          bool has_humidity, int humidity_percent) {
  std::string out;
  if (has_temperature) {
    out += temperature_text(static_cast<float>(celsius), 1);
  }
  if (has_humidity) {
    if (!out.empty()) out += " ";
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%d%%", humidity_percent);
    out += buffer;
  }
  return out;
}

void render_history_block(lv_obj_t* parent, const app_core::IndoorData& indoor,
                          const IndoorHistoryLayout layout) {
  label(parent, text(Text::TileHistory), layout.title, small_font());

  // Clamped rather than trusted: these counts cross a task boundary on the
  // snapshot, and an index past the end of the array is a far worse failure
  // than a chart one point short.
  const std::size_t temperature_count =
      std::min<std::size_t>(indoor.temperature_history_count,
                            indoor.temperature_history_c.size());
  const std::size_t humidity_count =
      std::min<std::size_t>(indoor.humidity_history_count,
                            indoor.humidity_history_percent.size());

  // Each series in its own integer unit - decidegrees and whole percent -
  // which is what trend_for_series' thresholds are written against and what
  // normalize_chart_samples_n wants anyway.
  std::array<int, 8> temperature_samples{};
  for (std::size_t i = 0; i < temperature_count; ++i) {
    temperature_samples[i] =
        static_cast<int>(indoor.temperature_history_c[i] * 10.0);
  }
  std::array<int, 8> humidity_samples{};
  for (std::size_t i = 0; i < humidity_count; ++i) {
    humidity_samples[i] = indoor.humidity_history_percent[i];
  }

  // Judged per series, not once for both, so one line can be drawn while the
  // other is still collecting.
  //
  // In practice the two counts move together: app_main's take_slot() writes
  // temperature and humidity from one accumulator gated on one environment
  // sample count, so a flash slot holds both readings or neither, and the
  // live appends add to both arrays on the same tick. That is what makes
  // index i of each array the same moment - which is the whole basis for
  // drawing them against one time axis. If the counts ever did diverge, each
  // series would still be spread across the full plot width and their x
  // positions would stop corresponding; the fix then is to make the
  // recorder pair them again, not to reconcile it here.
  const bool draw_temperature = temperature_count >= 2;
  const bool draw_humidity = humidity_count >= 2;

  if (!draw_temperature && !draw_humidity) {
    // One point is not a trend, and zero is not a chart. Say which, where
    // the chart would have been.
    label(parent, text(Text::HistoryCollecting), layout.plot, small_font(),
          LV_TEXT_ALIGN_CENTER);
    return;
  }

  // Each series is scaled over its own min..max, so the two lines' vertical
  // positions carry no meaning against each other - only each line's own
  // shape does, and the readings below carry the numbers. Do not "fix" this
  // by sharing one scale: a degree and a percent have no common axis, and
  // whichever series had the narrower range would flatten into a straight
  // line that reads as a room holding steady when it is not.
  if (draw_temperature) {
    solid_series(parent, layout.plot,
                 normalize_chart_samples_n(temperature_samples, layout.plot,
                                           temperature_count),
                 temperature_count);
  }
  if (draw_humidity) {
    dashed_series(parent,
                  normalize_chart_samples_n(humidity_samples, layout.plot,
                                            humidity_count),
                  humidity_count);
  }

  // Laid out right to left so temperature ends up on the left of humidity,
  // and so a series that is not drawn takes its legend with it rather than
  // leaving a gap or a swatch for a line that is not there.
  const auto draw_cluster = [&](int x, bool dashed, bool temperature,
                                TrendKind trend) {
    legend_swatch(parent, {x, layout.legend.y, 14, layout.legend.height},
                  dashed);
    const Rect icon{x + 18, layout.legend.y + 2, 13,
                    layout.legend.height - 4};
    if (temperature) {
      temperature_icon(parent, icon);
    } else {
      humidity_icon(parent, icon);
    }
    trend_icon(parent, {x + 33, layout.legend.y, 13, layout.legend.height},
               trend);
  };

  int cluster_x = layout.legend.right() - kLegendClusterWidth;
  if (draw_humidity) {
    draw_cluster(cluster_x, true, false,
                 trend_for_series(humidity_samples, humidity_count,
                                  kHumidityTrend));
    cluster_x -= kLegendClusterWidth + kLegendClusterGap;
  }
  if (draw_temperature) {
    draw_cluster(cluster_x, false, true,
                 trend_for_series(temperature_samples, temperature_count,
                                  kTemperatureTrend));
  }

  const std::string oldest = readings_text(
      draw_temperature, draw_temperature ? indoor.temperature_history_c[0] : 0.0,
      draw_humidity, draw_humidity ? indoor.humidity_history_percent[0] : 0);
  const std::string newest = readings_text(
      draw_temperature,
      draw_temperature ? indoor.temperature_history_c[temperature_count - 1]
                       : 0.0,
      draw_humidity,
      draw_humidity ? indoor.humidity_history_percent[humidity_count - 1] : 0);
  label(parent, oldest.c_str(), layout.oldest, small_font());
  label(parent, newest.c_str(), layout.newest, small_font(),
        LV_TEXT_ALIGN_RIGHT);
}

}  // namespace

void render_indoor(lv_obj_t* parent, const app_core::AppSnapshot& snapshot,
                   Rect bounds, std::size_t page_index,
                   std::size_t page_count, UiContext* context) {
  (void)context;
  // Page position now lives in the system tray (see render_tray in
  // render_shared.cpp), not a corner overlay on the page itself.
  (void)page_index;
  (void)page_count;
  apply_surface(parent);
  // Full width. market_layout's 72/28 split was only here to leave room for a
  // sidebar this page no longer has.
  const Rect primary_full = bounds;
  const Rect primary = primary_full;
  label(parent, text(Text::TileIndoor), {primary.x + 8, primary.y + 5, 100, 18}, small_font());

  if (!snapshot.indoor.valid) {
    // No fabricated temperature/humidity/comfort band/history chart below
    // the title - see ui_data.hpp no_data_rect for the shared placeholder
    // geometry. A comfort band drawn at 0% or a flat all-zero history line
    // would both be fabricated numbers, so nothing below the title draws.
    label(parent, text(Text::NoData), no_data_rect(primary), medium_font(),
          LV_TEXT_ALIGN_CENTER);
  } else {
    char temperature[24];
    char humidity[24];
    std::snprintf(temperature, sizeof(temperature), "%s",
                  temperature_text(snapshot.indoor.temperature_c, 1).c_str());
    std::snprintf(humidity, sizeof(humidity), "RH %u%%",
                  snapshot.indoor.humidity_percent);
    label(parent, temperature, {primary.x + 8, primary.y + 25, 190, 58},
          hero_font());
    label(parent, humidity, {primary.x + 211, primary.y + 44,
                             primary.width - 219, 28}, medium_font(),
          LV_TEXT_ALIGN_RIGHT);
    render_history_block(parent, snapshot.indoor, indoor_history_layout(primary));
  }

  // No sidebar. The market pages used to lend this page their column, which
  // put an index quote beside a room temperature and made the page about two
  // unrelated things. The sensor page is about the sensor; the primary column
  // takes the full width.
}

}  // namespace ui
