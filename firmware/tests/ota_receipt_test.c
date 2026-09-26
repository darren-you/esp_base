#include "esp_base_ota_receipt.h"
#include "esp_base_ota_firmware.h"
#include "nvs.h"
#include "esp_partition.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEVICE "22222222-2222-4222-8222-222222222222"
#define OP "44444444-4444-4444-8444-444444444444"
#define NEXT_OP "55555555-5555-4555-8555-555555555555"
#define IMAGE_BYTES 512
#define STORED_BYTES 186

static uint8_t running_subtype, boot_subtype;
static eota_state_t source_state, target_state;
static esp_err_t target_lookup;
static uint8_t image[IMAGE_BYTES], stored[STORED_BYTES], staged[STORED_BYTES];
static uint8_t observed_source[32], observed_inactive[32];
static bool exists, signed_enabled, after_write, firmware_observation_ok;
static size_t stored_size;
static int fault, writes, commits, reads, partition_reads, handles;
enum { NO_FAULT, INIT_FAULT, READ_FAULT, OPEN_WRITE_FAULT, SET_BEFORE_FAULT, SET_AFTER_FAULT, COMMIT_FAULT, READBACK_FAULT, READBACK_MISMATCH };

static void reset(void)
{
    running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    source_state = EOTA_STATE_VALID;
    target_state = EOTA_STATE_UNDEFINED;
    target_lookup = ESP_ERR_NOT_FOUND;
    memset(image, 0x35, sizeof image);
    memset(stored, 0, sizeof stored);
    memset(staged, 0, sizeof staged);
    for (size_t index = 0; index < 32; ++index) {
        observed_source[index] = (uint8_t)(0xa0U + index);
        observed_inactive[index] = (uint8_t)(0x40U + index);
    }
    exists = false;
    stored_size = STORED_BYTES;
    signed_enabled = true;
    firmware_observation_ok = true;
    after_write = false;
    fault = writes = commits = reads = partition_reads = handles = 0;
}

static esp_base_ota_receipt_snapshot_t snapshot(void)
{
    esp_base_ota_receipt_snapshot_t result = {.container_enabled = true,
                                               .container_sequence = 7U};
    memcpy(result.source_sha256, observed_source, 32);
    if (target_lookup == ESP_OK && target_state == EOTA_STATE_VALID &&
        memcmp(observed_source, observed_inactive, 32) != 0) {
        memcpy(result.inactive_sha256, observed_inactive, 32);
    }
    return result;
}

static esp_base_ota_receipt_result_t register_receipt(
    const char *device_id, const esp_base_ota_request_t *request)
{
    const esp_base_ota_receipt_snapshot_t current = snapshot();
    return esp_base_ota_receipt_register(device_id, request, &current);
}

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation,
    const eota_prepared_t *prepared, esp_base_ota_firmware_set_t *set)
{
    assert(observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED && prepared == NULL && set != NULL);
    if (!firmware_observation_ok) return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    *set = (esp_base_ota_firmware_set_t){.bootable_count = 1U};
    memcpy(set->running_firmware_sha256, observed_source, 32);
    memcpy(set->bootable_firmware_sha256[0], observed_source, 32);
    if (target_lookup == ESP_OK && target_state == EOTA_STATE_VALID &&
        memcmp(observed_source, observed_inactive, 32) != 0) {
        set->bootable_count = 2U;
        memcpy(set->bootable_firmware_sha256[1], observed_inactive, 32);
    }
    return ESP_BASE_OTA_FIRMWARE_OK;
}

static esp_base_ota_request_t request(const char *id)
{
    esp_base_ota_request_t out = {.image_size_bytes = IMAGE_BYTES};
    strcpy(out.operation_id, id);
    strcpy(out.image_url, "https://updates.example.test/base.bin");
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof image; ++i) sum += image[i];
    for (size_t i = 0; i < 32; ++i) out.sha256[i] = (uint8_t)(sum + i);
    return out;
}

bool eota_available(void) { return signed_enabled; }
eota_result_t eota_validate_image_request(const eota_image_t *candidate)
{
    assert(candidate && candidate->image_url != NULL);
    return candidate->image_size_bytes >= 512U &&
           strncmp(candidate->image_url, "https://", 8) == 0 ?
           EOTA_UPDATE_OK : EOTA_UPDATE_INVALID_REQUEST;
}
const char *eota_error(eota_result_t result)
{
    switch (result) {
    case EOTA_UPDATE_DOWNLOAD_FAILED: return "ota_download_failed";
    case EOTA_UPDATE_RESOURCE_FAILURE: return "resource_failure";
    default: return "other_failure";
    }
}
eota_result_t eota_observe_slots(const eota_policy_t *policy, eota_slots_t *slots)
{
    assert(policy && slots && !strcmp(policy->project_name, "esp_base") &&
           policy->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID &&
           policy->ota_0_address_bytes == ESP_BASE_OTA_0_ADDRESS_BYTES &&
           policy->ota_1_address_bytes == ESP_BASE_OTA_1_ADDRESS_BYTES &&
           policy->ota_size_bytes == ESP_BASE_OTA_SLOT_SIZE_BYTES &&
           policy->connect_timeout_ms == 5000 && policy->read_timeout_ms == 1000 &&
           policy->idle_timeout_ms == 30000 && policy->total_timeout_ms == 300000);
    const uint8_t target_subtype = running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ?
        ESP_PARTITION_SUBTYPE_APP_OTA_1 : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    *slots = (eota_slots_t){
        .running_subtype = running_subtype,
        .boot_subtype = boot_subtype,
        .target_subtype = target_subtype,
        .running_address_bytes = running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? ESP_BASE_OTA_0_ADDRESS_BYTES : ESP_BASE_OTA_1_ADDRESS_BYTES,
        .boot_address_bytes = boot_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? ESP_BASE_OTA_0_ADDRESS_BYTES : ESP_BASE_OTA_1_ADDRESS_BYTES,
        .target_address_bytes = target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? ESP_BASE_OTA_0_ADDRESS_BYTES : ESP_BASE_OTA_1_ADDRESS_BYTES,
        .running_size_bytes = ESP_BASE_OTA_SLOT_SIZE_BYTES,
        .boot_size_bytes = ESP_BASE_OTA_SLOT_SIZE_BYTES,
        .target_size_bytes = ESP_BASE_OTA_SLOT_SIZE_BYTES,
        .running_state = source_state,
        .target_state = target_lookup == ESP_ERR_NOT_FOUND ? EOTA_STATE_UNTRACKED : target_state,
    };
    return target_lookup == ESP_FAIL ? EOTA_UPDATE_SLOT_UNAVAILABLE : EOTA_UPDATE_OK;
}
eota_result_t eota_preflight(const eota_policy_t *policy, uint32_t size, eota_slots_t *slots)
{
    eota_result_t result = eota_observe_slots(policy, slots);
    if (result != EOTA_UPDATE_OK) return result;
    if (!size || size > slots->target_size_bytes ||
        slots->running_subtype != slots->boot_subtype || slots->running_state != EOTA_STATE_VALID ||
        slots->target_state == EOTA_STATE_NEW || slots->target_state == EOTA_STATE_PENDING_VERIFY ||
        slots->target_state == EOTA_STATE_OTHER) return EOTA_UPDATE_SLOT_UNAVAILABLE;
    return EOTA_UPDATE_OK;
}
eota_result_t eota_sha256_running(const eota_policy_t *policy, uint32_t size, uint8_t digest[32])
{
    assert(policy && digest && size == sizeof image);
    ++partition_reads;
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof image; ++i) sum += image[i];
    for (size_t i = 0; i < 32; ++i) digest[i] = (uint8_t)(sum + i);
    return EOTA_UPDATE_OK;
}
esp_err_t nvs_flash_init_partition(const char *partition)
{ assert(!strcmp(partition, "base_store")); return fault == INIT_FAULT ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_open_from_partition(const char *partition, const char *space, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(partition, "base_store") && !strcmp(space, "base_ota"));
    if (mode == NVS_READWRITE && fault == OPEN_WRITE_FAULT) return ESP_FAIL;
    if (mode == NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    ++handles;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 && handles > 0); --handles; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output, size_t *size)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "operation"));
    ++reads;
    if (fault == READ_FAULT || (fault == READBACK_FAULT && after_write)) return ESP_FAIL;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < stored_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(output, stored, stored_size);
    if (fault == READBACK_MISMATCH && after_write) ((uint8_t *)output)[0] ^= 1;
    *size = stored_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "operation") && size == sizeof stored);
    ++writes;
    if (fault == SET_BEFORE_FAULT) return ESP_FAIL;
    memcpy(staged, data, size);
    stored_size = size;
    after_write = true;
    if (fault == SET_AFTER_FAULT) { memcpy(stored, staged, size); exists = true; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && handles == 1);
    ++commits;
    if (fault == COMMIT_FAULT) return ESP_FAIL;
    memcpy(stored, staged, sizeof stored);
    exists = true;
    return ESP_OK;
}
int main(void)
{
    esp_base_ota_receipt_view_t view;
    esp_base_ota_receipt_recovery_t recovery;
    reset();
    esp_base_ota_request_t ota = request(OP);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_NOT_FOUND);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_NOT_FOUND);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(writes == 1 && commits == 1 && reads == 1 && handles == 0);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_OK);
    assert(recovery.status == ESP_BASE_OTA_RECEIPT_PREPARED &&
           !strcmp(recovery.operation_id, OP) &&
           recovery.source_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
           recovery.target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
           recovery.image_size_bytes == IMAGE_BYTES &&
           !memcmp(recovery.candidate_sha256, ota.sha256, 32) &&
           !memcmp(recovery.source_sha256, observed_source, 32) &&
           !memcmp(recovery.inactive_sha256, (uint8_t[32]){0}, 32) &&
           recovery.container_enabled && recovery.container_sequence == 7U);
    assert(esp_base_ota_receipt_query(DEVICE, OP, true, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_RUNNING && !view.error_code && partition_reads == 0);
    assert(!strcmp(view.operation_id, OP) && view.image_size_bytes == IMAGE_BYTES && view.target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
    assert(!memcmp(view.sha256, ota.sha256, 32));
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN && partition_reads == 0);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_EXISTS && writes == 1);
    ota.sha256[0] ^= 1;
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_CONFLICT && writes == 1);
    ota.sha256[0] ^= 1;
    esp_base_ota_request_t next = request(NEXT_OP);
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_BUSY && writes == 1);

    reset(); ota = request(OP); next = request(NEXT_OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);

    running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1; source_state = EOTA_STATE_PENDING_VERIFY;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_RUNNING);
    source_state = EOTA_STATE_VALID;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN && partition_reads == 0);
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_BUSY);
    assert(esp_base_ota_receipt_record_success(DEVICE) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) ==
           ESP_BASE_OTA_RECEIPT_OK &&
           recovery.status == ESP_BASE_OTA_RECEIPT_SUCCEEDED);
    assert(esp_base_ota_receipt_record_success(DEVICE) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_RESOURCE_FAILURE) ==
           ESP_BASE_OTA_RECEIPT_CONFLICT);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_SUCCEEDED && !view.error_code && partition_reads >= 1);
    image[0] ^= 1;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN);
    image[0] ^= 1;
    target_lookup = ESP_OK; target_state = EOTA_STATE_VALID;
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_OK && writes == 3);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_NOT_FOUND);

    reset(); ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_record_success(DEVICE) ==
           ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN);
    running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    source_state = EOTA_STATE_PENDING_VERIFY;
    assert(esp_base_ota_receipt_record_success(DEVICE) ==
           ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN);
    source_state = EOTA_STATE_VALID;
    image[0] ^= 1;
    assert(esp_base_ota_receipt_record_success(DEVICE) ==
           ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN);
    image[0] ^= 1;
    fault = COMMIT_FAULT;
    assert(esp_base_ota_receipt_record_success(DEVICE) ==
           ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    fault = NO_FAULT;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) ==
           ESP_BASE_OTA_RECEIPT_OK && view.state == ESP_BASE_OTA_OPERATION_UNKNOWN);
    assert(esp_base_ota_receipt_record_success(DEVICE) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) ==
           ESP_BASE_OTA_RECEIPT_OK && view.state == ESP_BASE_OTA_OPERATION_SUCCEEDED);

    reset(); ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    target_lookup = ESP_OK; target_state = EOTA_STATE_ABORTED;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN);
    next = request(NEXT_OP);
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_BUSY);
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_RESOURCE_FAILURE) ==
           ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_OK &&
           recovery.status == ESP_BASE_OTA_RECEIPT_FAILED);
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_RESOURCE_FAILURE) ==
           ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_DOWNLOAD_FAILED) ==
           ESP_BASE_OTA_RECEIPT_CONFLICT);

    reset(); ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_RESOURCE_FAILURE) ==
           ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_OK &&
           recovery.status == ESP_BASE_OTA_RECEIPT_PREPARED);
    reset(); ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_DOWNLOAD_FAILED) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_FAILED && !strcmp(view.error_code, "ota_download_failed"));
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_OK);

    reset(); ota = request(OP); next = request(NEXT_OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    target_lookup = ESP_OK; target_state = EOTA_STATE_NEW;
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE && writes == 1);
    target_state = EOTA_STATE_PENDING_VERIFY;
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE && writes == 1);
    target_lookup = ESP_FAIL;
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN && writes == 1);
    target_lookup = ESP_OK; target_state = EOTA_STATE_VALID;
    boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH && writes == 1);
    boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0; source_state = EOTA_STATE_PENDING_VERIFY;
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID && writes == 1);
    source_state = EOTA_STATE_VALID;
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_BUSY && writes == 1);

    reset(); ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    fault = COMMIT_FAULT;
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_DOWNLOAD_FAILED) ==
           ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    fault = NO_FAULT;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_UNKNOWN);
    fault = SET_AFTER_FAULT;
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_DOWNLOAD_FAILED) ==
           ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    fault = NO_FAULT;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_OK);
    assert(view.state == ESP_BASE_OTA_OPERATION_FAILED); /* Error may follow a durable write. */
    assert(esp_base_ota_receipt_record_failure(DEVICE, OP, EOTA_UPDATE_BOOT_STATE_UNKNOWN) ==
           ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE);

    reset(); ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    stored[4] = 9;
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
    assert(register_receipt(DEVICE, &next) == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);

    reset(); ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    stored_size = 118U; /* Existing V1 blob cannot authorize a destructive replay. */
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);

    reset(); ota = request(OP);
    target_lookup = ESP_OK; target_state = EOTA_STATE_VALID;
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_OK);
    assert(!memcmp(recovery.inactive_sha256, observed_inactive, 32));

    reset(); ota = request(OP);
    running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    target_lookup = ESP_OK; target_state = EOTA_STATE_VALID;
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_OK &&
           recovery.source_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
           recovery.target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0);

    reset(); ota = request(OP);
    target_lookup = ESP_OK; target_state = EOTA_STATE_VALID;
    memcpy(observed_inactive, observed_source, 32);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_OK &&
           !memcmp(recovery.inactive_sha256, (uint8_t[32]){0}, 32));

    reset(); ota = request(OP);
    esp_base_ota_receipt_snapshot_t current = snapshot();
    current.source_sha256[0] ^= 1U;
    assert(esp_base_ota_receipt_register(DEVICE, &ota, &current) ==
           ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH && writes == 0);
    current = snapshot();
    current.container_sequence = 0;
    assert(esp_base_ota_receipt_register(DEVICE, &ota, &current) ==
           ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH && writes == 0);
    current = snapshot();
    firmware_observation_ok = false;
    assert(esp_base_ota_receipt_register(DEVICE, &ota, &current) ==
           ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH && writes == 0);

    reset(); ota = request(OP);
    ota.image_size_bytes = 1U;
    assert(register_receipt(DEVICE, &ota) ==
           ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE && writes == 0);
    ota = request(OP);
    strcpy(ota.image_url, "http://updates.example.test/base.bin");
    assert(register_receipt(DEVICE, &ota) ==
           ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE && writes == 0);

    reset(); ota = request(OP);
    target_lookup = ESP_OK; target_state = EOTA_STATE_VALID;
    current = snapshot();
    current.inactive_sha256[0] ^= 1U;
    assert(esp_base_ota_receipt_register(DEVICE, &ota, &current) ==
           ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH && writes == 0);

    reset(); ota = request(OP);
    current = snapshot();
    current.container_enabled = false;
    current.container_sequence = 0;
    assert(esp_base_ota_receipt_register(DEVICE, &ota, &current) == ESP_BASE_OTA_RECEIPT_OK);
    assert(esp_base_ota_receipt_load_for_recovery(DEVICE, &recovery) == ESP_BASE_OTA_RECEIPT_OK &&
           !recovery.container_enabled && recovery.container_sequence == 0);

    for (int scenario = INIT_FAULT; scenario <= READBACK_MISMATCH; ++scenario) {
        reset(); ota = request(OP);
        if (scenario == READ_FAULT) {
            assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_OK);
            ota = request(NEXT_OP);
        }
        fault = scenario;
        const esp_base_ota_receipt_result_t result = register_receipt(DEVICE, &ota);
        if (scenario == OPEN_WRITE_FAULT) assert(result == ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE);
        else assert(result == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN);
        assert(handles == 0);
    }
    reset(); signed_enabled = false; ota = request(OP);
    assert(register_receipt(DEVICE, &ota) == ESP_BASE_OTA_RECEIPT_UNSUPPORTED && writes == 0);
    assert(esp_base_ota_receipt_query(DEVICE, OP, false, &view) == ESP_BASE_OTA_RECEIPT_UNSUPPORTED && reads == 0);
    puts("  ota_receipt passed (durable intent, active/pending/valid/rollback, uncertain NVS; fake SDK)");
}
