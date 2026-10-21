#pragma once
// The CO5300 AMOLED (driven as an SH8601): drawing lands in the emulator's frame buffer
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
typedef struct { int cmd; const void *data; size_t data_bytes; unsigned int delay_ms; } sh8601_lcd_init_cmd_t;
typedef struct { const sh8601_lcd_init_cmd_t *init_cmds; uint16_t init_cmds_size; struct { unsigned use_qspi_interface : 1; } flags; } sh8601_vendor_config_t;
esp_err_t esp_lcd_new_panel_sh8601(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *cfg, esp_lcd_panel_handle_t *out);
#define SH8601_PANEL_BUS_QSPI_CONFIG(clk, d0, d1, d2, d3, max) (spi_bus_config_t){.max_transfer_sz = (max)}
#define SH8601_PANEL_IO_QSPI_CONFIG(cs, cb, ctx) (esp_lcd_panel_io_spi_config_t){.cs_gpio_num = (cs), .on_color_trans_done = (cb), .user_ctx = (ctx)}
