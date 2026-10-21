#pragma once
// The battery reading (ADC1 channel 3, through the board's divide-by-3): the emulator's battery voltage
#include "esp_err.h"
typedef enum { ADC_UNIT_1, ADC_UNIT_2 } adc_unit_t;
typedef enum { ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3, ADC_CHANNEL_4 } adc_channel_t;
typedef enum { ADC_ATTEN_DB_0, ADC_ATTEN_DB_2_5, ADC_ATTEN_DB_6, ADC_ATTEN_DB_12, ADC_ATTEN_DB_11 = ADC_ATTEN_DB_12 } adc_atten_t;
typedef enum { ADC_BITWIDTH_DEFAULT = 0, ADC_BITWIDTH_12 = 12 } adc_bitwidth_t;
typedef enum { ADC_ULP_MODE_DISABLE } adc_ulp_mode_t;
typedef struct sim_adc *adc_oneshot_unit_handle_t;
typedef struct { adc_unit_t unit_id; int clk_src; adc_ulp_mode_t ulp_mode; } adc_oneshot_unit_init_cfg_t;
typedef struct { adc_atten_t atten; adc_bitwidth_t bitwidth; } adc_oneshot_chan_cfg_t;
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *out);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch, const adc_oneshot_chan_cfg_t *c);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *raw);
