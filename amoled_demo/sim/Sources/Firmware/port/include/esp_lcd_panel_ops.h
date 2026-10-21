#pragma once
#include "esp_lcd_panel_io.h"
typedef enum { LCD_RGB_ELEMENT_ORDER_RGB, LCD_RGB_ELEMENT_ORDER_BGR } lcd_rgb_element_order_t;
typedef struct {
    int reset_gpio_num;
    lcd_rgb_element_order_t rgb_ele_order;
    unsigned bits_per_pixel;
    void *vendor_config;
} esp_lcd_panel_dev_config_t;
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p);
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t p, int x, int y);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0, int x1, int y1, const void *data);
