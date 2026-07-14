#ifndef LV_CHANNEL_CARD_H
#define LV_CHANNEL_CARD_H

#include <lvgl.h>
#include "model.h"

#define CARD_HEIGHT    72   /* cover + title + two info lines */
#define CARD_COVER_SZ  60   /* square cover */
#define CARD_PAD        6

/* Set to 1 to load cached artwork PNGs. 0 = random solid color (much faster). */
#define LV_CHANNEL_CARD_SHOW_ARTWORK  0

lv_obj_t* lv_channel_card_create(lv_obj_t* parent, const Channel* album);

#endif
