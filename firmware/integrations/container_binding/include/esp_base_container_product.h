// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_base_storage_owner.h"

typedef enum {
    ESP_BASE_CONTAINER_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_BLOCKED,
    ESP_BASE_CONTAINER_EMPTY,
    ESP_BASE_CONTAINER_RUNNING,
} esp_base_container_boot_result_t;

/* Called with Base's already active boot claim after NVS and control start.
 * A fully specified product policy binds exact real package/NVS partitions,
 * then opens and initializes a persisted confirmed package on one pthread.
 * EMPTY/RUNNING/BLOCKED require the caller to retain its claim until joint
 * OTA is wired. NOT_CONFIGURED keeps the no-package layout manageable. */
esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim);

/* Pending firmware requires Container's durable joint-operation contract.
 * Until that API is integrated, it cannot run a guest or pass Base's local
 * confirmation gate. */
bool esp_base_container_product_pending_blocked(void);
