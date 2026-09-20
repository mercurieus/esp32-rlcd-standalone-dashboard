#include "ui_app.hpp"
#include "ui_fonts.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace ui {
namespace {

const lv_font_t* large_font() { return font_large(); }
const lv_font_t* medium_font() { return font_medium(); }
const lv_font_t* small_font() { return font_small(); }

void release_chart_points(lv_event_t* event) {
  auto* points = static_cast<lv_point_precise_t*>(lv_event_get_user_data(event));
  delete[] points;
}

// `count` - not source.size() - is how many of `source` are real: since
// app_snapshot.hpp's MarketData::intraday_sample_count can be smaller than
// the array's own app_core::kIntradaySampleCount (early in a session,
// before enough real bars exist to fill it), normalize_chart_samples_n
// leaves every slot past `count` zero-valued rather than a lie about
// where those points belong - drawing all of source unconditionally would
// draw a line down to (and back from) the coordinate origin.
void polyline(lv_obj_t* parent,
              const std::array<ChartPoint, app_core::kIntradaySampleCount>& source,
              std::size_t count, const Rect chart) {
  auto* points = new (std::nothrow) lv_point_precise_t[count];
  if (points == nullptr) return;
  for (std::size_t index = 0; index < count; ++index) {
    points[index] = {source[index].x - chart.x, source[index].y - chart.y};
  }
  lv_obj_t* line = lv_line_create(parent);
  if (line == nullptr) {
    delete[] points;
    return;
  }
  // Deliberately NOT apply_surface(): it sets bg_opa to LV_OPA_COVER with
  // white, and this object is sized to the whole chart, so an opaque
  // background paints a white rectangle over everything drawn before it -
  // here, the dotted grid, which has been invisible on this page for exactly
  // that reason, and now the rate scale and axis ticks too.
  //
  // This is the same defect that made the indoor chart's hour rules and
  // humidity curve disappear; see draw_solid_run() in render_indoor.cpp,
  // where three stroke widths were tried before the draw order explained it.
  lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(line, 0, 0);
  lv_obj_set_style_shadow_width(line, 0, 0);
  lv_obj_set_style_pad_all(line, 0, 0);
  lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(line, chart.x, chart.y);
  lv_obj_set_size(line, chart.width, chart.height);
  lv_obj_set_style_line_color(line, lv_color_black(), 0);
  lv_obj_set_style_line_width(line, kDataLineWidth, 0);
  lv_obj_set_style_line_rounded(line, false, 0);
  lv_line_set_points(line, points, count);
  lv_obj_add_event_cb(line, release_chart_points, LV_EVENT_DELETE, points);
}


// The widest value a scale label can hold, measured from
// lv_font_montserrat_14's own glyph table rather than from today's rate: '4'
// is the widest digit at 9.375 px and '.' is 3.1875, so "999.99" is 50.1 px.
// Sized for three integer digits deliberately - a box that only fits two is a
// bet on the hryvnia, and the label would clip the day it lost.
constexpr int kRateLabelWidth = 52;
constexpr int kRateTickLength = 5;
constexpr int kRateScaleGap = 3;
// The gutter the scale owns, taken off the plot's width rather than laid over
// it. The labels used to be overlaid to keep the polyline full-width, and on
// this panel that was the wrong trade: each label carries an opaque white
// plate (see label() in ui_theme.cpp), so a number sat in the data punched a
// hole in the very line it was describing. Narrower data beats obscured data.
constexpr int kRateScaleWidth = kRateLabelWidth + kRateTickLength +
                                kRateScaleGap;
constexpr int kAxisTickWidth = 2;
constexpr int kAxisTickHeight = 4;

// At most four labelled lines: five was legible but busy on a 400x300
// reflective panel, and four keeps the step one decade coarser - 0.10 rather
// than 0.05 on a month of this rate, which is what makes the marks land on
// 44.50 / 44.60 / 44.70 instead of every half-step between them.
constexpr int kMaxRateTicks = 4;

// The plot proper: the chart box less the scale's gutter. Everything that
// carries data - grid, polyline, date labels - is laid out against this, so
// the line's ends and the dates beneath them stay aligned.
constexpr Rect rate_plot_rect(const Rect chart) {
  return {chart.x, chart.y, std::max(1, chart.width - kRateScaleWidth),
          chart.height};
}

// The gutter must leave a plot worth drawing into, and must itself stay
// inside the page's primary column - a label that starts past the column's
// right edge is not clipped by LVGL, it simply renders over the sidebar.
// Both are properties of constants that are easy to nudge, so they are proven
// here rather than eyeballed on the panel.
static_assert(
    rate_plot_rect(
        market_chart_rect(
            market_layout(content_bounds(safe_canvas(), app_core::PageId::UaFx))
                .primary,
            kSetupSmallFontLineHeight))
            .width >= 120,
    "the rates scale gutter leaves too little room for the polyline");
static_assert(
    rate_plot_rect(
        market_chart_rect(
            market_layout(content_bounds(safe_canvas(), app_core::PageId::UaFx))
                .primary,
            kSetupSmallFontLineHeight))
                .right() +
            kRateTickLength + kRateScaleGap + kRateLabelWidth <=
        market_chart_rect(
            market_layout(content_bounds(safe_canvas(), app_core::PageId::UaFx))
                .primary,
            kSetupSmallFontLineHeight)
            .right(),
    "the rates scale labels overflow the chart box");

// The value axis the chart never had: without it the polyline's height means
// nothing, because the normalizer rescales every series to fill the box - a
// flat month and a volatile one draw the same shape.
//
// The marks land on round values (44.50, 44.60, ...), not on fractions of the
// plot. A grid ruled at quarter-heights answers "what is a quarter of the way
// up this box", which nobody asks; one ruled at round values answers "is it
// above 44.60", which is the question. Only values that fall inside the
// measured range get a line, so the grid never implies headroom the data does
// not have.
//
// Interpolating the scale is legitimate in a way an interpolated date is not:
// a scale is arithmetic on a measured range, whereas a midpoint date would be
// a day nobody published - which is why the date axis below still gets only
// its two real endpoints.
void draw_rate_grid(lv_obj_t* parent, const Rect plot, const MarketRange range,
                    const int label_height) {
  // A flat series gets no scale rather than one line labelled with the single
  // value repeated - and no division by a zero span either.
  if (range.high <= range.low) return;

  // Hundredths of a hryvnia, the unit intraday_samples is kept in: 1, 2 and 5
  // of a unit and the same through each decade above it. A month of this rate
  // spans ~27 of them and picks 10 - a 0.10 step.
  static constexpr int kCandidates[] = {1, 2, 5, 10, 20, 50, 100, 200, 500,
                                        1000, 2000, 5000};
  const int step = scale_step(range.low, range.high, kCandidates,
                              std::size(kCandidates), kMaxRateTicks);

  const int span = range.high - range.low;
  for (int value = (range.low + step - 1) / step * step; value <= range.high;
       value += step) {
    // The same mapping normalize_chart_samples_n uses: maximum to the top of
    // the box, minimum to the bottom. If these two ever disagree the labels
    // stop naming the heights the data is drawn at.
    const int y = plot.y + ((range.high - value) * plot.height) / span;

    for (int x = plot.x + 1; x < plot.right() - 1; x += 9) {
      line_segment(parent, x, y, std::min(4, plot.right() - x - 1), 1);
    }
    line_segment(parent, plot.right(), y - kAxisTickWidth / 2, kRateTickLength,
                 kAxisTickWidth);

    // Clamped so a mark at the very top or bottom of the plot keeps its label
    // inside the chart box instead of riding out over the row above.
    const int label_y = std::clamp(y - label_height / 2, plot.y,
                                   plot.bottom() - label_height);
    char value_text[12];
    std::snprintf(value_text, sizeof(value_text), "%.2f", value / 100.0);
    label(parent, value_text,
          {plot.right() + kRateTickLength + kRateScaleGap, label_y,
           kRateLabelWidth, label_height},
          small_font(), LV_TEXT_ALIGN_LEFT);
  }
}

// Ticks at the two ends of the time axis, placed outward from the plot so
// they mark the edges of the span rather than sitting inside the data.
void draw_span_ticks(lv_obj_t* parent, const Rect chart) {
  line_segment(parent, chart.x, chart.bottom(), kAxisTickWidth,
               kAxisTickHeight);
  line_segment(parent, chart.right() - kAxisTickWidth, chart.bottom(),
               kAxisTickWidth, kAxisTickHeight);
}

}  // namespace

void render_market(lv_obj_t* parent, const app_core::AppSnapshot& snapshot,
                   const app_core::MarketData& market, Rect bounds,
                   std::size_t page_index, std::size_t page_count,
                   bool us_market, UiContext* context) {
  (void)context;
  // Page position now lives in the system tray (see render_tray in
  // render_shared.cpp), not a corner overlay on the page itself.
  (void)page_index;
  (void)page_count;
  apply_surface(parent);
  const MarketLayout layout = market_layout(bounds);
  const Rect primary = layout.primary;

  // The title and the date split the row. They used to share one rect, and
  // because every label paints an opaque background the date simply covered
  // the title - the exact failure the board skill warns against fixing with
  // paint order.
  const int header_width = primary.width - 16;
  const Rect title_rect{primary.x + 8, primary.y + 4, header_width * 3 / 5, 18};
  const Rect date_rect{title_rect.right(), primary.y + 4,
                       header_width - title_rect.width, 18};
  label(parent, us_market ? text(Text::TitleUsMarket) : text(Text::TitleUaFx),
        title_rect, small_font());

  // The session these figures come from, on every market page and in every
  // state. The US page has an intraday series and so draws a chart, which
  // implies "now" more strongly than a bare number does - and outside trading
  // hours that chart is last session's. Dating it is the difference between a
  // stale reading and a lie.
  //
  // Shown unconditionally rather than only when the date differs from today's:
  // deciding that needs the device's date against the exchange's, in different
  // timezones, and a wrong answer there fails silently in the direction of
  // saying nothing. One short line costs less than the comparison.
  const std::string as_of = market_as_of_short(market);
  if (market.valid && !as_of.empty()) {
    label(parent, as_of.c_str(), date_rect, small_font(),
          LV_TEXT_ALIGN_RIGHT);
  }

  if (!market.valid) {
    // No fabricated primary_value/percent/chart below the title - see
    // ui_data.hpp no_data_rect for the shared placeholder geometry.
    label(parent, text(Text::NoData), no_data_rect(primary), medium_font(),
          LV_TEXT_ALIGN_CENTER);
  } else {
    // The axis labels below the chart get whatever label() will actually
    // give them, not what we ask for: a 17 px box grows to
    // font.line_height + 2 and the extra row used to land one pixel past the
    // safe canvas. Reserve the real height here so the chart yields the row
    // instead.
    const int axis_height = safe_text_box_height(17, small_font()->line_height);
    const Rect chart = market_chart_rect(primary, small_font()->line_height);

    label(parent, market.primary_label.c_str(),
          {primary.x + 8, primary.y + 23, primary.width / 2 - 8, 30},
          medium_font());
    char value[24];
    // value_has_decimals: primary_value is hundredths of a unit (e.g. an
    // exchange rate) rather than a whole-number index point - see its own
    // comment in app_snapshot.hpp.
    if (market.value_has_decimals) {
      std::snprintf(value, sizeof(value), "%d.%02d", market.primary_value / 100,
                    std::abs(market.primary_value % 100));
    } else {
      std::snprintf(value, sizeof(value), "%d", market.primary_value);
    }
    label(parent, value,
          {primary.x + primary.width / 2, primary.y + 20,
           primary.width / 2 - 8, 32},
          large_font(), LV_TEXT_ALIGN_RIGHT);
    // has_change: no fabricated "+0.00%" when this refresh has no honest
    // change figure to report - see its own comment in app_snapshot.hpp.
    if (market.has_change) {
      char change[24];
      std::snprintf(change, sizeof(change), "%+.2f%%",
                    market.primary_change_percent);
      label(parent, change,
            {primary.x + primary.width / 2, primary.y + 52,
             primary.width / 2 - 8, 18},
            small_font(), LV_TEXT_ALIGN_RIGHT);
    }
    divider(parent, {primary.x + 8, primary.y + 66, primary.width - 16,
                     kSeparatorWidth});

    if (market.has_intraday) {
      // The grid and the 開盤/盤中/收盤 labels below stay at the chart's
      // full width regardless - they are the session's time axis, not a
      // claim about how much of it the samples cover. Only the polyline
      // itself is narrowed:
      //
      // - Width, by market.session_elapsed_fraction: a session still in
      //   progress only ever has samples up through "now", and stretching
      //   those across the full axis would claim a finished trading day
      //   that has not happened yet (see app_snapshot.hpp's own comment on
      //   that field). A completed session, and the TWSE fallback, never
      //   see anything but the field's 1.0 default - full width, exactly
      //   as before.
      // - Point count, via normalize_chart_samples_n's own `count`
      //   parameter and market.intraday_sample_count: early in a session
      //   there may be fewer real bars than the array's target resolution
      //   (app_core::kIntradaySampleCount), and the unused trailing slots
      //   must not be read as real, zero-valued data.
      const float fraction =
          std::clamp(market.session_elapsed_fraction, 0.0f, 1.0f);
      // The scale's gutter comes off the plot before anything is laid out, so
      // the grid, the polyline and the dates below all share one width and
      // the labels sit beside the data rather than on it.
      const Rect plot = rate_plot_rect(chart);
      const Rect data_extent{
          plot.x, plot.y,
          std::max(1, static_cast<int>(plot.width * fraction)), plot.height};
      // Grid first, then the curve over it: the polyline is transparent now
      // (see its own comment), so this order is what puts the data on top of
      // its own reference lines rather than under them.
      draw_rate_grid(parent, plot,
                     market_intraday_range(market.intraday_samples,
                                           market.intraday_sample_count),
                     axis_height);
      polyline(parent,
              normalize_chart_samples_n(market.intraday_samples, data_extent,
                                        market.intraday_sample_count),
              market.intraday_sample_count, data_extent);
      draw_span_ticks(parent, plot);
      // What the axis is allowed to say depends on what the series is.
      //
      // "OPEN / MID / CLOSE" names the phases of one trading session. Under a
      // month of daily closes it is simply false, so a daily series gets the
      // span's own first and last dates instead - taken from the rows the
      // provider returned, never from the device clock, the same rule as the
      // as-of line above. No middle label: the midpoint of a run of banking
      // days is not a date anybody published.
      if (market.series_is_daily) {
        char from[8];
        char to[8];
        std::snprintf(from, sizeof(from), "%02u.%02u",
                      market.series_first_day, market.series_first_month);
        std::snprintf(to, sizeof(to), "%02u.%02u", market.series_last_day,
                      market.series_last_month);
        label(parent, from, {plot.x, plot.bottom() + 1, plot.width / 3,
                             axis_height},
              small_font());
        label(parent, to,
              {plot.x + 2 * plot.width / 3, plot.bottom() + 1,
               plot.width / 3, axis_height},
              small_font(), LV_TEXT_ALIGN_RIGHT);
      } else {
        label(parent, text(Text::ChartOpen),
              {plot.x, plot.bottom() + 1, plot.width / 3, axis_height},
              small_font());
        label(parent, text(Text::ChartMid),
              {plot.x + plot.width / 3, plot.bottom() + 1, plot.width / 3,
               axis_height},
              small_font(), LV_TEXT_ALIGN_CENTER);
        label(parent, text(Text::ChartClose),
              {plot.x + 2 * plot.width / 3, plot.bottom() + 1,
               plot.width / 3, axis_height},
              small_font(), LV_TEXT_ALIGN_RIGHT);
      }
    } else {
      // A daily-close-only source (e.g. TWSE) has no intraday series - the
      // figures above are real, but drawing a flat repeat of the close would
      // read as "the market did not move" rather than "no intraday data
      // exists". Skip the chart, grid, and axis labels; say so instead.
      // Naming the session is the point. A closed market is the normal
      // weekend state, and a page showing Thursday's close with no date
      // invites reading it as today's - the figures are real either way, but
      // only one of those is honest about when.
      const Rect placeholder = chart_placeholder_rect(chart);
      const std::string as_of = market_as_of_text(market);
      if (as_of.empty()) {
        label(parent, text(Text::NoIntradayData), placeholder, small_font(),
              LV_TEXT_ALIGN_CENTER);
      } else {
        label(parent, as_of.c_str(),
              {placeholder.x, placeholder.y - 12, placeholder.width, 30},
              medium_font(), LV_TEXT_ALIGN_CENTER);
        label(parent, text(Text::NoIntradayData),
              {placeholder.x, placeholder.y + 20, placeholder.width,
               placeholder.height - 20},
              small_font(), LV_TEXT_ALIGN_CENTER);
      }
    }
  }

  render_market_sidebar(parent, snapshot, market, layout.side, us_market);
}

}  // namespace ui
