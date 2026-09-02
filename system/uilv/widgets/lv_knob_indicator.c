#include "lv_knob_indicator.h"

lv_obj_t *lv_knob_indicator_create(lv_obj_t *parent)
{
	lv_obj_t *ind = lv_obj_create(parent);
	lv_obj_set_size(ind, 40, 40);
	return ind;
}
