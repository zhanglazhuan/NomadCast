#ifndef LV_CHANNEL_CARD_H
#define LV_CHANNEL_CARD_H

#include <lvgl.h>
#include "model.h"

#define CARD_HEIGHT    88   /* room for progress bar + icon rows */
#define CARD_COVER_SZ  60   /* square cover */
#define CARD_PAD        6

lv_obj_t* lv_channel_card_create(lv_obj_t* parent, const Channel* album);

#endif
