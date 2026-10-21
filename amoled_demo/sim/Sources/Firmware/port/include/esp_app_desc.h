#pragma once
#include <stdint.h>
typedef struct { uint32_t magic_word; char version[32], project_name[32], time[16], date[16], idf_ver[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
