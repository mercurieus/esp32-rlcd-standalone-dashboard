#include "ui_app.hpp"
#include "ui_fonts.hpp"

#include <cstdio>

namespace ui {
namespace {

const lv_font_t* hero_font() { return font_large(); }
const lv_font_t* medium_font() { return font_medium(); }
const lv_font_t* small_font() { return font_small(); }

}  // namespace

void render_weather(lv_obj_t* parent, const app_core::AppSnapshot& snapshot,
                    Rect bounds, std::size_t page_index,
                    std::size_t page_count, UiContext* context) {
  (void)context;
  // Page position now lives in the system tray (see render_tray in
  // render_shared.cpp), not a corner overlay on the page itself.
  (void)page_index;
  (void)page_count;
  apply_surface(parent);
  const auto& weather = snapshot.weather;

  if (!weather.valid) {
    // No fabricated condition/temperature/rain% or seven-day forecast - see
    // ui_data.hpp no_data_rect for the shared placeholder geometry. Weather
    // has no other title text (the condition line normally serves that
    // role), so give it the same small title row the market/indoor pages
    // use.
    label(parent, text(Text::TileWeather), {bounds.x + 8, bounds.y + 4, bounds.width - 16, 18},
          small_font());
    label(parent, text(Text::NoData), no_data_rect(bounds), medium_font(),
          LV_TEXT_ALIGN_CENTER);
    return;
  }

  const auto& current = weather.current;
  // Grown from the original 34x31 - too small and too fine to read on this
  // panel. Still clears the divider at bounds.y+70 with room to spare.
  weather_icon(parent, {bounds.x + 8, bounds.y + 8, 48, 46},
              weather_icon_kind_for_condition(current.condition));
  // One line, widened to fit it. Wrapping was the previous answer and it was
  // wrong: at 28px two lines need 56px and the box is 35px, so "Partly Cloudy"
  // rendered as "Partly" with the top of the second line sliced off. There was
  // no vertical room to give - the location sits at y+41 and the divider at
  // y+70 - but there was horizontal room, because nothing occupies y+4..y+39
  // out to the right edge. 264px takes the longest WMO wordings at 28px with
  // room to spare, and LONG_DOT means anything longer still ellipsises inside
  // its own row instead of bleeding into the one below.
  // Line one is the headline: what the weather is, and how warm it is.
  //
  // The temperature used to sit in the sub-row beside the rain figure, at
  // small font, which made the page's most-read number its least visible
  // one. It is a caption to the condition and is now set like one.
  //
  // The condition keeps the left of the row and yields what the reading
  // needs; both ellipsise rather than collide, and the longest WMO wordings
  // still fit 200px at 28px.
  label(parent, current.condition.c_str(),
        {bounds.x + 66, bounds.y + 4, 200, 35}, hero_font());
  label(parent, temperature_text(current.temperature_c, 1).c_str(),
        {bounds.x + 270, bounds.y + 4, bounds.width - 278, 35}, hero_font(),
        LV_TEXT_ALIGN_RIGHT);

  // The city, which until now was an empty label: nothing ever assigned
  // WeatherCurrent::location. It comes from the geolocation response the
  // coordinates already came from, and stays empty on the manual-location
  // path, which never asks anyone what the place is called.
  //
  // 114px, measured against a real place rather than a short one. 72px was
  // sized for "Kyiv" (about 32px) and the board answered with the city it
  // was actually in:
  //
  //   clipped: "Chernivtsi" needs 104px, box gives 70px
  //
  // Moving the temperature up to line one is what paid for this - the
  // sub-row no longer carries it, so the width went to the caption that
  // needed it.
  label(parent, current.location.c_str(),
        {bounds.x + 68, bounds.y + 41, 114, 20}, medium_font());

  // What it feels like, the chance of rain, and when this was fetched - the
  // qualifiers, on the row under the headline they qualify.
  //
  // FEELS is left untranslated to match the RAIN that was already here.
  // has_feels_like rather than a sentinel: a provider that omits the field
  // must show nothing, and 0.0 C is an ordinary reading (see WeatherCurrent).
  //
  // The stamp is drawn only with a synced clock (see
  // WeatherData::fetched_time_known) - an unsynced board shows no time
  // rather than a compile-time guess dressed up as a fetch time. The time
  // only, no date: a date needs about 90px this page has nowhere to take
  // from, and `stale` already carries "old enough to distrust".
  char current_line[96];
  char stamp[12] = "";
  if (weather.fetched_time_known) {
    std::snprintf(stamp, sizeof(stamp), "  %02u:%02u", weather.fetched_hour,
                  weather.fetched_minute);
  }
  if (current.has_feels_like) {
    std::snprintf(current_line, sizeof(current_line),
                  "FEELS %s  RAIN %u%%%s%s",
                  temperature_text(current.feels_like_c, 1).c_str(),
                  current.rain_probability_percent,
                  weather.stale ? text(Text::StaleSuffix) : "", stamp);
  } else {
    std::snprintf(current_line, sizeof(current_line), "RAIN %u%%%s%s",
                  current.rain_probability_percent,
                  weather.stale ? text(Text::StaleSuffix) : "", stamp);
  }
  // The row this lands in:
  // 192px of text, which takes today's "FEELS 12.8°C  RAIN 3%  01:52" (about
  // 175px, extrapolated from the 201px the panel measured for a longer
  // string) with room over. A winter extreme - two negative two-digit
  // temperatures and a three-digit rain figure - runs about 215px and will
  // ellipsise, logged. That is the deliberate end of the trade: the city
  // beside it is a name that must be readable every day, not only on the
  // coldest one.
  label(parent, current_line,
        {bounds.x + 186, bounds.y + 42, bounds.width - 194, 20}, small_font(),
        LV_TEXT_ALIGN_RIGHT);
  divider(parent, {bounds.x + 8, bounds.y + 70, bounds.width - 16,
                   kSeparatorWidth});

  const Rect forecast = weather_forecast_rect(bounds);
  const auto columns = forecast_columns(forecast);
  for (std::size_t index = 0; index < columns.size(); ++index) {
    const Rect column = columns[index];
    const auto& day = snapshot.weather.seven_day[index];
    const ForecastColumnLayout layout = forecast_column_layout(column);
    label(parent, day.day.c_str(), layout.day, small_font(),
          LV_TEXT_ALIGN_CENTER);
    weather_icon(parent, layout.icon,
                weather_icon_kind_for_condition(day.condition));
    label(parent, forecast_condition_short(day.condition),
          {layout.condition.x + 1, layout.condition.y,
           layout.condition.width - 2, layout.condition.height},
          small_font(), LV_TEXT_ALIGN_CENTER);
    // High above low - a "H68 L54" side-by-side pair was too cramped for
    // the ~55px column width seven days across the safe canvas leaves.
    char high[16];
    std::snprintf(high, sizeof(high), "H%.0f", day.high_c);
    label(parent, high,
          {layout.high.x + 1, layout.high.y, layout.high.width - 2,
           layout.high.height},
          small_font(), LV_TEXT_ALIGN_CENTER);
    char low[16];
    std::snprintf(low, sizeof(low), "L%.0f", day.low_c);
    label(parent, low,
          {layout.low.x + 1, layout.low.y, layout.low.width - 2,
           layout.low.height},
          small_font(), LV_TEXT_ALIGN_CENTER);
    char rain[16];
    std::snprintf(rain, sizeof(rain), "R%u%%",
                  day.rain_probability_percent);
    label(parent, rain,
          {layout.rain.x + 1, layout.rain.y, layout.rain.width - 2,
           layout.rain.height},
          small_font(), LV_TEXT_ALIGN_CENTER);
  }
}

}  // namespace ui
