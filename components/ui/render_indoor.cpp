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
  // The extent the curve is stretched over, and what the scale divides into
  // whole units. No index alongside them any more: the value markers that
  // needed to know *where* the extreme fell were replaced by a scale, which
  // only needs to know how far the series reaches.
  int minimum = 0;
  int maximum = 0;
  // Mean of the slots that carry a reading, in the same units. Absent slots
  // are skipped rather than counted as anything - the same rule the rest of
  // this page follows, and the reason a gap in the ring cannot drag the
  // average toward a number nobody measured.
  int average = 0;
  std::size_t count = 0;  // how many slots are present, not how many slots
};

Series series_of(const app_core::IndoorData& indoor, bool temperature) {
  Series series;
  bool first = true;
  long long total = 0;
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
    if (first || value < series.minimum) series.minimum = value;
    if (first || value > series.maximum) series.maximum = value;
    total += value;
    first = false;
  }
  if (series.count != 0) {
    series.average =
        static_cast<int>(total / static_cast<long long>(series.count));
  }
  return series;
}

// x by slot index from `origin` to the newest slot, y by the series' own
// min..max.
//
// x counts slots, not present readings: a point sits where its time puts it,
// so a gap in the middle leaves a gap in the line rather than closing up.
// That is the entire reason the series comes out of the ring by position -
// see history_series() in history.hpp. `origin` is the oldest slot with any
// reading (indoor_history_origin), which is what spends the full width on
// the history that exists instead of leaving four-fifths of the box blank
// while the ring fills.
ChartPoint plot_point(const Rect plot, std::size_t index, std::size_t origin,
                      const Series& series, int value) {
  const std::size_t span = app_core::kIndoorHistoryPoints - 1 - origin;
  const int x =
      span == 0
          ? plot.right() - 1
          : plot.x + static_cast<int>((index - origin) *
                                      static_cast<std::size_t>(plot.width - 1) /
                                      span);
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
                    const std::vector<ChartPoint>& run, int width) {
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
  // Deliberately NOT apply_surface(): it sets bg_opa to LV_OPA_COVER with
  // white, and this object is sized to the whole plot, so an opaque
  // background paints a white rectangle over everything already drawn - the
  // hour rules, the average rules, and the other series' curve.
  //
  // That is what "the hour rule is invisible" and "the humidity line is
  // invisible" both were. Neither was a stroke too thin to survive the 1-bit
  // threshold; each was a sibling painted on top of it. Three stroke widths
  // were tried against that theory before the draw order explained it - the
  // curve you could see was whichever one happened to be drawn last.
  lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(line, 0, 0);
  lv_obj_set_style_shadow_width(line, 0, 0);
  lv_obj_set_style_pad_all(line, 0, 0);
  lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(line, plot.x, plot.y);
  lv_obj_set_size(line, plot.width, plot.height);
  lv_obj_set_style_line_color(line, lv_color_black(), 0);
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_rounded(line, false, 0);
  lv_line_set_points(line, points, static_cast<uint32_t>(run.size()));
  lv_obj_add_event_cb(line, release_points, LV_EVENT_DELETE, points);
}

// How the two curves are told apart on a panel with no colour and no grey.
//
// By stroke weight: temperature 4 px, humidity 2 px, each one lv_line.
//
// 2 px is fine, and the three rounds spent insisting otherwise were chasing
// the wrong cause - see draw_solid_run above, where an opaque background on
// a plot-sized object was erasing whichever curve had been drawn first. With
// that fixed a thin stroke is simply a thin stroke.
constexpr int kTemperatureLineWidth = 4;
constexpr int kHumidityLineWidth = 2;

void draw_lone_point(lv_obj_t* parent, const ChartPoint point) {
  line_segment(parent, point.x - 1, point.y - 1, 3, 3);
}

void draw_series(lv_obj_t* parent, const Rect plot, std::size_t origin,
                 const Series& series, int width) {
  std::vector<ChartPoint> run;
  for (std::size_t i = origin; i <= app_core::kIndoorHistoryPoints; ++i) {
    const bool present = i < app_core::kIndoorHistoryPoints && series.present[i];
    if (present) {
      run.push_back(plot_point(plot, i, origin, series, series.value[i]));
      continue;
    }
    if (run.size() == 1) {
      draw_lone_point(parent, run[0]);
    } else if (run.size() > 1) {
      draw_solid_run(parent, plot, run, width);
    }
    run.clear();
  }
}

// How far apart the labelled values on one scale should be.
//
// Round numbers, not the series' own min and max. A curve annotated only at
// its extremes tells you where it peaked; a scale of whole units lets you
// read any point on it, which is what a chart is for. The extremes were
// tried first and they are the smaller thing.
//
// `candidates` is smallest-first, in the series' own storage units, and the
// first one that keeps the tick count inside the budget wins. Both lists
// start at the unit a person actually thinks in for that measure - a whole
// degree, one percent - and coarsen from there, so a quiet room gets a fine
// scale and a day of weather gets a legible one rather than forty marks.
//
// Ten, not the six this allowed when every tick carried a number. Ticks are
// 2 px marks now and the numbers are limited separately, so the scale can
// afford to be fine enough to count steps along.
constexpr int kMaxScaleTicks = 10;


// Where a value sits on its series' own scale.
int value_y(const Rect strip, const Series& series, int value) {
  const int range = series.maximum - series.minimum;
  if (range == 0) return strip.y + strip.height / 2;
  return strip.y + ((series.maximum - value) * (strip.height - 1)) / range;
}

// The window's mean, drawn the full width of the plot.
//
// Dashed, and neither 1 px nor a solid 2 px: a solid rule would read as a
// third series, and a 1 px one is invisible on this panel - the hour rules
// already proved that. A long pitch keeps it cheap: 20 px per dash across
// 372 px is about 19 objects, which is the budget this page has to respect
// (see draw_hour_rules for what happens when it does not).
constexpr int kAverageDash = 8;
constexpr int kAverageGap = 12;

void draw_average_rule(lv_obj_t* parent, const Rect plot, int y) {
  for (int offset = 0; offset < plot.width;
       offset += kAverageDash + kAverageGap) {
    const int length = std::min(kAverageDash, plot.width - offset);
    line_segment(parent, plot.x + offset, y, length, kDataLineWidth);
  }
}

// One series' scale, overlaid on the edge of the plot it belongs to.
//
// Each label sits at the exact height of the value it names, so the
// positions are as measured as the curve is - the rounding is in which
// values get named, never in where they are drawn.
//
// The two series are scaled independently over their own min..max (see the
// comment at the call site for why a degree and a percent must not share an
// axis), so without this the height of a line means nothing beyond "higher
// than it was".
//
// Each value gets a tick at the plot's outer edge with its number inboard,
// so a label belongs to a height rather than floating near one.
// `mirrored` flips the pair for the right-hand scale, putting its tick on
// the plot's far edge and its number to the left of it.
//
// The marks are placed in priority order - the extremes first, then the
// average, then the round intermediates - and any whose label would overlap
// one already placed is dropped. So a narrow range cannot stack three
// numbers on top of each other, and what survives a crowded scale is what
// the reader most needs.
void draw_scale(lv_obj_t* parent, const Rect plot, const Rect strip,
                const Series& series, bool temperature, bool mirrored) {
  // Nothing measured, nothing to scale. One reading is its own minimum and
  // maximum, and a flat run has no extent to divide.
  if (series.count < 2 || series.minimum == series.maximum) return;

  // Decidegrees and percent respectively - see HistorySample. A whole degree
  // is 10 of the former.
  //
  // Humidity starts at 1, not 5. Fives were tried and an indoor room is too
  // steady for them: a 55-58% window contains exactly one multiple of five,
  // so the entire right-hand scale was the single figure "55%". The step
  // search below widens on its own when the air actually moves, so nothing
  // is lost by starting fine.
  static constexpr int kTemperatureSteps[] = {10, 20, 50, 100, 200, 500};
  static constexpr int kHumiditySteps[] = {1, 2, 5, 10, 20, 25, 50};
  const int* candidates = temperature ? kTemperatureSteps : kHumiditySteps;
  const std::size_t candidate_count =
      temperature ? std::size(kTemperatureSteps) : std::size(kHumiditySteps);
  const int step =
      scale_step(series.minimum, series.maximum, candidates,
                 candidate_count, kMaxScaleTicks);

  constexpr int kLabelHeight = 16;
  // Shared with the gutter widths in ui_data.hpp, which are the tick plus the
  // text box: if this and those disagree, the labels silently lose room.
  constexpr int kTickLength = kScaleTickLength;
  const int text_width = strip.width - kTickLength;

  // Label tops already spoken for, and the tick heights that go with them.
  // Capacity: the two extremes, the average, and one per step mark.
  constexpr std::size_t kMaxLabels = 3 + kMaxScaleTicks;
  std::array<int, kMaxLabels> taken{};
  std::array<int, kMaxLabels> taken_tick{};
  // The value each reserved slot belongs to. Carried rather than re-derived
  // from the slot's position: a reservation can be refused for overlap, so
  // "slot 1 is the minimum" stops being true the moment one is skipped.
  std::array<int, kMaxLabels> taken_value{};
  // Whether that value is a measurement or a scale reference, which decides
  // how it prints. A measurement keeps its decimal - rounding a maximum of
  // 26.4 to 26 would put a figure on the panel the sensor never reported -
  // while a step mark is an even reference and has no decimal to lose.
  std::array<bool, kMaxLabels> taken_exact{};
  std::size_t taken_count = 0;

  // Which values get a number, decided before anything is drawn so the step
  // ticks below can stand aside for them.
  //
  // `exact` keeps a decimal for temperature: the extremes and the average
  // are measurements, and rounding 26.4 to 26 would print a figure the
  // sensor never reported. The step marks are scale references, not
  // readings, so they stay whole.
  const auto reserve = [&](int value, bool exact) {
    if (taken_count >= kMaxLabels) return;
    const int y = value_y(strip, series, value);
    int text_y = y - kLabelHeight / 2;
    if (text_y < plot.y) text_y = plot.y;
    if (text_y + kLabelHeight > plot.bottom()) {
      text_y = plot.bottom() - kLabelHeight;
    }
    for (std::size_t i = 0; i < taken_count; ++i) {
      if (text_y < taken[i] + kLabelHeight && taken[i] < text_y + kLabelHeight) {
        return;
      }
    }
    taken[taken_count] = text_y;
    taken_tick[taken_count] = y;
    taken_value[taken_count] = value;
    taken_exact[taken_count] = exact;
    ++taken_count;
  };

  // The extremes say what the curve's whole vertical extent means; the
  // average is the one figure about the window rather than about a moment.
  // These are reserved first so that where a measurement and a round step
  // mark fall within a label of each other, the measured one is the one that
  // survives - it is the fact, and the step is only a ruler.
  reserve(series.maximum, true);
  reserve(series.minimum, true);
  reserve(series.average, true);

  // Then the round values, so a height can be read off the scale directly
  // instead of interpolated between two extremes tens of pixels apart. Each
  // is refused if it would overlap a label already placed, so the scale
  // thins out by itself on a narrow range rather than stacking numbers.
  const int first_label = (series.minimum + step - 1) / step * step;
  for (int value = first_label; value <= series.maximum; value += step) {
    reserve(value, false);
  }

  // Ticks at every whole unit, including the ones whose number was refused.
  //
  // Most step marks now carry a label (they are reserved above), and those
  // draw their own tick with the numbers below. This loop is what keeps the
  // scale fine-grained where the labels had to thin out: on a narrow range
  // the reservations start colliding and get refused, and without this the
  // scale would lose the mark as well as the number. A tick is two pixels and
  // costs nothing to read.
  //
  // A step mark within kTickMergeDistance of a labelled one is skipped, which
  // also stops a labelled step from drawing its tick twice. A maximum of 25.8
  // and a whole-degree mark at 26.0 land two pixels apart, which read as one
  // thick smudged tick rather than two facts; the labelled height is the
  // measured one, so it is the one that stays.
  constexpr int kTickMergeDistance = 4;
  const int mark_x = mirrored ? strip.x + text_width : strip.x;
  const int first = (series.minimum + step - 1) / step * step;
  for (int value = first; value <= series.maximum; value += step) {
    const int tick_y = value_y(strip, series, value);
    bool collides = false;
    for (std::size_t i = 0; i < taken_count; ++i) {
      const int gap = tick_y - taken_tick[i];
      if (gap > -kTickMergeDistance && gap < kTickMergeDistance) {
        collides = true;
        break;
      }
    }
    if (collides) continue;
    line_segment(parent, mark_x, tick_y, kTickLength, kDataLineWidth);
  }

  // The numbers themselves, each with the tick at its own measured height.
  for (std::size_t i = 0; i < taken_count; ++i) {
    const int value = taken_value[i];
    char caption[20];
    // No unit: the header row above carries °C and % beside their swatches,
    // and repeating either down the axis would cost the plot real width for
    // ink that says nothing new. See kIndoorTemperatureScaleWidth.
    if (!temperature) {
      std::snprintf(caption, sizeof(caption), "%d", value);
    } else if (taken_exact[i]) {
      std::snprintf(caption, sizeof(caption), "%.1f", value / 10.0);
    } else {
      // A whole number of degrees, because kTemperatureSteps starts at 10
      // decidegrees - a step mark never lands between two of them.
      std::snprintf(caption, sizeof(caption), "%d", value / 10);
    }
    const int text_x = mirrored ? strip.x : strip.x + kTickLength;
    label(parent, caption, {text_x, taken[i], text_width, kLabelHeight},
          small_font(), mirrored ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
    line_segment(parent, mark_x, taken_tick[i], kTickLength, kDataLineWidth);
  }
}

// A sample of the stroke its curve is drawn with, at the same weight. Sits
// beside the reading in the header, so the stroke is identified where the
// measure is named rather than in a legend the eye has to carry back to the
// plot.
void stroke_swatch(lv_obj_t* parent, const Rect bounds, int width) {
  const int y = bounds.y + bounds.height / 2 - width / 2;
  line_segment(parent, bounds.x, y, bounds.width, width);
}

// Grid weight, and the width the hour labels are laid out against. Named so
// the clamping below and the rule itself cannot drift apart.
constexpr int kHourRuleWidth = 2;
constexpr int kHourLabelWidth = 20;

// A rule at every whole hour inside the window, with the hour under it.
//
// This is how the chart says how wide it is: the span changes as the ring
// fills, so a fixed "8 h" caption would be wrong most of the time, and a
// label per point was more numbers than the question deserved. Counting
// rules answers it at a glance.
//
// 2 px, like every other stroke on this panel. A 1 px rule was tried and
// reported back from the board as simply not visible - which is what the
// board's own rule (pure black, 2 px minimum) says about a reflective
// display with no backlight, and a grid line nobody can see is not a
// lighter grid line, it is an absent one.
//
// So the separation from data is orientation, not weight: these are
// full-height verticals behind curves that run horizontally. Nothing here
// reads as a third series.
//
// One object per rule, and that is a hard constraint rather than a style
// preference. This drew each rule as a column of 2 px dashes stepping 4 px
// down the plot, which is 40 lv_objs per rule; at 8 rules across a 7.5 h
// window that is 320 widgets on top of the ~150 the curves, markers and
// header already cost, against a 64 KB LVGL heap
// (CONFIG_LV_MEM_SIZE_KILOBYTES). lv_malloc returned null, LVGL 9 does not
// check it, and the page panicked with StoreProhibited inside
// lv_obj_class_create_obj - every time the Indoor page came round, which is
// a reboot loop rather than a glitch.
//
// It hid until the network came up, because of the guard directly below:
// with no clock there are no rules, so the page fit. Nothing in the type
// system counts widgets, so the count lives here - anything added to this
// loop is paid for once per hour drawn, not once per page.
//
// Needs the clock: an hour boundary is a wall-clock fact, and without a
// synced time there is nothing to anchor one to. Nothing is drawn then,
// exactly as the axis labels draw nothing.
void draw_hour_rules(lv_obj_t* parent, const Rect plot, const Rect axis,
                     const app_core::IndoorData& indoor, std::size_t origin) {
  if (!indoor.history_time_known || indoor.history_interval_minutes == 0) {
    return;
  }
  const std::size_t span_slots = app_core::kIndoorHistoryPoints - 1 - origin;
  if (span_slots == 0) return;
  const int span_minutes =
      static_cast<int>(span_slots) *
      static_cast<int>(indoor.history_interval_minutes);
  if (span_minutes <= 0) return;

  const ClockHm start = time_minus_minutes(indoor.history_newest_hour,
                                           indoor.history_newest_minute,
                                           span_minutes);
  for (int offset = minutes_to_next_hour(start.minute); offset <= span_minutes;
       offset += 60) {
    const int x = plot.x + offset * (plot.width - 1) / span_minutes;
    line_segment(parent, x, plot.y, kHourRuleWidth, plot.height);

    // The hour itself, centred under its own rule.
    //
    // Two digits, not "23:00": the rules are on whole hours by construction,
    // so ":00" is the same claim four more glyphs. It also has to fit - at a
    // 450-minute window the rules land about 49 px apart, which "23:00"
    // (~36 px) would fill nearly edge to edge, and two digits (~16 px) leave
    // the scale legible instead of crowded.
    const ClockHm at = time_minus_minutes(indoor.history_newest_hour,
                                          indoor.history_newest_minute,
                                          span_minutes - offset);
    // Sized for what the format can emit rather than for what the value can
    // hold: at.hour is always 0..23, but %u is promoted to unsigned int and
    // the compiler rejects a buffer that only fits the values we know about.
    char hour[8];
    std::snprintf(hour, sizeof(hour), "%02u", at.hour);
    // Clamped into the axis strip rather than centred blindly: the first and
    // last rules sit against the plot edges, and a label centred on one of
    // those would hang outside the safe canvas.
    int label_x = x + kHourRuleWidth / 2 - kHourLabelWidth / 2;
    if (label_x < axis.x) label_x = axis.x;
    if (label_x + kHourLabelWidth > axis.right()) {
      label_x = axis.right() - kHourLabelWidth;
    }
    label(parent, hour, {label_x, axis.y, kHourLabelWidth, axis.height},
          small_font(), LV_TEXT_ALIGN_CENTER);
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

  // Each measure once: the stroke its curve uses, its icon, the reading, and
  // the arrow for what that reading just did.
  char reading[24];
  stroke_swatch(parent, layout.temperature_swatch, kTemperatureLineWidth);
  temperature_icon(parent, layout.temperature_icon);
  std::snprintf(reading, sizeof(reading), "%s",
                temperature_text(snapshot.indoor.temperature_c, 1).c_str());
  label(parent, reading, layout.temperature, reading_font(),
        LV_TEXT_ALIGN_RIGHT);
  trend_icon(parent, layout.temperature_trend,
             trend_of(temperature, kTemperatureTrend));

  stroke_swatch(parent, layout.humidity_swatch, kHumidityLineWidth);
  humidity_icon(parent, layout.humidity_icon);
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
  // The oldest slot either series has a reading in is the chart's left edge,
  // so the whole width is spent on history that exists - see
  // indoor_history_origin(). Rules first, so the curves sit over them.
  const std::size_t origin = indoor_history_origin(snapshot.indoor);
  draw_hour_rules(parent, layout.plot, layout.axis, snapshot.indoor, origin);
  // Averages with the grid, before the curves: this is a reference line, and
  // a curve crossing it should read as the room passing through its own mean
  // rather than the line interrupting the measurement.
  if (draw_temperature) {
    draw_average_rule(parent, layout.plot,
                      value_y(layout.plot, temperature, temperature.average));
  }
  if (draw_humidity) {
    draw_average_rule(parent, layout.plot,
                      value_y(layout.plot, humidity, humidity.average));
  }

  if (draw_temperature) {
    draw_series(parent, layout.plot, origin, temperature,
                kTemperatureLineWidth);
  }
  if (draw_humidity) {
    draw_series(parent, layout.plot, origin, humidity, kHumidityLineWidth);
  }

  // Both scales after both curves, and that ordering is load-bearing. LVGL
  // paints siblings in creation order and label() gives every label an
  // opaque white surface, so a scale drawn here knocks its own hole in the
  // grid and curves behind it. Drawn per-series instead - scale, then the
  // other curve - the second curve would paint straight back over the first
  // series' numbers.
  //
  // `mirrored` is about which edge of the gutter the ticks live on, and both
  // flags flipped when the scales moved out of the plot: a tick has to touch
  // the data it marks, so the left gutter's ticks sit on its right edge and
  // the right gutter's on its left. Overlaid, it was the other way round.
  if (draw_temperature) {
    draw_scale(parent, layout.plot, layout.scale_left, temperature, true,
               true);
  }
  if (draw_humidity) {
    draw_scale(parent, layout.plot, layout.scale_right, humidity, false,
               false);
  }

  // No end labels here any more: draw_hour_rules above writes the whole
  // scale, and it carries the same "only with a synced clock" gate these
  // did - an unsynced device's idea of the time is a compile-time guess, and
  // an axis labelled from it would invent the one thing an axis is for.
}

}  // namespace ui
