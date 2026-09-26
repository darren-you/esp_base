#pragma once

#include "esp_base_storage_owner.h"
#include "esp_base_ota_receipt.h"
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
bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_receipt_snapshot_t *snapshot);
typedef enum {
    ESP_BASE_CONTAINER_RETIRE_COMPLETE = 0,
    ESP_BASE_CONTAINER_RETIRE_BLOCKED,
    ESP_BASE_CONTAINER_RETIRE_UNCERTAIN,
} esp_base_container_retire_result_t;
esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32]);
