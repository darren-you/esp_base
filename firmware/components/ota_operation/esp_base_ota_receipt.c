#include "esp_base_ota_receipt.h"
#include "esp_base_ota_firmware.h"

#include <stddef.h>
#include <string.h>

#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#define OTA_PARTITION "base_store"
#define OTA_NAMESPACE "base_ota"
#define OTA_KEY "operation"
#define OTA_BYTES 186
#define OTA_STATUS_PREPARED 1
#define OTA_STATUS_FAILED 2
#define OTA_STATUS_SUCCEEDED 3

typedef struct {
    uint8_t status, source_subtype, target_subtype, failure;
    uint32_t image_size_bytes;
    bool container_enabled;
    uint32_t container_sequence;
    char device_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES];
    uint8_t sha256[32];
    uint8_t source_sha256[32];
    uint8_t inactive_sha256[32];
} receipt_t;

static bool zero_sha256(const uint8_t sha256[EOTA_SHA256_BYTES])
{
    uint8_t any = 0;
    for (size_t index = 0; index < EOTA_SHA256_BYTES; ++index) any |= sha256[index];
    return any == 0;
}

static bool same_slots(const eota_slots_t *first, const eota_slots_t *second)
{
    return first->running_subtype == second->running_subtype &&
        first->boot_subtype == second->boot_subtype &&
        first->target_subtype == second->target_subtype &&
        first->running_address_bytes == second->running_address_bytes &&
        first->boot_address_bytes == second->boot_address_bytes &&
        first->target_address_bytes == second->target_address_bytes &&
        first->running_size_bytes == second->running_size_bytes &&
        first->boot_size_bytes == second->boot_size_bytes &&
        first->target_size_bytes == second->target_size_bytes &&
        first->running_state == second->running_state &&
        first->target_state == second->target_state;
}

static bool valid_uuid(const char *value)
{
    if (value == NULL || strnlen(value, ESP_BASE_OTA_OPERATION_ID_BYTES) != 36 ||
        value[14] != '4' || strchr("89ab", value[19]) == NULL) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

static void encode(const receipt_t *receipt, uint8_t bytes[OTA_BYTES])
{
    memcpy(bytes, "EOTA", 4);
    bytes[4] = 2;
    bytes[5] = receipt->status;
    bytes[6] = receipt->source_subtype;
    bytes[7] = receipt->target_subtype;
    bytes[8] = receipt->failure;
    bytes[9] = receipt->container_enabled ? 1U : 0U;
    for (unsigned i = 0; i < 4; ++i) bytes[10 + i] = (uint8_t)(receipt->image_size_bytes >> (8 * i));
    memcpy(bytes + 14, receipt->device_id, 36);
    memcpy(bytes + 50, receipt->operation_id, 36);
    memcpy(bytes + 86, receipt->sha256, 32);
    memcpy(bytes + 118, receipt->source_sha256, 32);
    memcpy(bytes + 150, receipt->inactive_sha256, 32);
    for (unsigned i = 0; i < 4; ++i) {
        bytes[182 + i] = (uint8_t)(receipt->container_sequence >> (8 * i));
    }
}

static bool decode(const uint8_t bytes[OTA_BYTES], receipt_t *receipt)
{
    if (memcmp(bytes, "EOTA", 4) || bytes[4] != 2 || (bytes[9] & ~1U) != 0U ||
        (bytes[5] != OTA_STATUS_PREPARED && bytes[5] != OTA_STATUS_FAILED &&
         bytes[5] != OTA_STATUS_SUCCEEDED) ||
        !((bytes[6] == ESP_PARTITION_SUBTYPE_APP_OTA_0 && bytes[7] == ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
          (bytes[6] == ESP_PARTITION_SUBTYPE_APP_OTA_1 && bytes[7] == ESP_PARTITION_SUBTYPE_APP_OTA_0))) return false;
    receipt_t candidate = {0};
    candidate.status = bytes[5];
    candidate.source_subtype = bytes[6];
    candidate.target_subtype = bytes[7];
    candidate.failure = bytes[8];
    candidate.container_enabled = bytes[9] != 0U;
    for (unsigned i = 0; i < 4; ++i) candidate.image_size_bytes |= (uint32_t)bytes[10 + i] << (8 * i);
    for (unsigned i = 0; i < 4; ++i) candidate.container_sequence |= (uint32_t)bytes[182 + i] << (8 * i);
    if (candidate.image_size_bytes == 0 ||
        candidate.image_size_bytes > esp_base_ota_policy(false).ota_size_bytes ||
        ((candidate.status == OTA_STATUS_PREPARED ||
          candidate.status == OTA_STATUS_SUCCEEDED) && candidate.failure != 0) ||
        (candidate.status == OTA_STATUS_FAILED &&
         (candidate.failure == EOTA_UPDATE_OK ||
          candidate.failure == EOTA_UPDATE_BOOT_STATE_UNKNOWN ||
          candidate.failure > EOTA_UPDATE_RESOURCE_FAILURE))) return false;
    memcpy(candidate.device_id, bytes + 14, 36);
    memcpy(candidate.operation_id, bytes + 50, 36);
    memcpy(candidate.sha256, bytes + 86, 32);
    memcpy(candidate.source_sha256, bytes + 118, 32);
    memcpy(candidate.inactive_sha256, bytes + 150, 32);
    if (!valid_uuid(candidate.device_id) || !valid_uuid(candidate.operation_id) ||
        zero_sha256(candidate.sha256) || zero_sha256(candidate.source_sha256) ||
        (!zero_sha256(candidate.inactive_sha256) &&
         memcmp(candidate.inactive_sha256, candidate.source_sha256, 32) == 0) ||
        (candidate.container_enabled ? candidate.container_sequence == 0U :
         candidate.container_sequence != 0U)) return false;
    *receipt = candidate;
    return true;
}

static esp_base_ota_receipt_result_t load(receipt_t *receipt)
{
    esp_err_t error = nvs_flash_init_partition(OTA_PARTITION);
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    nvs_handle_t handle;
    error = nvs_open_from_partition(OTA_PARTITION, OTA_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    uint8_t bytes[OTA_BYTES];
    size_t size = sizeof bytes;
    error = nvs_get_blob(handle, OTA_KEY, bytes, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    if (error != ESP_OK || size != sizeof bytes || !decode(bytes, receipt)) {
        return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    }
    return ESP_BASE_OTA_RECEIPT_OK;
}

static esp_base_ota_receipt_result_t store(const receipt_t *receipt)
{
    uint8_t bytes[OTA_BYTES];
    encode(receipt, bytes);
    nvs_handle_t handle;
    esp_err_t error = nvs_open_from_partition(OTA_PARTITION, OTA_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    error = nvs_set_blob(handle, OTA_KEY, bytes, sizeof bytes);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    receipt_t observed;
    if (load(&observed) != ESP_BASE_OTA_RECEIPT_OK) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    uint8_t actual[OTA_BYTES];
    encode(&observed, actual);
    return memcmp(bytes, actual, sizeof bytes) == 0 ? ESP_BASE_OTA_RECEIPT_OK :
           ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
}

static void evaluate(const receipt_t *receipt, bool worker_active, esp_base_ota_receipt_view_t *view)
{
    view->state = ESP_BASE_OTA_OPERATION_UNKNOWN;
    view->error_code = "ota_result_uncertain";
    if (worker_active) { view->state = ESP_BASE_OTA_OPERATION_RUNNING; view->error_code = NULL; return; }
    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots;
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK) return;
    if ((receipt->status == OTA_STATUS_PREPARED ||
         receipt->status == OTA_STATUS_SUCCEEDED) &&
        slots.running_subtype == receipt->target_subtype &&
        slots.boot_subtype == receipt->target_subtype) {
        if (receipt->status == OTA_STATUS_PREPARED &&
            slots.running_state == EOTA_STATE_PENDING_VERIFY) {
            view->state = ESP_BASE_OTA_OPERATION_RUNNING;
            view->error_code = NULL;
        } else if (receipt->status == OTA_STATUS_SUCCEEDED &&
                   slots.running_state == EOTA_STATE_VALID) {
            uint8_t digest[EOTA_SHA256_BYTES];
            if (eota_sha256_running(&policy, receipt->image_size_bytes, digest) == EOTA_UPDATE_OK &&
                memcmp(digest, receipt->sha256, sizeof digest) == 0) {
                view->state = ESP_BASE_OTA_OPERATION_SUCCEEDED;
                view->error_code = NULL;
            }
        }
        return;
    }
    if (slots.running_subtype != receipt->source_subtype ||
        slots.boot_subtype != receipt->source_subtype || slots.running_state != EOTA_STATE_VALID) return;
    if (receipt->status == OTA_STATUS_FAILED) {
        view->state = ESP_BASE_OTA_OPERATION_FAILED;
        view->error_code = eota_error((eota_result_t)receipt->failure);
        return;
    }
    /* A target marked INVALID/ABORTED can still contain a bootloader-loadable
     * signed image, and ECS2 may still name it. PREPARED stays unresolved until
     * the original receipt has reconciled Flash, otadata and Container. */
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_load_for_recovery(
    const char *device_id, esp_base_ota_receipt_recovery_t *recovery)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id) || recovery == NULL) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    *recovery = (esp_base_ota_receipt_recovery_t){0};
    receipt_t receipt;
    const esp_base_ota_receipt_result_t result = load(&receipt);
    if (result != ESP_BASE_OTA_RECEIPT_OK) return result;
    if (strcmp(receipt.device_id, device_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    recovery->status = (esp_base_ota_receipt_status_t)receipt.status;
    memcpy(recovery->operation_id, receipt.operation_id, sizeof recovery->operation_id);
    recovery->source_subtype = receipt.source_subtype;
    recovery->target_subtype = receipt.target_subtype;
    memcpy(recovery->source_sha256, receipt.source_sha256, 32);
    memcpy(recovery->inactive_sha256, receipt.inactive_sha256, 32);
    memcpy(recovery->candidate_sha256, receipt.sha256, 32);
    recovery->image_size_bytes = receipt.image_size_bytes;
    recovery->container_enabled = receipt.container_enabled;
    recovery->container_sequence = receipt.container_sequence;
    return ESP_BASE_OTA_RECEIPT_OK;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!view || !valid_uuid(device_id) || !valid_uuid(operation_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    *view = (esp_base_ota_receipt_view_t){0};
    receipt_t receipt;
    const esp_base_ota_receipt_result_t result = load(&receipt);
    if (result != ESP_BASE_OTA_RECEIPT_OK) return result;
    if (strcmp(receipt.device_id, device_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    if (strcmp(receipt.operation_id, operation_id)) return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
    memcpy(view->operation_id, receipt.operation_id, sizeof view->operation_id);
    memcpy(view->sha256, receipt.sha256, sizeof view->sha256);
    view->image_size_bytes = receipt.image_size_bytes;
    view->target_subtype = receipt.target_subtype;
    evaluate(&receipt, worker_active, view);
    return ESP_BASE_OTA_RECEIPT_OK;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_request_t *request,
    const esp_base_ota_receipt_snapshot_t *snapshot)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id) || request == NULL || !valid_uuid(request->operation_id) ||
        snapshot == NULL) {
        return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    }
    eota_image_t image = {.image_url = request->image_url,
                          .image_size_bytes = request->image_size_bytes};
    memcpy(image.sha256, request->sha256, sizeof image.sha256);
    if (eota_validate_image_request(&image) != EOTA_UPDATE_OK) {
        return ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
    }
    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots;
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK) {
        return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
    }
    if (request->image_size_bytes == 0 || request->image_size_bytes > slots.target_size_bytes) {
        return ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
    }
    if (slots.running_subtype != slots.boot_subtype ||
        slots.running_address_bytes != slots.boot_address_bytes) return ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH;
    if (slots.running_state != EOTA_STATE_VALID) return ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID;
    if (slots.target_state == EOTA_STATE_NEW || slots.target_state == EOTA_STATE_PENDING_VERIFY) {
        return ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE;
    }
    if (slots.target_state != EOTA_STATE_UNTRACKED && slots.target_state != EOTA_STATE_UNDEFINED &&
        slots.target_state != EOTA_STATE_VALID && slots.target_state != EOTA_STATE_INVALID &&
        slots.target_state != EOTA_STATE_ABORTED) return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
    if (eota_preflight(&policy, request->image_size_bytes, &slots) != EOTA_UPDATE_OK) {
        return ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
    }
    receipt_t previous;
    esp_base_ota_receipt_result_t existing = load(&previous);
    if (existing != ESP_BASE_OTA_RECEIPT_NOT_FOUND && existing != ESP_BASE_OTA_RECEIPT_OK) return existing;
    if (existing == ESP_BASE_OTA_RECEIPT_OK) {
        if (strcmp(previous.device_id, device_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
        if (!strcmp(previous.operation_id, request->operation_id)) {
            return previous.image_size_bytes == request->image_size_bytes &&
                   memcmp(previous.sha256, request->sha256, 32) == 0 ?
                   ESP_BASE_OTA_RECEIPT_EXISTS : ESP_BASE_OTA_RECEIPT_CONFLICT;
        }
        esp_base_ota_receipt_view_t view = {0};
        evaluate(&previous, false, &view);
        if (view.state != ESP_BASE_OTA_OPERATION_SUCCEEDED && view.state != ESP_BASE_OTA_OPERATION_FAILED) {
            return ESP_BASE_OTA_RECEIPT_BUSY;
        }
    }
    if (zero_sha256(request->sha256) || zero_sha256(snapshot->source_sha256) ||
        (!zero_sha256(snapshot->inactive_sha256) &&
         memcmp(snapshot->source_sha256, snapshot->inactive_sha256, 32) == 0) ||
        (snapshot->container_enabled ? snapshot->container_sequence == 0U :
         snapshot->container_sequence != 0U)) {
        return ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH;
    }
    esp_base_ota_firmware_set_t observed = {0};
    eota_slots_t after = {0};
    if (esp_base_ota_observe_firmware_set(
            ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &observed) != ESP_BASE_OTA_FIRMWARE_OK ||
        eota_observe_slots(&policy, &after) != EOTA_UPDATE_OK ||
        !same_slots(&slots, &after) ||
        (observed.bootable_count != 1U && observed.bootable_count != 2U) ||
        memcmp(observed.running_firmware_sha256, snapshot->source_sha256, 32) != 0 ||
        memcmp(observed.bootable_firmware_sha256[0], snapshot->source_sha256, 32) != 0 ||
        memcmp(observed.bootable_firmware_sha256[1], snapshot->inactive_sha256, 32) != 0 ||
        (observed.bootable_count == 1U && !zero_sha256(snapshot->inactive_sha256)) ||
        (observed.bootable_count == 2U && zero_sha256(snapshot->inactive_sha256))) {
        return ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH;
    }
    receipt_t next = {.status = OTA_STATUS_PREPARED, .source_subtype = slots.running_subtype,
        .target_subtype = slots.target_subtype, .image_size_bytes = request->image_size_bytes,
        .container_enabled = snapshot->container_enabled,
        .container_sequence = snapshot->container_sequence};
    memcpy(next.device_id, device_id, sizeof next.device_id);
    memcpy(next.operation_id, request->operation_id, sizeof next.operation_id);
    memcpy(next.sha256, request->sha256, sizeof next.sha256);
    memcpy(next.source_sha256, snapshot->source_sha256, sizeof next.source_sha256);
    memcpy(next.inactive_sha256, snapshot->inactive_sha256, sizeof next.inactive_sha256);
    return store(&next);
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id) || !valid_uuid(operation_id) || error == EOTA_UPDATE_OK ||
        error == EOTA_UPDATE_BOOT_STATE_UNKNOWN ||
        error > EOTA_UPDATE_RESOURCE_FAILURE) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    receipt_t receipt;
    esp_base_ota_receipt_result_t result = load(&receipt);
    if (result != ESP_BASE_OTA_RECEIPT_OK) return result;
    if (strcmp(receipt.device_id, device_id) || strcmp(receipt.operation_id, operation_id)) {
        return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    }
    if (receipt.status == OTA_STATUS_FAILED) {
        return receipt.failure == (uint8_t)error ? ESP_BASE_OTA_RECEIPT_OK :
               ESP_BASE_OTA_RECEIPT_CONFLICT;
    }
    if (receipt.status == OTA_STATUS_SUCCEEDED) return ESP_BASE_OTA_RECEIPT_CONFLICT;
    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots = {0};
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK ||
        slots.running_subtype != receipt.source_subtype ||
        slots.boot_subtype != receipt.source_subtype ||
        slots.target_subtype != receipt.target_subtype ||
        slots.running_state != EOTA_STATE_VALID) {
        return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
    }
    receipt.status = OTA_STATUS_FAILED;
    receipt.failure = error;
    return store(&receipt);
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_record_success(
    const char *device_id)
{
    if (!eota_available()) return ESP_BASE_OTA_RECEIPT_UNSUPPORTED;
    if (!valid_uuid(device_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE;
    receipt_t receipt;
    const esp_base_ota_receipt_result_t loaded = load(&receipt);
    if (loaded != ESP_BASE_OTA_RECEIPT_OK) return loaded;
    if (strcmp(receipt.device_id, device_id)) return ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    if (receipt.status == OTA_STATUS_FAILED) return ESP_BASE_OTA_RECEIPT_CONFLICT;
    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots = {0};
    uint8_t digest[EOTA_SHA256_BYTES] = {0};
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK ||
        slots.running_subtype != receipt.target_subtype ||
        slots.boot_subtype != receipt.target_subtype ||
        slots.running_address_bytes != slots.boot_address_bytes ||
        slots.running_state != EOTA_STATE_VALID ||
        eota_sha256_running(&policy, receipt.image_size_bytes, digest) != EOTA_UPDATE_OK ||
        memcmp(digest, receipt.sha256, sizeof digest) != 0) {
        return ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN;
    }
    if (receipt.status == OTA_STATUS_SUCCEEDED) return ESP_BASE_OTA_RECEIPT_OK;
    receipt.status = OTA_STATUS_SUCCEEDED;
    return store(&receipt);
}
