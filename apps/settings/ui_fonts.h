/*
 * Font aliases for EPOS-originated code.
 * Original (720px screen):   Adapted (240×320):
 *   LV_FONT_TINY  = 18px   → lv_font_montserrat_14
 *   LV_FONT_SMALL = 24px   → lv_font_montserrat_16
 *   LV_FONT_NORMAL = 32px  → lv_font_montserrat_18
 */

#ifndef SETTINGS_UI_FONTS_H
#define SETTINGS_UI_FONTS_H

#include "lvgl.h"

/* Declare fonts that are enabled via Kconfig */
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);

#define LV_FONT_TINY   (&lv_font_montserrat_14)
#define LV_FONT_SMALL  (&lv_font_montserrat_16)
#define LV_FONT_NORMAL (&lv_font_montserrat_18)

#endif
