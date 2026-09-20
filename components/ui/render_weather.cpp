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
  // 56x58, taking the whole column the text leaves free: the condition and
  // temperature line starts at bounds.x+66, and the divider is at
  // bounds.y+70, so x+6..x+62 and y+6..y+64 belong to the icon and nothing
  // else. Grown twice - 34x31, then 48x46 - each time because the icon read
  // as too fine on this panel, and this is the end of it: the rect is now
  // the free space rather than a guess inside it.
  weather_icon(parent, {bounds.x + 6, bounds.y + 6, 56, 58},
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

  // No leading space here any more: the stamp used to be appended straight
  // onto the measurement row, which needed its own separator. It is now a
  // field the city line joins with a space of its own.
  char stamp[12] = "";
  if (weather.fetched_time_known) {
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u", weather.fetched_hour,
                  weather.fetched_minute);
  }

  // The city and when the reading was fetched: where it came from and when,
  // on one line, with the row below left for the measurements themselves.
  //
  // The stamp used to live on that measurement row and could not stay there.
  // The three fields together need 335px of the 324px the row has, and the
  // panel had started saying so on an ordinary afternoon rather than at some
  // extreme:
  //
  //   clipped: "FEELS 11.4°C RAIN 100% 02:02" needs 205px, box gives 198px
  //
  // What paid for the move is this label dropping from medium to small.
  // "Chernivtsi" measures 104px at medium and 72px at small, and 72 + a space
  // + "02:02" is 118px - so place and time together now cost less than the
  // place alone did. The city is context rather than a reading, which is why
  // it is the one that gives up the size; the temperature beside it went the
  // other way for the same reason.
  //
  // 126px of box against 118px of worst measured text. A longer name than
  // this city's still ellipsises (LONG_DOT), as it did before at medium.
  char location_line[48];
  if (stamp[0] != '\0') {
    std::snprintf(location_line, sizeof(location_line), "%s %s",
                  current.location.c_str(), stamp);
  } else {
    std::snprintf(location_line, sizeof(location_line), "%s",
                  current.location.c_str());
  }
  label(parent, location_line, {bounds.x + 68, bounds.y + 41, 126, 20},
        small_font());

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
  if (current.has_feels_like) {
    std::snprintf(current_line, sizeof(current_line),
                  "FEELS %s RAIN %u%%%s",
                  temperature_text(current.feels_like_c, 1).c_str(),
                  current.rain_probability_percent,
                  weather.stale ? text(Text::StaleSuffix) : "");
  } else {
    std::snprintf(current_line, sizeof(current_line), "RAIN %u%%%s",
                  current.rain_probability_percent,
                  weather.stale ? text(Text::StaleSuffix) : "");
  }
  // The row this lands in, sized from lv_font_montserrat_14's glyph table
  // rather than extrapolated. The previous comment here put today's string at
  // "about 175px" and sized the box at 192px on that basis; the panel then
  // measured a plain summer reading, "FEELS 24.1°C  RAIN 0%  17:36", at
  // 199px and ellipsised it. An estimate was standing in for a measurement,
  // which is the one thing this project's layouts are not allowed to do.
  //
  // Measured, at single-space separators:
  //   FEELS 24.1°C RAIN 0% 17:36     191.5px  (an ordinary day)
  //   FEELS -9.9°C RAIN 100% 04:44   213.2px  (a wet winter one)
  //
  // The box gives 198px of text, so ordinary readings now fit with room and
  // the wet-winter extreme still ellipsises, logged. That remains a
  // deliberate trade rather than an oversight - the city beside it is a name
  // that must be readable every day, not only on the mild ones - but it is
  // now a trade made against measured numbers, and the case it gives up is a
  // genuine extreme rather than an afternoon in September.
  //
  // Width came from two places: the city box dropped from 114 to 108 (it
  // needs 104 for "Chernivtsi" in medium) and the separators went from two
  // spaces to one.
  // 186px of box, against 173px for the widest this row can now hold
  // ("FEELS -24.1°C RAIN 100%", measured from the glyph table). The stamp's
  // departure is worth 46px and the city's demotion another 32, so the case
  // that was ellipsising today now clears by a margin rather than by a pixel.
  label(parent, current_line,
        {bounds.x + 198, bounds.y + 42, bounds.width - 202, 20}, small_font(),
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
    // Only when the date actually parsed - see WeatherDay::day_number. An
    // unparsed slot shows its weekday and nothing under it, rather than a
    // day "0" that no calendar has.
    if (day.day_number != 0) {
      char date[4];
      std::snprintf(date, sizeof(date), "%u",
                    static_cast<unsigned>(day.day_number));
      label(parent, date, layout.date, small_font(), LV_TEXT_ALIGN_CENTER);
    }
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
