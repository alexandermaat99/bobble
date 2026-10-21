// Over-the-air updates: POST the firmware .bin to http://bobble.local/ota with the
// X-OTA-Token header (ota.sh does this). The image streams into the spare OTA slot; on success we
// switch to it and reboot. A new image has to run for 30 s before it's confirmed, otherwise the
// bootloader rolls back to the previous one.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "ota.h"
#include "secrets.h"
#include "agent.h"

static const char *TAG = "ota";
static volatile int progress = -1;

int ota_progress(void) { return progress; }

static void reboot_soon(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!agent_token_ok(req)) {                        // (the paired Mac, or the development token)
        ESP_LOGW(TAG, "rejected upload: bad token");
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "bad token\n");
    }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || req->content_len <= 0 || req->content_len > slot->size)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no OTA slot, or image doesn't fit");

    esp_ota_handle_t ota;
    if (esp_ota_begin(slot, req->content_len, &ota) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, slot->label);
    progress = 0;

    EXT_RAM_BSS_ATTR static char buf[4096];
    int left = req->content_len, timeouts = 0;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof buf ? left : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0 || esp_ota_write(ota, buf, n) != ESP_OK) {
            esp_ota_abort(ota);
            progress = -1;
            ESP_LOGE(TAG, "upload interrupted");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
        }
        left -= n;
        progress = (int)(100LL * (req->content_len - left) / req->content_len);
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(slot) != ESP_OK) {   // end() validates the image
        progress = -1;
        ESP_LOGE(TAG, "image rejected");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image rejected (corrupt or wrong chip)");
    }
    ESP_LOGI(TAG, "update ok, rebooting into %s", slot->label);
    httpd_resp_sendstr(req, "ok, rebooting\n");
    xTaskCreate(reboot_soon, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void ota_register(httpd_handle_t server)
{
    const httpd_uri_t u = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    httpd_register_uri_handler(server, &u);
}

// Called from the main loop: confirm a freshly installed image once it has run for a while
void ota_confirm_if_healthy(void)
{
    static bool done;
    if (done || esp_timer_get_time() < 30 * 1000000LL) return;
    done = true;
    esp_ota_img_states_t st;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

const char *ota_build_time(void)
{
    static char s[32];
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s, sizeof s, "%s %.5s", d->date, d->time);
    return s;
}
