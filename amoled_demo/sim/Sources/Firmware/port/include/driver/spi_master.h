#pragma once
#include "esp_err.h"
typedef enum { SPI1_HOST, SPI2_HOST, SPI3_HOST } spi_host_device_t;
#define SPI_DMA_CH_AUTO 3
typedef struct { int max_transfer_sz; } spi_bus_config_t;
static inline esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int dma) { return ESP_OK; }
