#include "ui_app.hpp"
#include "ui_fonts.hpp"

#include <algorithm>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

namespace ui {
namespace {

// Not font_hero(): that is the clock's 128px face and it contains ten digits
// and a colon. A temperature carries a decimal point, a space and a unit, none
// of which have glyphs there - which is exactly how this page lost its reading
// until a screenshot showed an empty box where the number should be.
const lv_font_t* reading_font() { return font_medium(); }
const lv_font_t* small_font() { return font_small(); }

void release_points(lv_event_t* event) {
  auto* points = static_cast<lv_point_precise_t*>(lv_event_get_user_data(event));
  delete[] points;
}

// One series, reduced to what the plot needs: the value at each slot in its
// own integer unit (decidegrees or whole percent), and whether that slot has
// one at all.
struct Series {
  std::array<int, app_core::kIndoorHistoryPoints> value{};
  std::array<bool, app_core::kIndoorHistoryPoints> present{};
  int minimum = 0;
  int maximum = 0;
  std::size_t minimum_index = 0;
  std::size_t maximum_index = 0;
  std::size_t count = 0;  // how many slots are present, not how many slots
};

Series series_of(const app_core::IndoorData& indoor, bool temperature) {
  Series series;
  bool first = true;
  for (std::size_t i = 0; i < app_core::kIndoorHistoryPoints; ++i) {
    const app_core::IndoorHistoryPoint& point = indoor.history[i];
    const bool present =
        temperature ? point.has_temperature : point.has_humidity;
    if (!present) continue;
    const int value = temperature
                          ? static_cast<int>(point.temperature_c * 10.0)
                          : static_cast<int>(point.humidity_percent);
    series.value[i] = value;
    series.present[i] = true;
    ++series.count;
    if (first || value < series.minimum) {
      series.minimum = value;
      series.minimum_index = i;
    }
    if (first || value > series.maximum) {
      series.maximum = value;
      series.maximum_index = i;
    }
    first = false;
  }
  return series;
}

// x by slot index across the whole window, y by the series' own min..max.
//
// x deliberately ignores how many slots are present: a point sits where its
// time puts it, so a gap in the middle of the window leaves a gap in the
// line rather than closing up. That is the entire reason the series comes
// out of the ring by position - see history_series() in history.hpp.
ChartPoint plot_point(const Rect plot, std::size_t index, const Series& series,
                      int value) {
  const int x =
      plot.x + static_cast<int>(index * static_cast<std::size_t>(plot.width - 1) /
                                (app_core::kIndoorHistoryPoints - 1));
  const int range = series.maximum - series.minimum;
  int y = plot.y + plot.height / 2;
  if (range != 0) {
    y = plot.y + ((series.maximum - value) * (plot.height - 1)) / range;
  }
  if (y < plot.y) y = plot.y;
  if (y >= plot.bottom()) y = plot.bottom() - 1;
  return {x, y};
}

// A run of adjacent present slots, drawn as one polyline. Runs rather than
// one line through everything: bridging an absent slot would draw a segment
// across time nobody measured.
void draw_solid_run(lv_obj_t* parent, const Rect plot,
                    const std::vector<ChartPoint>& run) {
  if (run.size() < 2) return;
  auto* points = new (std::nothrow) lv_point_precise_t[run.size()];
  if (points == nullptr) return;
  for (std::size_t i = 0; i < run.size(); ++i) {
    points[i] = {run[i].x - plot.x, run[i].y - plot.y};
  }
  lv_obj_t* line = lv_line_create(parent);
  if (line == nullptr) {
    delete[] points;
    return;
  }
  apply_surface(line);
  lv_obj_set_pos(line, plot.x, plot.y);
  lv_obj_set_size(line, plot.width, plot.height);
  lv_obj_set_style_line_color(line, lv_color_black(), 0);
  lv_obj_set_style_line_width(line, kDataLineWidth, 0);
  lv_obj_set_style_line_rounded(line, false, 0);
  lv_line_set_points(line, points, static_cast<uint32_t>(run.size()));
  lv_obj_add_event_cb(line, release_points, LV_EVENT_DELETE, points);
}

constexpr int kDashLength = 4;
constexpr int kDashGap = 3;

// The second series, dashed so it can be told from the first on a panel with
// no colour and no grey to spend. Stepped by hand out of line_segment calls
// because LVGL 9 has no dash style - render_market.cpp's dotted_grid already
// draws a broken line this way - and each dash is kDataLineWidth tall so it
// carries the same weight as the solid series instead of speckling away.
void draw_dashed_run(lv_obj_t* parent, const std::vector<ChartPoint>& run) {
  for (std::size_t i = 0; i + 1 < run.size(); ++i) {
    const ChartPoint from = run[i];
    const ChartPoint to = run[i + 1];
    const int span = to.x - from.x;
    if (span <= 0) continue;
    for (int offset = 0; offset < span; offset += kDashLength + kDashGap) {
      const int length = std::min(kDashLength, span - offset);
      const int y = from.y + (to.y - from.y) * (offset + length / 2) / span;
      line_segment(parent, from.x + offset, y, length, kDataLineWidth);
    }
  }
}

// A lone present slot between two absent ones is still a measurement, and a
// polyline of one point draws nothing - so it gets a mark of its own rather
// than vanishing.
void draw_lone_point(lv_obj_t* parent, const ChartPoint point) {
  line_segment(parent, point.x - 1, point.y - 1, 3, 3);
}

void draw_series(lv_obj_t* parent, const Rect plot, const Series& series,
                 bool dashed) {
  std::vector<ChartPoint> run;
  for (std::size_t i = 0; i <= app_core::kIndoorHistoryPoints; ++i) {
    const bool present = i < app_core::kIndoorHistoryPoints && series.present[i];
    if (present) {
      run.push_back(plot_point(plot, i, series, series.value[i]));
      continue;
    }
    if (run.size() == 1) {
      draw_lone_point(parent, run[0]);
    } else if (run.size() > 1) {
      if (dashed) {
        draw_dashed_run(parent, run);
      } else {
        draw_solid_run(parent, plot, run);
      }
    }
    run.clear();
  }
}

// Slot `index` as a wall-clock HH:MM, given that the newest slot is at the
// time the snapshot carries. Empty when the clock is not trustworthy - see
// history_time_known in app_snapshot.hpp.
std::string time_at(const app_core::IndoorData& indoor, std::size_t index) {
  if (!indoor.history_time_known || indoor.history_interval_minutes == 0) {
    return {};
  }
  const int back =
      static_cast<int>(app_core::kIndoorHistoryPoints - 1 - index) *
      static_cast<int>(indoor.history_interval_minutes);
  const ClockHm at = time_minus_minutes(indoor.history_newest_hour,
                                        indoor.history_newest_minute, back);
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02u:%02u", at.hour, at.minute);
  return buffer;
}

// A tick on the curve at its own extreme, labelled with the value and, when
// the clock allows it, when it happened.
//
// The label goes below a maximum and above a minimum, which puts it inward
// from the plot edge the extreme is pressed against, and its x is clamped so
// it cannot hang outside the plot. Four of these on one 1-bit chart is the
// busiest thing on the page; if it reads as clutter on the glass, dropping
// humidity's pair is one branch here.
void draw_extreme(lv_obj_t* parent, const Rect plot, const ChartPoint at,
                  const std::string& value, const std::string& when,
                  bool is_maximum) {
  line_segment(parent, at.x - 3, at.y - 1, 7, 2);

  std::string caption = value;
  if (!when.empty()) caption += " " + when;

  constexpr int kLabelWidth = 92;
  constexpr int kLabelHeight = 16;
  int x = at.x - kLabelWidth / 2;
  x = std::max(plot.x, std::min(x, plot.right() - kLabelWidth));
  const int y = is_maximum ? at.y + 3 : at.y - 3 - kLabelHeight;
  label(parent, caption.c_str(), {x, y, kLabelWidth, kLabelHeight},
        small_font(), LV_TEXT_ALIGN_CENTER);
}

void draw_extremes(lv_obj_t* parent, const Rect plot,
                   const app_core::IndoorData& indoor, const Series& series,
                   bool temperature) {
  // One point is its own minimum and maximum; labelling it twice says
  // nothing twice. Two identical labels on top of each other is also how a
  // flat series would render, so both cases fall out of the same check.
  if (series.count < 2 || series.minimum == series.maximum) return;

  const auto caption = [temperature](int value) {
    if (temperature) {
      char buffer[12];
      std::snprintf(buffer, sizeof(buffer), "%.1f°", value / 10.0);
      return std::string(buffer);
    }
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%d%%", value);
    return std::string(buffer);
  };

  draw_extreme(parent, plot,
               plot_point(plot, series.maximum_index, series, series.maximum),
               caption(series.maximum), time_at(indoor, series.maximum_index),
               true);
  draw_extreme(parent, plot,
               plot_point(plot, series.minimum_index, series, series.minimum),
               caption(series.minimum), time_at(indoor, series.minimum_index),
               false);
}

// Solid bar or dashes, matching how the series is drawn. This is what binds
// a stroke to a measure; without it the two lines are anonymous shapes.
void legend_swatch(lv_obj_t* parent, const Rect bounds, bool dashed) {
  const int y = bounds.y + bounds.height / 2 - kDataLineWidth / 2;
  if (!dashed) {
    line_segment(parent, bounds.x, y, bounds.width, kDataLineWidth);
    return;
  }
  for (int offset = 0; offset < bounds.width;
       offset += kDashLength + kDashGap) {
    const int length = std::min(kDashLength, bounds.width - offset);
    line_segment(parent, bounds.x + offset, y, length, kDataLineWidth);
  }
}

void draw_legend(lv_obj_t* parent, const Rect bounds, bool temperature_drawn,
                 bool humidity_drawn) {
  const int cluster = 34;
  int x = bounds.x + (bounds.width - cluster * 2 - 12) / 2;
  if (temperature_drawn) {
    legend_swatch(parent, {x, bounds.y, 14, bounds.height}, false);
    temperature_icon(parent, {x + 18, bounds.y + 1, 13, bounds.height - 2});
  }
  x += cluster + 12;
  if (humidity_drawn) {
    legend_swatch(parent, {x, bounds.y, 14, bounds.height}, true);
    humidity_icon(parent, {x + 18, bounds.y + 1, 13, bounds.height - 2});
  }
}

// The trend the header arrows describe, from the series' two newest present
// readings.
//
// Compacted into a dense array on purpose: trend_for_series() reads the last
// step and makes no claim about when it happened, so closing the gaps is
// harmless here in a way it would never be for the chart. A gap does mean
// the step spans longer than one interval, which widens what the threshold
// is really measuring - acceptable for an arrow, not for an axis.
TrendKind trend_of(const Series& series, const TrendThresholds thresholds) {
  std::array<int, app_core::kIndoorHistoryPoints> dense{};
  std::size_t count = 0;
  for (std::size_t i = 0; i < app_core::kIndoorHistoryPoints; ++i) {
    if (series.present[i]) dense[count++] = series.value[i];
  }
  return trend_for_series(dense, count, thresholds);
}

}  // namespace

void render_indoor(lv_obj_t* parent, const app_core::AppSnapshot& snapshot,
                   Rect bounds, std::size_t page_index,
                   std::size_t page_count, UiContext* context) {
  (void)context;
  // Page position lives in the system tray (see render_tray in
  // render_shared.cpp), not a corner overlay on the page itself.
  (void)page_index;
  (void)page_count;
  apply_surface(parent);

  const IndoorLayout layout = indoor_layout(bounds);
  label(parent, text(Text::TileIndoor), layout.title, small_font());

  if (!snapshot.indoor.valid) {
    // No fabricated temperature, humidity or chart below the title - see
    // ui_data.hpp's no_data_rect for the shared placeholder geometry.
    label(parent, text(Text::NoData), no_data_rect(bounds), reading_font(),
          LV_TEXT_ALIGN_CENTER);
    return;
  }

  const Series temperature = series_of(snapshot.indoor, true);
  const Series humidity = series_of(snapshot.indoor, false);

  char reading[24];
  std::snprintf(reading, sizeof(reading), "%s",
                temperature_text(snapshot.indoor.temperature_c, 1).c_str());
  label(parent, reading, layout.temperature, reading_font(),
        LV_TEXT_ALIGN_RIGHT);
  trend_icon(parent, layout.temperature_trend,
             trend_of(temperature, kTemperatureTrend));

  std::snprintf(reading, sizeof(reading), "%u%%",
                snapshot.indoor.humidity_percent);
  label(parent, reading, layout.humidity, reading_font(), LV_TEXT_ALIGN_RIGHT);
  trend_icon(parent, layout.humidity_trend,
             trend_of(humidity, kHumidityTrend));

  divider(parent, layout.divider);

  // Two points is a line; one is a reading and none is nothing. Each series
  // is judged on its own so one can draw while the other is still filling.
  const bool draw_temperature = temperature.count >= 2;
  const bool draw_humidity = humidity.count >= 2;
  if (!draw_temperature && !draw_humidity) {
    label(parent, text(Text::HistoryCollecting), layout.plot, small_font(),
          LV_TEXT_ALIGN_CENTER);
    return;
  }

  // Each series is scaled over its own min..max, so the two lines' vertical
  // positions carry no meaning against each other - only their shapes do,
  // and the markers carry the numbers. Do not "fix" this by sharing one
  // scale: a degree and a percent have no common axis, and whichever series
  // had the narrower range would flatten into a straight line that reads as
  // a room holding still when it is not.
  if (draw_temperature) {
    draw_series(parent, layout.plot, temperature, false);
    draw_extremes(parent, layout.plot, snapshot.indoor, temperature, true);
  }
  if (draw_humidity) {
    draw_series(parent, layout.plot, humidity, true);
    draw_extremes(parent, layout.plot, snapshot.indoor, humidity, false);
  }

  draw_legend(parent, layout.legend, draw_temperature, draw_humidity);

  // The axis says how wide the window actually is. Drawn only with a synced
  // clock: an unsynced device's idea of the time is a compile-time guess,
  // and labelling an axis from it would invent the one thing the axis is
  // there to report.
  const std::string oldest = time_at(snapshot.indoor, 0);
  const std::string newest =
      time_at(snapshot.indoor, app_core::kIndoorHistoryPoints - 1);
  if (!oldest.empty()) {
    label(parent, oldest.c_str(), layout.axis_oldest, small_font());
  }
  if (!newest.empty()) {
    label(parent, newest.c_str(), layout.axis_newest, small_font(),
          LV_TEXT_ALIGN_RIGHT);
  }
}

}  // namespace ui
