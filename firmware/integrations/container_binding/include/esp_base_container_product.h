// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_base_storage_owner.h"
#include "esp_base_ota_policy.h"
#include "eota.h"

typedef enum {
    ESP_BASE_CONTAINER_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_BLOCKED,
    ESP_BASE_CONTAINER_EMPTY,
    ESP_BASE_CONTAINER_RUNNING,
} esp_base_container_boot_result_t;

/* Called with Base's already active boot claim after NVS and control start.
 * A fully specified product policy binds exact real package/NVS partitions,
 * then opens and initializes a persisted confirmed package on one pthread.
 * EMPTY/RUNNING have finished storage admission and the caller releases its
 * claim. BLOCKED retains the claim because storage state may be uncertain.
 * The running guest never owns this app/otadata operation claim. */
esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37]);

/* PENDING_VERIFY starts only the exact PREPARED firmware transition and uses
 * this boot's protocol UUID. A no-package trial still advances durable state. */
esp_base_container_boot_result_t esp_base_container_product_start_trial(
    const esp_base_storage_claim_t *claim, const char boot_id[37]);
bool esp_base_container_product_mark_healthy(const esp_base_storage_claim_t *claim);
bool esp_base_container_product_confirm_firmware(const esp_base_storage_claim_t *claim);
/* Synchronize with the unique guest pthread before rejecting a pending app.
 * False means native reclamation or guest stop was not proven: retain owner
 * and do not ask IDF to roll back while candidate code may still execute. */
bool esp_base_container_product_stop_trial(const esp_base_storage_claim_t *claim);

/* A complete product policy is required for the persistent binding path.
 * NO_PACKAGE can enter firmware trial; package trials need a real authorized
 * business event source and are rejected before inactive-app writing. */
bool esp_base_container_product_configured(void);
/* Reject a blocked or uninitialized product before any inactive-app write. */
bool esp_base_container_product_ota_ready(void);

typedef enum {
    ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED = 0,
    ESP_BASE_CONTAINER_STAGE_PREPARED,
    ESP_BASE_CONTAINER_STAGE_REJECTED,
    ESP_BASE_CONTAINER_STAGE_UNCERTAIN,
} esp_base_container_stage_result_t;

/* Called by the OTA worker with its existing claim, after eota_prepare and
 * before eota_select. Only a real persisted running binding without a package
 * stages NO_PACKAGE. A package trial lacks an authorized business event source;
 * until that input exists, REUSE and WRITE are rejected before app Flash write. */
esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES]);
