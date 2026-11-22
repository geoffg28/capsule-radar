#pragma once
// Airport markers for the radar scope. Projects the embedded airport list
// (airports_data.h: every US public-use airport from the FAA, the main fields elsewhere
// from OurAirports) like the coastline: cull to the scope, great-circle project, cache
// screen markers, draw in the static chrome layer. Every entry gets the same size ring;
// towered and non-towered fields are told apart by color, as on a US VFR sectional.
// Towered fields are always labelled; non-towered ones only at a display range of
// airports_label_max_nm() or closer (board default AIRPORT_LABEL_UNTOWERED_NM_DEFAULT).
// Projection is done only on a home/range change, never per frame.
#include <lvgl.h>

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx);

// Label non-towered fields at this display range (NM) or closer: 0 = never,
// AIRPORT_LABEL_ALWAYS_NM = always. Takes effect on the next airports_draw().
void airports_set_label_max_nm(int nm);
int  airports_label_max_nm();

// font: the identifier label font (the radar passes a bigger one in large-text mode).
void airports_draw(lv_draw_ctx_t *ctx, lv_color_t colorTowered, lv_color_t colorUntowered,
                   lv_opa_t opa, const lv_font_t *font);

// Find the nearest recognizable airport (one with an IATA code). Used by the
// weather view for aviation context; entirely offline from the embedded data.
bool airports_nearest_iata(double lat, double lon, float maxKm,
                           char iata[4], float *distKm, float *bearingDeg);
