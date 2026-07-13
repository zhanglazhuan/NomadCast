/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2022 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _CNV_DEBUG_H_
#define _CNV_DEBUG_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNV_DEBUG_INT16_DATA,
    CNV_DEBUG_INT32_DATA,
    CNV_DEBUG_FLOAT_DATA,
} cnv_debug_data_type_t;

/**
 * @brief Printing display direction
 */
typedef enum {
    CNV_DISPLAY_HORIZONTALLY,       /*!< Horizontal display */
    CNV_DISPLAY_PORTRAIT,           /*!< Portrait display */
} cnv_debug_display_t;

/**
 * @brief      Use this method to print the debugging information of source_data[]/fft_y_cf[]
 *
 * @param[in]  data         Address to start printing
 * @param[in]  data_len     Data length
 * @param[in]  tpye         Type of data
 * @param[in]  direction    Display direction: vertical display or horizontal display
 *
 * @return
 *     - ESP_OK
 */
esp_err_t cnv_debug_display(void *data, uint16_t data_len, cnv_debug_data_type_t tpye, cnv_debug_display_t direction);

#ifdef __cplusplus
}
#endif
#endif
