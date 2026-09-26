// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_base_storage_owner.h"
#include "esp_base_ota_policy.h"
#include "esp_base_ota_receipt.h"
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

/* The caller holds Base's app/otadata claim. A CONFIRMED double observation
 * supplies exact signed source/inactive digests. With a product policy, the
 * existing ECS2 blob must reconcile, the running binding must have no package,
 * and its sequence is copied into the same durable OTA receipt before erase.
 * No policy still supplies the physical hashes with container_enabled=false. */
bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_receipt_snapshot_t *snapshot);

typedef enum {
    ESP_BASE_CONTAINER_RETIRE_COMPLETE = 0,
    ESP_BASE_CONTAINER_RETIRE_BLOCKED,
    ESP_BASE_CONTAINER_RETIRE_UNCERTAIN,
} esp_base_container_retire_result_t;

/* Called only after eota_retire_inactive has physically erased and read back
 * the exact inactive app under this claim. It accepts the receipt's A/B -> A
 * sequence or an already durable A-only state; PREPARED is never consumed on
 * the OTA worker's same boot. A mismatch retains the claim. */
esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32]);

/* Fresh-boot recovery after physical eota_retire_inactive, before product_boot
 * creates any guest thread. In addition to A/B and A-only it recognizes only
 * the same receipt's exact A/C operation, abandons its candidate, then drops
 * the unbootable C binding. The current boot ID must differ from any recorded
 * trial boot ID; no live guest may be silently canceled. */
esp_base_container_retire_result_t esp_base_container_product_recover_retired_firmware(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32], const uint8_t candidate_sha256[32],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const char boot_id[37]);

/* Before starting a guest on selected C, bind the original V2 receipt to the
 * exact A/C ECS2 transition and sequence. A successful prior receipt requires
 * a durable CONFIRMED phase; a PREPARED receipt may resume only the precise
 * pending trial or a VALID C awaiting/after Container confirmation. */
bool esp_base_container_product_verify_selected_ota(
    const esp_base_storage_claim_t *claim,
    const esp_base_ota_receipt_recovery_t *receipt, eota_state_t running_state);

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
