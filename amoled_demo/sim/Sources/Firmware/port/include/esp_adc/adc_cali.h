#pragma once
#include "esp_adc/adc_oneshot.h"
typedef struct sim_adc_cali *adc_cali_handle_t;
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv);
