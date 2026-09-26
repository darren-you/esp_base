#pragma once

#include <stdbool.h>

#include "esp_base_storage_owner.h"

typedef enum {
    ESP_BASE_CONTAINER_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_BLOCKED,
    ESP_BASE_CONTAINER_EMPTY,
    ESP_BASE_CONTAINER_RUNNING,
} esp_base_container_boot_result_t;

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim);
bool esp_base_container_product_pending_blocked(void);
