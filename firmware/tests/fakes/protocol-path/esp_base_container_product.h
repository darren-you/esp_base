#pragma once

#include "esp_base_storage_owner.h"
#include "eota.h"

typedef enum {
    ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_STAGE_PREPARED,
    ESP_BASE_CONTAINER_STAGE_REJECTED,
    ESP_BASE_CONTAINER_STAGE_UNCERTAIN,
} esp_base_container_stage_result_t;

esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[37]);
bool esp_base_container_product_ota_ready(void);
bool esp_base_container_product_configured(void);
