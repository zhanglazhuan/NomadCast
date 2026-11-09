#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t sd_access_guard_init(void);
void sd_access_guard_block(bool blocked);
bool sd_access_guard_idle(void);
