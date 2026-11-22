#include "airports.h"
#include "airports_data.h"
#include "geo.h"
#include "config.h"
#include <vector>
#include <math.h>
#include <string.h>

struct Apt { lv_point_t pos; char ident[5]; uint8_t towered; };
static std::vector<Apt> s_apts;
static float s_rangeKm = 0;                                       // range of the last projection
static int   s_labelMaxNm = AIRPORT_LABEL_UNTOWERED_NM_DEFAULT;   // see airports_set_label_max_nm

void airports_set_label_max_nm(int nm) { s_labelMaxNm = nm; }
int  airports_label_max_nm() { return s_labelMaxNm; }

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx) {
    s_apts.clear();
    s_rangeKm = (float)rangeKm;
    if (rangeKm <= 0) return;

    const double rangeDeg  = rangeKm / 111.0;
    const double latMargin = rangeDeg * 1.05;
    const double cosLat    = cos(homeLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    for (int i = 0; i < AIRPORT_NUM; ++i) {
        const double lat = AIRPORT_LAT[i] / (double)AIRPORT_SCALE;
        const double lon = AIRPORT_LON[i] / (double)AIRPORT_SCALE;
        const double dlon = lon - homeLon;
        if (fabs(lat - homeLat) > latMargin) continue;                      // cheap bbox reject
        if (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin) continue;
        const double dist = geo::haversineKm(homeLat, homeLon, lat, lon);
        if (dist > rangeKm) continue;                                       // only inside the scope
        const double brg = geo::bearingDeg(homeLat, homeLon, lat, lon);
        const double rPx = (dist / rangeKm) * rOuterPx;
        const double a   = brg * M_PI / 180.0;
        Apt ap;
        ap.pos.x = (lv_coord_t)lroundf((float)(cx + rPx * sin(a)));
        ap.pos.y = (lv_coord_t)lroundf((float)(cy - rPx * cos(a)));
        memcpy(ap.ident, AIRPORT_IDENT[i], 5);
        ap.towered = (AIRPORT_FLAGS[i] & AIRPORT_F_TOWERED) ? 1 : 0;
        s_apts.push_back(ap);
    }
}

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t colorTowered, lv_color_t colorUntowered,
                   lv_opa_t opa, const lv_font_t *font) {
    if (s_apts.empty()) return;

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.width = 2; ring.opa = opa;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.opa = opa; lbl.font = font;
    // Label box sized from the font, so a larger one isn't clipped: vertically centred on
    // the ring, wide enough for a four-character FAA identifier.
    const lv_coord_t halfH = (lv_coord_t)((font->line_height + 1) / 2);
    const lv_coord_t boxW  = (lv_coord_t)(font->line_height * 3);

    const bool labelUntowered = s_labelMaxNm > 0 && s_rangeKm <= s_labelMaxNm * NM_TO_KM + 0.01f;

    // Non-towered first, so a towered field (and its label) wins where they overlap.
    for (int pass = 0; pass < 2; ++pass) {
        for (const Apt &ap : s_apts) {
            if (ap.towered != pass) continue;
            const lv_color_t c = ap.towered ? colorTowered : colorUntowered;
            ring.color = c;
            lv_draw_arc(ctx, &ring, &ap.pos, 3, 0, 360);                    // small hollow ring
            if (ap.ident[0] && (ap.towered || labelUntowered)) {
                lbl.color = c;
                lv_area_t la = { (lv_coord_t)(ap.pos.x + 5),        (lv_coord_t)(ap.pos.y - halfH),
                                 (lv_coord_t)(ap.pos.x + 5 + boxW), (lv_coord_t)(ap.pos.y + halfH) };
                lv_draw_label(ctx, &lbl, &la, ap.ident, NULL);
            }
        }
    }
}

bool airports_nearest_iata(double lat, double lon, float maxKm,
                           char iata[4], float *distKm, float *bearingDeg) {
    if (iata) iata[0] = 0;
    double best = maxKm;
    int bestIdx = -1;
    for (int i = 0; i < AIRPORT_NUM; ++i) {
        if (!(AIRPORT_FLAGS[i] & AIRPORT_F_IATA)) continue;
        const double alat = AIRPORT_LAT[i] / (double)AIRPORT_SCALE;
        const double alon = AIRPORT_LON[i] / (double)AIRPORT_SCALE;
        const double d = geo::haversineKm(lat, lon, alat, alon);
        if (d < best) { best = d; bestIdx = i; }
    }
    if (bestIdx < 0) return false;
    if (iata) { memcpy(iata, AIRPORT_IDENT[bestIdx], 3); iata[3] = 0; }
    if (distKm) *distKm = (float)best;
    if (bearingDeg) {
        const double alat = AIRPORT_LAT[bestIdx] / (double)AIRPORT_SCALE;
        const double alon = AIRPORT_LON[bestIdx] / (double)AIRPORT_SCALE;
        *bearingDeg = (float)geo::bearingDeg(lat, lon, alat, alon);
    }
    return true;
}
