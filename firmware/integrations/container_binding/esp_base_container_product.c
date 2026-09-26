// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_product.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_base_container_binding.h"
#include "esp_base_container_no_package.h"
#include "esp_container_package_slot.h"
#include "esp_container_product.h"
#include "esp_container_slots.h"
#include "esp_container_slots_idf.h"

static const char *TAG = "base_container";

typedef struct {
    econtainer_slots_idf_provider_t provider;
    econtainer_package_workspace_t package_workspace;
    econtainer_wasm_workspace_t wasm_workspace;
    econtainer_package_slot_validation_t validation;
    econtainer_runtime_limits_t limits;
    uint8_t public_key[512];
    size_t public_key_size_bytes;
    SemaphoreHandle_t ready;
    SemaphoreHandle_t stopped;
    SemaphoreHandle_t storage_lock;
    const esp_base_storage_claim_t *claim;
    atomic_int result;
    bool trial_mode;
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
    atomic_bool stop_requested;
    atomic_bool instance_active;
    atomic_bool boot_admitted;
    bool stop_succeeded;
    bool provider_bound;
} product_context_t;

static product_context_t s_product;

/* Every field is an independently approved build input. An entirely empty
 * configuration is the existing no-package Base product; any partial input
 * activates a strict reject path rather than silently ignoring it. */
static bool policy_present(void)
{
    return CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_KEY_ID[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PUBLIC_KEY_DER_HEX[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_LABEL[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_LABEL[0] != '\0' ||
        CONFIG_ESP_BASE_CONTAINER_NVS_OFFSET != 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_SIZE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_EVENT_QUEUE != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_HOST_CALL_MS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES != 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_ENTRY_MS != 0 ||
        CONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES != 0;
}

bool esp_base_container_product_configured(void)
{
    return policy_present();
}

bool esp_base_container_product_ota_ready(void)
{
    return !policy_present() ||
        (atomic_load_explicit(&s_product.boot_admitted, memory_order_acquire) &&
         atomic_load_explicit(&s_product.result, memory_order_acquire) ==
             ESP_BASE_CONTAINER_EMPTY);
}

static int hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static bool decode_public_key(void)
{
    const char *hex = CONFIG_ESP_BASE_CONTAINER_PUBLIC_KEY_DER_HEX;
    const size_t length = strlen(hex);
    if (length == 0 || (length & 1U) != 0U || length > sizeof(s_product.public_key) * 2U) {
        return false;
    }
    for (size_t index = 0; index < length / 2U; ++index) {
        const int upper = hex_digit(hex[index * 2U]);
        const int lower = hex_digit(hex[index * 2U + 1U]);
        if (upper < 0 || lower < 0) return false;
        s_product.public_key[index] = (uint8_t)((upper << 4) | lower);
    }
    s_product.public_key_size_bytes = length / 2U;
    return true;
}

static bool decode_uuid(const char value[37], uint8_t bytes[16])
{
    if (value == NULL || strnlen(value, 37) != 36 ||
        value[14] != '4' || strchr("89ab", value[19]) == NULL) return false;
    size_t written = 0;
    for (size_t index = 0; index < 36; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
            continue;
        }
        const int upper = hex_digit(value[index]);
        if (upper < 0 || ++index == 36) return false;
        const int lower = hex_digit(value[index]);
        if (lower < 0 || written >= 16) return false;
        bytes[written++] = (uint8_t)((upper << 4) | lower);
    }
    return written == 16;
}

static bool configure_policy(void)
{
    if (CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_KEY_ID[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_LABEL[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_NVS_LABEL[0] == '\0' ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_PACKAGE_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_0_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_1_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_SLOT_2_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_OFFSET == 0 ||
        CONFIG_ESP_BASE_CONTAINER_NVS_SIZE == 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES > ECONTAINER_PACKAGE_WASM_MAX_BYTES ||
        CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES > 65536 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_EVENT_QUEUE <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS > INT32_MAX ||
        CONFIG_ESP_BASE_CONTAINER_MAX_HOST_CALL_MS <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_MAX_ENTRY_MS <= 0 ||
        CONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES < 8192 ||
        (CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES & ~ECONTAINER_CAP_ALL) != 0 ||
        ((CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES & ECONTAINER_CAP_TIMER) != 0 ?
             CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS <= 0 :
             CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS != 0) ||
        ((CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES & ECONTAINER_CAP_LOG) != 0 ?
             CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES <= 0 ||
             CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES > 256 :
             CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES != 0) ||
        !decode_public_key()) return false;

    s_product.validation = (econtainer_package_slot_validation_t){
        .expected_product_id = CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID,
        .public_key_rsa_der = s_product.public_key,
        .public_key_size_bytes = s_product.public_key_size_bytes,
        .expected_key_id = CONFIG_ESP_BASE_CONTAINER_KEY_ID,
        .max_wasm_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES,
        .wasm_authorization = {
            .allowed_capabilities = CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES,
            .max_memory_bytes = 65536U,
            .max_stack_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES,
        },
        .max_event_queue_limit = CONFIG_ESP_BASE_CONTAINER_MAX_EVENT_QUEUE,
        .max_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .max_host_call_timeout_ms = CONFIG_ESP_BASE_CONTAINER_MAX_HOST_CALL_MS,
        .max_storage_limit_bytes = 0U,
        .package_workspace = &s_product.package_workspace,
        .wasm_workspace = &s_product.wasm_workspace,
    };
    s_product.limits = (econtainer_runtime_limits_t){
        .max_wasm_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES,
        .max_memory_pages = 1U,
        .stack_size_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES,
        .max_event_bytes = ECONTAINER_EVENT_BUFFER_BYTES,
        .allowed_capabilities = CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES,
        .max_log_bytes = CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES,
        .max_timers = CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS,
        .init_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .event_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .stop_instruction_budget = CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS,
        .max_entry_duration_ms = CONFIG_ESP_BASE_CONTAINER_MAX_ENTRY_MS,
    };
    return true;
}

typedef struct {
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
} stage_context_t;

static econtainer_slots_result_t stage_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    if (firmware_set->bootable_count != 2U) return ECONTAINER_SLOTS_CONFLICT;
    econtainer_slots_state_t current = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &current);
    if (result != ECONTAINER_SLOTS_OK) return result;

    const econtainer_slot_binding_t *running = NULL;
    for (size_t index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (current.bindings[index].present &&
            memcmp(current.bindings[index].firmware_sha256,
                   firmware_set->running_firmware_sha256, 32) == 0) {
            running = &current.bindings[index];
            break;
        }
    }
    if (running == NULL) return ECONTAINER_SLOTS_CONFLICT;
    /* The Base product has no authorized business event source yet. A signed
     * reusable package cannot be made healthy merely by opening it. */
    if (running->package_present) return ECONTAINER_SLOTS_UNTRUSTED;

    const stage_context_t *stage = context;
    econtainer_slot_operation_t operation = {0};
    memcpy(operation.operation_id, stage->operation_id, sizeof operation.operation_id);
    memcpy(operation.target_firmware_sha256,
           firmware_set->bootable_firmware_sha256[1], 32);
    operation.kind = ECONTAINER_SLOT_NO_PACKAGE;
    return econtainer_slots_stage_firmware(
        &s_product.provider.io, &s_product.provider.geometry, current.sequence,
        firmware_set, &operation, NULL, NULL, &current);
}

esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES])
{
    if (!policy_present()) return ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED;
    if (!esp_base_storage_claim_active(claim) || prepared == NULL ||
        s_product.ready == NULL ||
        !atomic_load_explicit(&s_product.boot_admitted, memory_order_acquire)) {
        return ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
    }
    stage_context_t stage = {0};
    if (!decode_uuid(operation_id, stage.operation_id)) {
        return ESP_BASE_CONTAINER_STAGE_REJECTED;
    }
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, prepared,
        stage_firmware, &stage);
    if (result == ECONTAINER_SLOTS_OK) return ESP_BASE_CONTAINER_STAGE_PREPARED;
    if (result == ECONTAINER_SLOTS_CONFLICT || result == ECONTAINER_SLOTS_UNTRUSTED ||
        result == ECONTAINER_SLOTS_EMPTY || result == ECONTAINER_SLOTS_NO_SPACE) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STAGE_REJECTED result=%d", (int)result);
        return ESP_BASE_CONTAINER_STAGE_REJECTED;
    }
    ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STAGE_UNCERTAIN result=%d", (int)result);
    return ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
}

typedef struct {
    uint32_t sequence;
    bool trial;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
    econtainer_runtime_t *runtime;
    econtainer_runtime_result_t runtime_result;
} open_context_t;

static econtainer_slots_result_t open_selected(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    open_context_t *open = context;
    econtainer_slot_selection_request_t request = {
        .expected_sequence = open->sequence,
        .firmware_set = *firmware_set,
        .selection = open->trial ? ECONTAINER_SLOT_SELECT_TRIAL :
                                   ECONTAINER_SLOT_SELECT_CONFIRMED,
    };
    if (open->trial) {
        memcpy(request.operation_id, open->operation_id, sizeof request.operation_id);
        memcpy(request.boot_id, open->boot_id, sizeof request.boot_id);
    }
    const econtainer_slot_runtime_result_t result = econtainer_product_open(
        &s_product.provider.io, &s_product.provider.geometry, &request,
        &s_product.validation, &s_product.limits, &open->runtime);
    open->runtime_result = result.runtime;
    return result.slots;
}

static void report_result(esp_base_container_boot_result_t result)
{
    atomic_store_explicit(&s_product.result, result, memory_order_release);
    xSemaphoreGive(s_product.ready);
}

typedef struct {
    uint32_t expected_sequence;
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
    econtainer_slots_state_t state;
} begin_trial_context_t;

static econtainer_slots_result_t begin_trial(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    begin_trial_context_t *begin = context;
    return econtainer_slots_begin_trial(
        &s_product.provider.io, &s_product.provider.geometry,
        begin->expected_sequence, firmware_set->running_firmware_sha256,
        begin->boot_id, &begin->state);
}

typedef struct {
    uint32_t offset_bytes;
    uint32_t size_bytes;
} recovery_reader_t;

static bool read_recovery_package(void *context, size_t relative_offset_bytes,
                                  uint8_t *destination, size_t size_bytes)
{
    const recovery_reader_t *reader = context;
    return size_bytes > 0U && relative_offset_bytes <= reader->size_bytes &&
           size_bytes <= reader->size_bytes - relative_offset_bytes &&
           s_product.provider.io.flash_read(s_product.provider.io.context,
               reader->offset_bytes + (uint32_t)relative_offset_bytes,
               destination, size_bytes);
}

static econtainer_slots_result_t validate_recovery_package(
    const econtainer_slot_operation_t *operation)
{
    if (operation->kind == ECONTAINER_SLOT_NO_PACKAGE) return ECONTAINER_SLOTS_OK;
    if (operation->slot >= ECONTAINER_SLOT_COUNT ||
        operation->package_size_bytes >
            s_product.provider.geometry.slots[operation->slot].size_bytes) {
        return ECONTAINER_SLOTS_INVALID;
    }
    econtainer_slot_binding_t binding = {
        .present = true,
        .package_present = true,
        .slot = operation->slot,
        .package_size_bytes = operation->package_size_bytes,
        .guest_abi_version = operation->guest_abi_version,
        .data_schema_version = operation->data_schema_version,
    };
    memcpy(binding.firmware_sha256, operation->target_firmware_sha256, 32);
    memcpy(binding.package_sha256, operation->package_sha256, 32);
    const recovery_reader_t reader = {
        .offset_bytes = s_product.provider.geometry.slots[operation->slot].offset_bytes,
        .size_bytes = operation->package_size_bytes,
    };
    econtainer_package_info_t verified_info = {0};
    econtainer_package_slot_validation_t validation = s_product.validation;
    validation.verified_info = &verified_info;
    if (!s_product.provider.io.lock(s_product.provider.io.context)) {
        return ECONTAINER_SLOTS_BUSY;
    }
    const econtainer_slot_validation_result_t checked =
        econtainer_package_slot_validate_binding(
            &validation, &binding, read_recovery_package, (void *)&reader,
            operation->package_size_bytes);
    s_product.provider.io.unlock(s_product.provider.io.context);
    return checked == ECONTAINER_SLOT_VALIDATION_OK ? ECONTAINER_SLOTS_OK :
           checked == ECONTAINER_SLOT_VALIDATION_IO_FAILED ?
               ECONTAINER_SLOTS_IO_FAILED : ECONTAINER_SLOTS_UNTRUSTED;
}

static econtainer_slots_result_t recover_firmware_transition(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK || !state.operation.firmware_transition) return result;
    const bool running_target = memcmp(state.operation.target_firmware_sha256,
        firmware_set->running_firmware_sha256, 32) == 0;
    if (running_target && state.phase == ECONTAINER_SLOT_HEALTH_VERIFIED) {
        /* The caller's CONFIRMED double observation proves that running C is
         * signed, selected and OTA VALID around this operation. This boot has
         * passed NVS/identity/safety/control startup; the previous trial
         * proved health. Recheck the package before finishing its confirm. */
        result = validate_recovery_package(&state.operation);
        if (result != ECONTAINER_SLOTS_OK) return result;
        result = econtainer_slots_confirm(&s_product.provider.io,
            &s_product.provider.geometry, state.sequence,
            firmware_set->running_firmware_sha256,
            state.operation.trial_boot_id, &state);
        if (result == ECONTAINER_SLOTS_OK) {
            ESP_LOGI(TAG, "ESP_BASE_CONTAINER_RECOVERED_VALID confirmed_sequence=%u",
                     (unsigned)state.sequence);
        }
        return result;
    }
    if (running_target || state.phase == ECONTAINER_SLOT_CONFIRMED ||
        state.phase == ECONTAINER_SLOT_IDLE) return ECONTAINER_SLOTS_OK;

    if (state.phase >= ECONTAINER_SLOT_WRITING &&
        state.phase <= ECONTAINER_SLOT_HEALTH_VERIFIED) {
        eota_slots_t slots = {0};
        const eota_policy_t policy = esp_base_ota_policy(false);
        if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK ||
            slots.running_subtype != slots.boot_subtype ||
            slots.running_state != EOTA_STATE_VALID ||
            (slots.target_state != EOTA_STATE_UNTRACKED &&
             slots.target_state != EOTA_STATE_INVALID &&
             slots.target_state != EOTA_STATE_ABORTED)) {
            return ECONTAINER_SLOTS_CONFLICT;
        }
        if (firmware_set->bootable_count == 2U &&
            memcmp(state.operation.target_firmware_sha256,
                   firmware_set->bootable_firmware_sha256[1], 32) != 0) {
            return ECONTAINER_SLOTS_CONFLICT;
        }
        result = econtainer_slots_abandon(
            &s_product.provider.io, &s_product.provider.geometry,
            state.sequence, s_product.boot_id, NULL, NULL, &state);
        if (result != ECONTAINER_SLOTS_OK) return result;
        ESP_LOGW(TAG, "ESP_BASE_CONTAINER_ABORTED firmware candidate sequence=%u",
                 (unsigned)state.sequence);
    }
    if (state.phase == ECONTAINER_SLOT_ABORTED && firmware_set->bootable_count == 1U) {
        result = econtainer_slots_drop_aborted_firmware(
            &s_product.provider.io, &s_product.provider.geometry,
            state.sequence, firmware_set, &state);
        if (result == ECONTAINER_SLOTS_OK) {
            ESP_LOGI(TAG, "ESP_BASE_CONTAINER_DROPPED_UNBOOTABLE sequence=%u",
                     (unsigned)state.sequence);
        }
    }
    return result;
}

static econtainer_slots_result_t initialize_no_package(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)context;
    bool initialized = false;
    const econtainer_slots_result_t result = esp_base_container_initialize_no_package(
        &s_product.provider.io, &s_product.provider.geometry, firmware_set, &initialized);
    if (initialized) {
        ESP_LOGI(TAG, "ESP_BASE_CONTAINER_INITIALIZED_NO_PACKAGE firmware_count=%u",
                 (unsigned)firmware_set->bootable_count);
    }
    return result;
}

static bool drain_log(econtainer_runtime_t *runtime)
{
    uint8_t bytes[256];
    size_t size_bytes = 0;
    const econtainer_runtime_result_t result = econtainer_product_take_log(
        runtime, bytes, sizeof bytes, &size_bytes);
    if (result == ECONTAINER_RUNTIME_NO_LOG) return true;
    if (result != ECONTAINER_RUNTIME_OK || size_bytes > sizeof bytes) return false;
    char hex[sizeof bytes * 2U + 1U];
    const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size_bytes; ++index) {
        hex[index * 2U] = digits[bytes[index] >> 4];
        hex[index * 2U + 1U] = digits[bytes[index] & 15U];
    }
    hex[size_bytes * 2U] = '\0';
    ESP_LOGI(TAG, "ESP_BASE_CONTAINER_LOG_HEX bytes=%u data=%s",
             (unsigned)size_bytes, hex);
    return true;
}

static void *product_thread(void *unused)
{
    (void)unused;
    if (!s_product.trial_mode) {
        const econtainer_slots_result_t initialized = esp_base_container_with_firmware_set(
            s_product.claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
            initialize_no_package, NULL);
        if (initialized != ECONTAINER_SLOTS_OK) {
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED initialization=%d", (int)initialized);
            s_product.stop_succeeded = true;
            xSemaphoreGive(s_product.stopped);
            report_result(ESP_BASE_CONTAINER_BLOCKED);
            return NULL;
        }
        const econtainer_slots_result_t recovered = esp_base_container_with_firmware_set(
            s_product.claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
            recover_firmware_transition, NULL);
        if (recovered != ECONTAINER_SLOTS_OK) {
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED recovery=%d", (int)recovered);
            s_product.stop_succeeded = true;
            xSemaphoreGive(s_product.stopped);
            report_result(ESP_BASE_CONTAINER_BLOCKED);
            return NULL;
        }
    }
    econtainer_slots_state_t state = {0};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    const esp_base_ota_firmware_observation_t observation = s_product.trial_mode ?
        ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL : ESP_BASE_OTA_FIRMWARE_CONFIRMED;
    const econtainer_slots_result_t reconcile = esp_base_container_reconcile(
        s_product.claim, observation,
        &s_product.provider.io, &s_product.provider.geometry, &state, &decision);
    if (reconcile == ECONTAINER_SLOTS_EMPTY) {
        s_product.stop_succeeded = true;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_EMPTY);
        return NULL;
    }
    if (reconcile != ECONTAINER_SLOTS_OK ||
        (s_product.trial_mode ? decision != ECONTAINER_SLOT_BOOT_START_TRIAL :
         (decision != ECONTAINER_SLOT_BOOT_CONFIRMED &&
          decision != ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED))) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED reconcile=%d decision=%d",
                 (int)reconcile, (int)decision);
        s_product.stop_succeeded = true;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    if (s_product.trial_mode && state.operation.kind != ECONTAINER_SLOT_NO_PACKAGE) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED package trial lacks business event source");
        s_product.stop_succeeded = true;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }

    open_context_t open = {.sequence = state.sequence,
                           .trial = s_product.trial_mode,
                           .runtime_result = ECONTAINER_RUNTIME_INVALID_STATE};
    if (s_product.trial_mode) {
        begin_trial_context_t begin = {.expected_sequence = state.sequence};
        memcpy(begin.boot_id, s_product.boot_id, sizeof begin.boot_id);
        const econtainer_slots_result_t started = esp_base_container_with_firmware_set(
            s_product.claim, ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL, NULL,
            begin_trial, &begin);
        if (started != ECONTAINER_SLOTS_OK) {
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED begin_trial=%d", (int)started);
            s_product.stop_succeeded = true;
            xSemaphoreGive(s_product.stopped);
            report_result(ESP_BASE_CONTAINER_BLOCKED);
            return NULL;
        }
        open.sequence = begin.state.sequence;
        memcpy(open.operation_id, begin.state.operation.operation_id,
               sizeof open.operation_id);
        memcpy(open.boot_id, begin.boot_id, sizeof open.boot_id);
    }
    const econtainer_slots_result_t slots = esp_base_container_with_firmware_set(
        s_product.claim, observation, NULL, open_selected, &open);
    if (slots != ECONTAINER_SLOTS_OK ||
        open.runtime_result != ECONTAINER_RUNTIME_OK || open.runtime == NULL) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED open_slots=%d open_runtime=%d",
                 (int)slots, (int)open.runtime_result);
        if (open.runtime != NULL) (void)econtainer_product_close(&open.runtime);
        s_product.stop_succeeded = open.runtime == NULL;
        xSemaphoreGive(s_product.stopped);
        if (slots == ECONTAINER_SLOTS_EMPTY) {
            atomic_store_explicit(&s_product.boot_admitted, true, memory_order_release);
        }
        report_result(slots == ECONTAINER_SLOTS_EMPTY ? ESP_BASE_CONTAINER_EMPTY :
                      ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    const econtainer_runtime_result_t initialized = econtainer_product_init(open.runtime);
    if (initialized != ECONTAINER_RUNTIME_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED init=%d", (int)initialized);
        s_product.stop_succeeded = econtainer_product_close(&open.runtime) == ECONTAINER_RUNTIME_OK &&
                                   open.runtime == NULL;
        xSemaphoreGive(s_product.stopped);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    ESP_LOGI(TAG, "ESP_BASE_CONTAINER_RUNNING sequence=%u trial=%d",
             (unsigned)open.sequence, s_product.trial_mode);
    atomic_store_explicit(&s_product.boot_admitted, true, memory_order_release);
    atomic_store_explicit(&s_product.instance_active, true, memory_order_release);
    report_result(ESP_BASE_CONTAINER_RUNNING);

    for (;;) {
        if (atomic_load_explicit(&s_product.stop_requested, memory_order_acquire)) break;
        if (!drain_log(open.runtime)) break;
        uint64_t deadline_ms = 0;
        const econtainer_runtime_result_t timer =
            econtainer_product_next_timer_deadline(open.runtime, &deadline_ms);
        if (timer == ECONTAINER_RUNTIME_OK) {
            const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
            if (deadline_ms <= now_ms) {
                econtainer_timer_event_t event = {0};
                int32_t guest_result = 0;
                const econtainer_runtime_result_t fired = econtainer_product_poll_timer(
                    open.runtime, &event, &guest_result);
                if (fired == ECONTAINER_RUNTIME_OK || fired == ECONTAINER_RUNTIME_NO_TIMER)
                    continue;
                ESP_LOGE(TAG, "ESP_BASE_CONTAINER_TIMER_FAILED result=%d", (int)fired);
                break;
            }
            const uint64_t wait_ms = deadline_ms - now_ms;
            vTaskDelay(pdMS_TO_TICKS(wait_ms > 1000U ? 1000U : (uint32_t)wait_ms));
        } else if (timer == ECONTAINER_RUNTIME_NO_TIMER) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        } else {
            ESP_LOGE(TAG, "ESP_BASE_CONTAINER_TIMER_FAILED result=%d", (int)timer);
            break;
        }
    }
    const econtainer_runtime_result_t stopped = econtainer_product_stop(open.runtime);
    const econtainer_runtime_result_t closed = econtainer_product_close(&open.runtime);
    s_product.stop_succeeded = stopped == ECONTAINER_RUNTIME_OK &&
                               closed == ECONTAINER_RUNTIME_OK && open.runtime == NULL;
    atomic_store_explicit(&s_product.instance_active, false, memory_order_release);
    xSemaphoreGive(s_product.stopped);
    ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED stopped=%d closed=%d",
             (int)stopped, (int)closed);
    atomic_store_explicit(&s_product.result, ESP_BASE_CONTAINER_BLOCKED,
                          memory_order_release);
    return NULL;
}

static bool ensure_provider(const esp_base_storage_claim_t *claim)
{
    if (!esp_base_storage_claim_active(claim) || !policy_present()) return false;
    if (s_product.provider_bound) return true;
    /* A failed bind leaves this boot blocked. Do not replace a lock which may
     * already be referenced by the provider or retry with partial state. */
    if (s_product.storage_lock != NULL) return false;
    if (!configure_policy()) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED incomplete product authorization");
        return false;
    }
    s_product.storage_lock = xSemaphoreCreateMutex();
    if (s_product.storage_lock == NULL) return false;
    const econtainer_slots_idf_config_t config = {
        .package_partition_label = CONFIG_ESP_BASE_CONTAINER_PACKAGE_LABEL,
        .package_partition_offset_bytes = CONFIG_ESP_BASE_CONTAINER_PACKAGE_OFFSET,
        .package_partition_size_bytes = CONFIG_ESP_BASE_CONTAINER_PACKAGE_SIZE,
        .slots = {
            {CONFIG_ESP_BASE_CONTAINER_SLOT_0_OFFSET, CONFIG_ESP_BASE_CONTAINER_SLOT_0_SIZE},
            {CONFIG_ESP_BASE_CONTAINER_SLOT_1_OFFSET, CONFIG_ESP_BASE_CONTAINER_SLOT_1_SIZE},
            {CONFIG_ESP_BASE_CONTAINER_SLOT_2_OFFSET, CONFIG_ESP_BASE_CONTAINER_SLOT_2_SIZE},
        },
        .nvs_partition_label = CONFIG_ESP_BASE_CONTAINER_NVS_LABEL,
        .nvs_partition_offset_bytes = CONFIG_ESP_BASE_CONTAINER_NVS_OFFSET,
        .nvs_partition_size_bytes = CONFIG_ESP_BASE_CONTAINER_NVS_SIZE,
        .nvs_namespace = "base_pkg",
        .nvs_key = "slots",
        .storage_lock = s_product.storage_lock,
    };
    if (!econtainer_slots_idf_bind(&s_product.provider, &config) ||
        nvs_flash_init_partition(CONFIG_ESP_BASE_CONTAINER_NVS_LABEL) != ESP_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED unapproved partition or NVS");
        return false;
    }
    s_product.provider_bound = true;
    return true;
}

static bool digest_zero(const uint8_t sha256[32])
{
    uint8_t value = 0;
    for (size_t index = 0; index < 32U; ++index) value |= sha256[index];
    return value == 0U;
}

/* Check the entire ECS2 identity set, not only a matching running entry.
 * The inactive confirmed package may exist at snapshot time; its reference
 * is validated by reconcile/retire before its binding is removed. */
static bool state_has_firmware(const econtainer_slots_state_t *state,
                               const uint8_t source_sha256[32],
                               const uint8_t other_sha256[32],
                               bool other_must_be_unpacked)
{
    bool source_found = false;
    bool other_found = false;
    const bool has_other = !digest_zero(other_sha256);
    for (size_t index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        const econtainer_slot_binding_t *binding = &state->bindings[index];
        if (!binding->present) continue;
        if (memcmp(binding->firmware_sha256, source_sha256, 32) == 0) {
            if (source_found || binding->package_present) return false;
            source_found = true;
        } else if (has_other &&
                   memcmp(binding->firmware_sha256, other_sha256, 32) == 0) {
            if (other_found || (other_must_be_unpacked && binding->package_present))
                return false;
            other_found = true;
        } else {
            return false;
        }
    }
    return source_found && other_found == has_other;
}

static econtainer_slots_result_t verified_a_only(
    const econtainer_slot_firmware_set_t *firmware_set)
{
    econtainer_slots_state_t state = {0};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    const econtainer_slots_result_t result = econtainer_slots_reconcile(
        &s_product.provider.io, &s_product.provider.geometry,
        firmware_set, &state, &decision);
    if (result != ECONTAINER_SLOTS_OK) return result;
    return state.phase == ECONTAINER_SLOT_IDLE &&
           decision == ECONTAINER_SLOT_BOOT_CONFIRMED &&
           state_has_firmware(&state, firmware_set->running_firmware_sha256,
                              (const uint8_t[32]){0}, false) ?
           ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
}

typedef struct {
    bool container_enabled;
    esp_base_ota_receipt_snapshot_t *snapshot;
} snapshot_context_t;

static econtainer_slots_result_t snapshot_for_ota(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    snapshot_context_t *entry = context;
    esp_base_ota_receipt_snapshot_t *snapshot = entry->snapshot;
    if (firmware_set->bootable_count < 1U || firmware_set->bootable_count > 2U)
        return ECONTAINER_SLOTS_CONFLICT;
    memcpy(snapshot->source_sha256, firmware_set->running_firmware_sha256, 32);
    if (firmware_set->bootable_count == 2U)
        memcpy(snapshot->inactive_sha256, firmware_set->bootable_firmware_sha256[1], 32);
    if (!entry->container_enabled) return ECONTAINER_SLOTS_OK;

    econtainer_slots_state_t state = {0};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    const econtainer_slots_result_t result = econtainer_slots_reconcile(
        &s_product.provider.io, &s_product.provider.geometry,
        firmware_set, &state, &decision);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (state.sequence == 0U ||
        (state.phase != ECONTAINER_SLOT_IDLE && state.phase != ECONTAINER_SLOT_CONFIRMED) ||
        decision != ECONTAINER_SLOT_BOOT_CONFIRMED ||
        !state_has_firmware(&state, snapshot->source_sha256,
                            snapshot->inactive_sha256, false))
        return ECONTAINER_SLOTS_CONFLICT;
    /* A-only after a previous retirement is IDLE. An A-only CONFIRMED blob
     * would have lost a previous candidate without an explicit transition. */
    if (firmware_set->bootable_count == 1U && state.phase != ECONTAINER_SLOT_IDLE)
        return ECONTAINER_SLOTS_CONFLICT;
    snapshot->container_enabled = true;
    snapshot->container_sequence = state.sequence;
    return ECONTAINER_SLOTS_OK;
}

bool esp_base_container_product_snapshot_for_ota(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_receipt_snapshot_t *snapshot)
{
    if (snapshot == NULL) return false;
    *snapshot = (esp_base_ota_receipt_snapshot_t){0};
    if (!esp_base_storage_claim_active(claim)) return false;
    const bool configured = policy_present();
    if (configured &&
        (!s_product.provider_bound || !esp_base_container_product_ota_ready()))
        return false;
    snapshot_context_t context = {.container_enabled = configured, .snapshot = snapshot};
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, snapshot_for_ota, &context);
    if (result == ECONTAINER_SLOTS_OK) return true;
    *snapshot = (esp_base_ota_receipt_snapshot_t){0};
    ESP_LOGE(TAG, "ESP_BASE_CONTAINER_SNAPSHOT_BLOCKED result=%d", (int)result);
    return false;
}

static esp_base_container_retire_result_t retire_result(econtainer_slots_result_t result)
{
    if (result == ECONTAINER_SLOTS_OK) return ESP_BASE_CONTAINER_RETIRE_COMPLETE;
    if (result == ECONTAINER_SLOTS_CONFLICT || result == ECONTAINER_SLOTS_UNTRUSTED ||
        result == ECONTAINER_SLOTS_EMPTY || result == ECONTAINER_SLOTS_INVALID ||
        result == ECONTAINER_SLOTS_NO_SPACE) return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    return ESP_BASE_CONTAINER_RETIRE_UNCERTAIN;
}

typedef struct {
    uint32_t expected_sequence;
    const uint8_t *source_sha256;
    const uint8_t *inactive_sha256;
} retire_context_t;

static econtainer_slots_result_t verify_source_only(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const retire_context_t *retire = context;
    return firmware_set->bootable_count == 1U &&
           memcmp(firmware_set->running_firmware_sha256,
                  retire->source_sha256, 32) == 0 ?
           ECONTAINER_SLOTS_OK : ECONTAINER_SLOTS_CONFLICT;
}

static econtainer_slots_result_t retire_inactive(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const retire_context_t *retire = context;
    if (firmware_set->bootable_count != 1U ||
        memcmp(firmware_set->running_firmware_sha256, retire->source_sha256, 32) != 0)
        return ECONTAINER_SLOTS_CONFLICT;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    const bool old_inactive = !digest_zero(retire->inactive_sha256);
    if (old_inactive && state.sequence == retire->expected_sequence &&
        (state.phase == ECONTAINER_SLOT_IDLE || state.phase == ECONTAINER_SLOT_CONFIRMED) &&
        state_has_firmware(&state, retire->source_sha256,
                            retire->inactive_sha256, false)) {
        result = econtainer_slots_retire_inactive_firmware(
            &s_product.provider.io, &s_product.provider.geometry,
            state.sequence, firmware_set, retire->inactive_sha256, &state);
        if (result != ECONTAINER_SLOTS_OK) return result;
    } else if (state.sequence != retire->expected_sequence + (old_inactive ? 1U : 0U) ||
               state.phase != ECONTAINER_SLOT_IDLE ||
               !state_has_firmware(&state, retire->source_sha256,
                                   (const uint8_t[32]){0}, false)) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    return verified_a_only(firmware_set);
}

esp_base_container_retire_result_t esp_base_container_product_retire_inactive(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32])
{
    if (!esp_base_storage_claim_active(claim) || source_sha256 == NULL ||
        inactive_sha256 == NULL || digest_zero(source_sha256) ||
        (!digest_zero(inactive_sha256) &&
         memcmp(source_sha256, inactive_sha256, 32) == 0) ||
        policy_present() != container_enabled ||
        (container_enabled ? expected_sequence == 0U : expected_sequence != 0U))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    if (container_enabled &&
        (!s_product.provider_bound || !esp_base_container_product_ota_ready()))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    retire_context_t context = {expected_sequence, source_sha256, inactive_sha256};
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        container_enabled ? retire_inactive :
            /* With no Container policy, still prove the physical A-only set. */
            verify_source_only, &context);
    return retire_result(result);
}

typedef struct {
    retire_context_t retire;
    const uint8_t *candidate_sha256;
    uint8_t operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    uint8_t boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
} recovery_context_t;

static bool exact_recovery_operation(const econtainer_slots_state_t *state,
                                     const recovery_context_t *recovery)
{
    return state->operation.firmware_transition &&
        state->operation.kind == ECONTAINER_SLOT_NO_PACKAGE &&
        memcmp(state->operation.operation_id, recovery->operation_id,
               sizeof recovery->operation_id) == 0 &&
        memcmp(state->operation.target_firmware_sha256,
               recovery->candidate_sha256, 32) == 0 &&
        state_has_firmware(state, recovery->retire.source_sha256,
                           recovery->candidate_sha256, true) &&
        /* A trial can be abandoned without a stop callback only on a
         * different boot, before product_boot creates its guest executor. */
        memcmp(state->operation.trial_boot_id, recovery->boot_id,
               sizeof recovery->boot_id) != 0;
}

static econtainer_slots_result_t recover_retired_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const recovery_context_t *recovery = context;
    const retire_context_t *retire = &recovery->retire;
    if (firmware_set->bootable_count != 1U ||
        memcmp(firmware_set->running_firmware_sha256, retire->source_sha256, 32) != 0)
        return ECONTAINER_SLOTS_CONFLICT;

    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    const bool old_inactive = !digest_zero(retire->inactive_sha256);
    const uint32_t retired_sequence = retire->expected_sequence +
                                      (old_inactive ? 1U : 0U);
    if (old_inactive && state.sequence == retire->expected_sequence &&
        (state.phase == ECONTAINER_SLOT_IDLE || state.phase == ECONTAINER_SLOT_CONFIRMED) &&
        state_has_firmware(&state, retire->source_sha256,
                            retire->inactive_sha256, false)) {
        return retire_inactive(firmware_set, (void *)retire);
    }
    if (state.phase == ECONTAINER_SLOT_IDLE &&
        state_has_firmware(&state, retire->source_sha256,
                            (const uint8_t[32]){0}, false)) {
        const uint32_t delta = state.sequence - retired_sequence;
        /* 0: retired before prepare; 3/4/5: stage, optional trial/health,
         * abandon, drop. No other sequence can be attributed to this receipt. */
        if (delta != 0U && delta != 3U && delta != 4U && delta != 5U)
            return ECONTAINER_SLOTS_CONFLICT;
        return verified_a_only(firmware_set);
    }
    if (!exact_recovery_operation(&state, recovery)) return ECONTAINER_SLOTS_CONFLICT;
    const uint32_t delta = state.sequence - retired_sequence;
    if ((state.phase == ECONTAINER_SLOT_PREPARED && delta != 1U) ||
        (state.phase == ECONTAINER_SLOT_TRIAL_STARTED && delta != 2U) ||
        (state.phase == ECONTAINER_SLOT_HEALTH_VERIFIED && delta != 3U) ||
        (state.phase == ECONTAINER_SLOT_ABORTED &&
         (delta < 2U || delta > 4U)) ||
        (state.phase != ECONTAINER_SLOT_PREPARED &&
         state.phase != ECONTAINER_SLOT_TRIAL_STARTED &&
         state.phase != ECONTAINER_SLOT_HEALTH_VERIFIED &&
         state.phase != ECONTAINER_SLOT_ABORTED)) {
        return ECONTAINER_SLOTS_CONFLICT;
    }
    if (state.phase != ECONTAINER_SLOT_ABORTED) {
        result = econtainer_slots_abandon(
            &s_product.provider.io, &s_product.provider.geometry,
            state.sequence, recovery->boot_id, NULL, NULL, &state);
        if (result != ECONTAINER_SLOTS_OK) return result;
        if (state.phase != ECONTAINER_SLOT_ABORTED ||
            !exact_recovery_operation(&state, recovery)) return ECONTAINER_SLOTS_UNCERTAIN;
    }
    result = econtainer_slots_drop_aborted_firmware(
        &s_product.provider.io, &s_product.provider.geometry,
        state.sequence, firmware_set, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (state.phase != ECONTAINER_SLOT_IDLE ||
        !state_has_firmware(&state, retire->source_sha256,
                            (const uint8_t[32]){0}, false))
        return ECONTAINER_SLOTS_UNCERTAIN;
    return verified_a_only(firmware_set);
}

esp_base_container_retire_result_t esp_base_container_product_recover_retired_firmware(
    const esp_base_storage_claim_t *claim, bool container_enabled,
    uint32_t expected_sequence, const uint8_t source_sha256[32],
    const uint8_t inactive_sha256[32], const uint8_t candidate_sha256[32],
    const char operation_id[ESP_BASE_OTA_OPERATION_ID_BYTES],
    const char boot_id[37])
{
    if (!esp_base_storage_claim_active(claim) || source_sha256 == NULL ||
        inactive_sha256 == NULL || candidate_sha256 == NULL ||
        operation_id == NULL || boot_id == NULL || digest_zero(source_sha256) ||
        digest_zero(candidate_sha256) ||
        (!digest_zero(inactive_sha256) &&
         memcmp(source_sha256, inactive_sha256, 32) == 0) ||
        policy_present() != container_enabled ||
        (container_enabled ? expected_sequence == 0U : expected_sequence != 0U) ||
        s_product.ready != NULL ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    recovery_context_t context = {
        .retire = {expected_sequence, source_sha256, inactive_sha256},
        .candidate_sha256 = candidate_sha256,
    };
    if (!decode_uuid(operation_id, context.operation_id) ||
        !decode_uuid(boot_id, context.boot_id))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    if (container_enabled && !ensure_provider(claim))
        return ESP_BASE_CONTAINER_RETIRE_BLOCKED;
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL,
        container_enabled ? recover_retired_firmware : verify_source_only,
        container_enabled ? (void *)&context : (void *)&context.retire);
    return retire_result(result);
}

static esp_base_container_boot_result_t start_product(
    const esp_base_storage_claim_t *claim, bool trial_mode, const char boot_id[37])
{
    if (!esp_base_storage_claim_active(claim)) return ESP_BASE_CONTAINER_BLOCKED;
    if (!policy_present()) return ESP_BASE_CONTAINER_NOT_CONFIGURED;
    if (s_product.ready != NULL) return ESP_BASE_CONTAINER_BLOCKED;
    if (!decode_uuid(boot_id, s_product.boot_id)) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED invalid boot identity");
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    if (!ensure_provider(claim)) return ESP_BASE_CONTAINER_BLOCKED;
    s_product.trial_mode = trial_mode;
    atomic_store_explicit(&s_product.stop_requested, false, memory_order_relaxed);
    atomic_store_explicit(&s_product.instance_active, false, memory_order_relaxed);
    atomic_store_explicit(&s_product.boot_admitted, false, memory_order_relaxed);
    atomic_store_explicit(&s_product.result, ESP_BASE_CONTAINER_BLOCKED,
                          memory_order_relaxed);
    s_product.claim = claim;
    s_product.ready = xSemaphoreCreateBinary();
    if (s_product.ready == NULL) return ESP_BASE_CONTAINER_BLOCKED;
    s_product.stopped = xSemaphoreCreateBinary();
    if (s_product.stopped == NULL) return ESP_BASE_CONTAINER_BLOCKED;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return ESP_BASE_CONTAINER_BLOCKED;
    const bool valid_thread =
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0 &&
        pthread_attr_setstacksize(&attributes,
            CONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES) == 0;
    pthread_t thread;
    const int created = valid_thread ?
        pthread_create(&thread, &attributes, product_thread, NULL) : -1;
    (void)pthread_attr_destroy(&attributes);
    if (created != 0) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED executor create=%d", created);
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    (void)xSemaphoreTake(s_product.ready, portMAX_DELAY);
    return (esp_base_container_boot_result_t)atomic_load_explicit(
        &s_product.result, memory_order_acquire);
}

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim, const char boot_id[37])
{
    return start_product(claim, false, boot_id);
}

esp_base_container_boot_result_t esp_base_container_product_start_trial(
    const esp_base_storage_claim_t *claim, const char boot_id[37])
{
    return start_product(claim, true, boot_id);
}

static econtainer_slots_result_t mark_trial_healthy(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)firmware_set;
    (void)context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (!state.operation.firmware_transition) return ECONTAINER_SLOTS_CONFLICT;
    return econtainer_slots_mark_healthy(
        &s_product.provider.io, &s_product.provider.geometry, state.sequence,
        s_product.boot_id, &state);
}

bool esp_base_container_product_mark_healthy(const esp_base_storage_claim_t *claim)
{
    const int result = atomic_load_explicit(&s_product.result, memory_order_acquire);
    if (!s_product.trial_mode || !esp_base_storage_claim_active(claim) ||
        result != ESP_BASE_CONTAINER_EMPTY) {
        return false;
    }
    const econtainer_slots_result_t marked = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL, NULL, mark_trial_healthy, NULL);
    if (marked != ECONTAINER_SLOTS_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_HEALTH_BLOCKED result=%d", (int)marked);
        return false;
    }
    return true;
}

typedef struct {
    const uint8_t *boot_id;
} confirm_context_t;

static econtainer_slots_result_t confirm_firmware(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const confirm_context_t *confirm = context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(
        &s_product.provider.io, &s_product.provider.geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) return result;
    if (!state.operation.firmware_transition) return ECONTAINER_SLOTS_CONFLICT;
    return econtainer_slots_confirm(
        &s_product.provider.io, &s_product.provider.geometry, state.sequence,
        firmware_set->running_firmware_sha256, confirm->boot_id, &state);
}

bool esp_base_container_product_confirm_firmware(const esp_base_storage_claim_t *claim)
{
    if (!s_product.trial_mode || !esp_base_storage_claim_active(claim)) return false;
    const confirm_context_t confirm = {.boot_id = s_product.boot_id};
    /* This observation independently requires signed C and otadata VALID,
     * after eota_confirm_pending returned and Base inspected the selector. */
    const econtainer_slots_result_t committed = esp_base_container_with_firmware_set(
        claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, confirm_firmware,
        (void *)&confirm);
    if (committed != ECONTAINER_SLOTS_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_CONFIRM_BLOCKED result=%d", (int)committed);
        return false;
    }
    return true;
}

bool esp_base_container_product_stop_trial(const esp_base_storage_claim_t *claim)
{
    if (!s_product.trial_mode) return true;
    if (!esp_base_storage_claim_active(claim) || s_product.stopped == NULL) return false;
    atomic_store_explicit(&s_product.stop_requested, true, memory_order_release);
    if (xSemaphoreTake(s_product.stopped, pdMS_TO_TICKS(5000U)) != pdTRUE) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STOP_UNPROVEN executor timeout");
        return false;
    }
    if (!s_product.stop_succeeded ||
        atomic_load_explicit(&s_product.instance_active, memory_order_acquire)) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_STOP_UNPROVEN native cleanup");
        return false;
    }
    return true;
}
