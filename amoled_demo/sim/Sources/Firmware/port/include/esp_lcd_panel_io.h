#pragma once
#include "esp_err.h"
typedef struct sim_lcd_io *esp_lcd_panel_io_handle_t;
typedef struct sim_lcd_panel *esp_lcd_panel_handle_t;
typedef int esp_lcd_spi_bus_handle_t;
typedef struct { int dummy; } esp_lcd_panel_io_event_data_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx);
typedef struct {
    int cs_gpio_num, dc_gpio_num, spi_mode;
    unsigned pclk_hz;
    size_t trans_queue_depth;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    int lcd_cmd_bits, lcd_param_bits;
    struct { unsigned quad_mode : 1; } flags;
} esp_lcd_panel_io_spi_config_t;
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus, const esp_lcd_panel_io_spi_config_t *cfg, esp_lcd_panel_io_handle_t *out);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t size);
