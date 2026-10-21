#pragma once
// NVS kept in a file (~/Library/Application Support/BobbleSim/nvs.bin), same calls and behaviour as the board's
#include "esp_err.h"
#include <stddef.h>
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef enum {
    NVS_TYPE_U8 = 0x01, NVS_TYPE_I8 = 0x11, NVS_TYPE_U16 = 0x02, NVS_TYPE_I16 = 0x12, NVS_TYPE_U32 = 0x04, NVS_TYPE_I32 = 0x14,
    NVS_TYPE_U64 = 0x08, NVS_TYPE_I64 = 0x18, NVS_TYPE_STR = 0x21, NVS_TYPE_BLOB = 0x42, NVS_TYPE_ANY = 0xff
} nvs_type_t;
#define NVS_DEFAULT_PART_NAME "nvs"
#define NVS_KEY_NAME_MAX_SIZE 16
typedef struct { char namespace_name[16]; char key[NVS_KEY_NAME_MAX_SIZE]; nvs_type_t type; } nvs_entry_info_t;
typedef struct sim_nvs_it *nvs_iterator_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
#define SIM_NVS_INT(T, n) esp_err_t nvs_get_##n(nvs_handle_t h, const char *key, T *out); esp_err_t nvs_set_##n(nvs_handle_t h, const char *key, T v);
SIM_NVS_INT(uint8_t, u8) SIM_NVS_INT(int8_t, i8) SIM_NVS_INT(uint16_t, u16) SIM_NVS_INT(int16_t, i16)
SIM_NVS_INT(uint32_t, u32) SIM_NVS_INT(int32_t, i32) SIM_NVS_INT(uint64_t, u64) SIM_NVS_INT(int64_t, i64)
esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *out);
esp_err_t nvs_entry_next(nvs_iterator_t *it);
esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *out);
void nvs_release_iterator(nvs_iterator_t it);
