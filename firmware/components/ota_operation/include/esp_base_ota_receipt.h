#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_base_ota_policy.h"

typedef enum {
    ESP_BASE_OTA_RECEIPT_OK,
    ESP_BASE_OTA_RECEIPT_UNSUPPORTED,
    ESP_BASE_OTA_RECEIPT_NOT_FOUND,
    ESP_BASE_OTA_RECEIPT_EXISTS,
    ESP_BASE_OTA_RECEIPT_CONFLICT,
    ESP_BASE_OTA_RECEIPT_BUSY,
    ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE,
    ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH,
    ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID,
    ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE,
    ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN,
    ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH,
    ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE,
    ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN,
} esp_base_ota_receipt_result_t;

typedef enum {
    ESP_BASE_OTA_OPERATION_UNKNOWN,
    ESP_BASE_OTA_OPERATION_RUNNING,
    ESP_BASE_OTA_OPERATION_SUCCEEDED,
    ESP_BASE_OTA_OPERATION_FAILED,
} esp_base_ota_operation_state_t;

typedef struct {
    esp_base_ota_operation_state_t state;
    const char *error_code;
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint8_t sha256[32];
    uint32_t image_size_bytes;
    uint8_t target_subtype;
} esp_base_ota_receipt_view_t;

/* Captured under the app/otadata storage owner immediately before registering
 * the write intent. Digests are the distinct signed Base identities returned
 * by CONFIRMED observation: inactive is zero for an A-only set, including two
 * physical slots containing the same signed image. A configured Container
 * supplies its reconciled ECS2 sequence from that same owner interval. */
typedef struct esp_base_ota_receipt_snapshot {
    uint8_t source_sha256[EOTA_SHA256_BYTES];
    uint8_t inactive_sha256[EOTA_SHA256_BYTES];
    bool container_enabled;
    uint32_t container_sequence;
} esp_base_ota_receipt_snapshot_t;

typedef enum {
    ESP_BASE_OTA_RECEIPT_PREPARED = 1,
    ESP_BASE_OTA_RECEIPT_FAILED = 2,
    ESP_BASE_OTA_RECEIPT_SUCCEEDED = 3,
} esp_base_ota_receipt_status_t;

typedef struct {
    esp_base_ota_receipt_status_t status;
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint8_t source_subtype;
    uint8_t target_subtype;
    uint8_t source_sha256[EOTA_SHA256_BYTES];
    uint8_t inactive_sha256[EOTA_SHA256_BYTES];
    uint8_t candidate_sha256[EOTA_SHA256_BYTES];
    uint32_t image_size_bytes;
    bool container_enabled;
    uint32_t container_sequence;
} esp_base_ota_receipt_recovery_t;

/* One latest operation is retained in base_store/base_ota/operation. V2 stores
 * the source, old distinct inactive and requested candidate identities plus
 * the current ECS2 sequence in that same blob. The caller holds the storage
 * claim and supplies a reconciled Container snapshot; register independently
 * rechecks the signed Base firmware set before commit/readback. A new operation
 * may replace only a terminal result; the same ID never downloads twice. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_request_t *request,
    const esp_base_ota_receipt_snapshot_t *snapshot);
/* Read the original committed intent before deciding whether an interrupted
 * target may be erased. FAILED is terminal and never authorizes replay. A
 * corrupt or older-format record is uncertain, not absent. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_load_for_recovery(
    const char *device_id, esp_base_ota_receipt_recovery_t *recovery);
/* Record failure after either no app/Container mutation occurred or the
 * original PREPARED receipt has driven complete physical/ECS2 reconciliation.
 * This function cannot itself prove the caller's Container state. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error);
/* The caller must first prove OTA VALID and, when Container is configured,
 * persistent Container confirmation. This independently rechecks C's signed
 * identity, then commits and reads back the success marker. */
esp_base_ota_receipt_result_t esp_base_ota_receipt_record_success(
    const char *device_id);
esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view);
