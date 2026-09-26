#pragma once

#include <stdbool.h>

#include "esp_base_storage_owner.h"
#include "esp_base_ota_receipt.h"

typedef enum {
    ESP_BASE_CONTAINER_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_BLOCKED,
    ESP_BASE_CONTAINER_EMPTY,
    ESP_BASE_CONTAINER_RUNNING,
} esp_base_container_boot_result_t;

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37]);
esp_base_container_boot_result_t esp_base_container_product_start_trial(
    const esp_base_storage_claim_t *claim, const char boot_id[37]);
bool esp_base_container_product_mark_healthy(const esp_base_storage_claim_t *claim);
bool esp_base_container_product_confirm_firmware(const esp_base_storage_claim_t *claim);
bool esp_base_container_product_stop_trial(const esp_base_storage_claim_t *claim);
bool esp_base_container_product_configured(void);
bool esp_base_container_product_verify_selected_ota(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt, eota_state_t running_state);
typedef enum {
    ESP_BASE_CONTAINER_RETIRE_COMPLETE = 0,
    ESP_BASE_CONTAINER_RETIRE_BLOCKED,
    ESP_BASE_CONTAINER_RETIRE_UNCERTAIN,
} esp_base_container_retire_result_t;
esp_base_container_retire_result_t esp_base_container_product_recover_retired_firmware(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32], const uint8_t candidate_sha256[32],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const char boot_id[37]);
