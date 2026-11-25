// SPDX-License-Identifier: Apache-2.0
// Composition root: Recovery stays reachable first, then the Mosaico shell
// (launcher, Glimdock and Swifty Home) takes the display.
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "boot_stage.h"
#include "mosaico_shell.h"
#include "nvs_flash.h"

static const char *TAG = "glimdock";

void app_main(void)
{
    /* Wi-Fi start-up chatter would overflow the retained ESP-Iris log ring. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* Never erase NVS to recover: it holds Glimdock and Home pairings. */
        ESP_LOGE(TAG, "NVS unavailable (%s); Recovery remains active", esp_err_to_name(err));
    }
    /* Recovery must remain reachable even when the UI cannot start. */
    iris_ota_support_start();
    boot_stage_register();
    boot_stage("shell start");
    err = mosaico_shell_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shell unavailable (%s); Recovery remains active", esp_err_to_name(err));
        return;
    }
    boot_stage("ready");
    ESP_LOGI(TAG, "Glimdock + Home ready");
}
