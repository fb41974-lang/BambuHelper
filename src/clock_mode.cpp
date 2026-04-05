#include "clock_mode.h"
#include "display_ui.h"
#include "settings.h"
#include "config.h"
#include "layout.h"
#include <time.h>

static int prevMinute = -1;

static void drawBambuMiniLogoClock(int16_t cx, int16_t cy, uint16_t color, uint16_t bg) {
  const int16_t bw = 14;
  const int16_t bh = 36;
  const int16_t gap = 5;
  const int16_t x0 = cx - bw - gap / 2;
  const int16_t x1 = cx + gap / 2;
  const int16_t y  = cy - bh / 2;

  tft.fillRect(x0, y, bw, bh, color);
  tft.fillRect(x1, y, bw, bh, color);

  const int16_t sl = 5, th = 3;

  auto slash_fwd = [&](int16_t x, int16_t lc) {
    tft.fillTriangle(x,    lc-th,    x+bw, lc-sl-th, x,    lc,    bg);
    tft.fillTriangle(x+bw, lc-sl-th, x,    lc,       x+bw, lc-sl, bg);
  };

  auto slash_rev = [&](int16_t x, int16_t lc) {
    tft.fillTriangle(x,    lc-th,    x+bw, lc+sl-th, x,    lc,    bg);
    tft.fillTriangle(x+bw, lc+sl-th, x,    lc,       x+bw, lc+sl, bg);
  };

  slash_fwd(x0, y + 22);   // left bar: '/' cut
  slash_rev(x1, y + 14);   // right bar: '\\' cut higher than left slash
}

void resetClock() {
  prevMinute = -1;
}

void drawClock() {
  struct tm now;
  if (!getLocalTime(&now, 0)) {
    // getLocalTime() fails when SNTP sync status is reset (e.g., after a
    // timezone change). Fall back to the raw system clock, which remains
    // valid even without a completed NTP sync.
    time_t t = time(nullptr);
    if (t < 1600000000UL) return;  // sanity: before Sep 2020 = clock not set
    localtime_r(&t, &now);
  }

  // Only redraw when minute changes (resetClock() forces redraw)
  if (now.tm_min == prevMinute) return;
  prevMinute = now.tm_min;

  uint16_t bg = dispSettings.bgColor;

  // Clear clock area
  tft.fillRect(0, LY_CLK_CLEAR_Y, LY_W, LY_CLK_CLEAR_H, bg);

  // Time — large 7-segment font
  char timeBuf[12];
  if (netSettings.use24h) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", now.tm_hour, now.tm_min);
  } else {
    int h = now.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(timeBuf, sizeof(timeBuf), "%2d:%02d", h, now.tm_min);
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(7);
  tft.setTextColor(CLR_TEXT, bg);
  tft.drawString(timeBuf, LY_W / 2, LY_CLK_TIME_Y);

  // AM/PM indicator for 12h mode
  if (!netSettings.use24h) {
    tft.setTextFont(4);
    tft.setTextColor(CLR_TEXT_DIM, bg);
    tft.drawString(now.tm_hour < 12 ? "AM" : "PM", LY_W / 2, LY_CLK_AMPM_Y);
  }

  // Date — smaller font below
  const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  char dateBuf[24];
  int day = now.tm_mday, mon = now.tm_mon + 1, year = now.tm_year + 1900;
  switch (netSettings.dateFormat) {
    case 1:  snprintf(dateBuf, sizeof(dateBuf), "%s  %02d-%02d-%04d", days[now.tm_wday], day, mon, year); break;
    case 2:  snprintf(dateBuf, sizeof(dateBuf), "%s  %02d/%02d/%04d", days[now.tm_wday], mon, day, year); break;
    case 3:  snprintf(dateBuf, sizeof(dateBuf), "%s  %04d-%02d-%02d", days[now.tm_wday], year, mon, day); break;
    case 4:  snprintf(dateBuf, sizeof(dateBuf), "%s  %d %s %04d", days[now.tm_wday], day, months[now.tm_mon], year); break;
    case 5:  snprintf(dateBuf, sizeof(dateBuf), "%s  %s %d, %04d", days[now.tm_wday], months[now.tm_mon], day, year); break;
    default: snprintf(dateBuf, sizeof(dateBuf), "%s  %02d.%02d.%04d", days[now.tm_wday], day, mon, year); break;
  }
  tft.setTextFont(4);
  tft.setTextColor(CLR_TEXT_DIM, bg);
  tft.drawString(dateBuf, LY_W / 2, LY_CLK_DATE_Y);

  // Small brand mark above time
  drawBambuMiniLogoClock(LY_W / 2, LY_CLK_CLEAR_Y + 12, CLR_GREEN, bg);
}
