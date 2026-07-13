#pragma once
// Airport markers for the radar scope. Projects the embedded OurAirports list
// (airports_data.h) like the coastline: cull to the scope, great-circle project,
// cache screen markers, draw in the static chrome layer. Every entry gets the same
// size ring + IATA label; large airports (hubs) and medium airports (regional/
// Class C-D fields) are told apart by color only. Projection is done only on a
// home/range change, never per frame.
#include <lvgl.h>

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx);

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t colorLarge, lv_color_t colorMedium, lv_opa_t opa);
