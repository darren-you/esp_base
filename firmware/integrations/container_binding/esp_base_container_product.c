// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_product.h"

#include <limits.h>
#include <pthread.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_base_container_binding.h"
#include "esp_container_package_slot.h"
#include "esp_container_product.h"
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
    SemaphoreHandle_t storage_lock;
    const esp_base_storage_claim_t *claim;
    esp_base_container_boot_result_t result;
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

bool esp_base_container_product_pending_blocked(void)
{
    return policy_present();
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
    uint32_t sequence;
    econtainer_runtime_t *runtime;
    econtainer_runtime_result_t runtime_result;
} open_context_t;

static econtainer_slots_result_t open_confirmed(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    open_context_t *open = context;
    const econtainer_slot_selection_request_t request = {
        .expected_sequence = open->sequence,
        .firmware_set = *firmware_set,
        .selection = ECONTAINER_SLOT_SELECT_CONFIRMED,
    };
    const econtainer_slot_runtime_result_t result = econtainer_product_open(
        &s_product.provider.io, &s_product.provider.geometry, &request,
        &s_product.validation, &s_product.limits, &open->runtime);
    open->runtime_result = result.runtime;
    return result.slots;
}

static void report_result(esp_base_container_boot_result_t result)
{
    s_product.result = result;
    xSemaphoreGive(s_product.ready);
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
    econtainer_slots_state_t state = {0};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    const econtainer_slots_result_t reconcile = esp_base_container_reconcile(
        s_product.claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        &s_product.provider.io, &s_product.provider.geometry, &state, &decision);
    if (reconcile == ECONTAINER_SLOTS_EMPTY) {
        report_result(ESP_BASE_CONTAINER_EMPTY);
        return NULL;
    }
    if (reconcile != ECONTAINER_SLOTS_OK ||
        (decision != ECONTAINER_SLOT_BOOT_CONFIRMED &&
         decision != ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED)) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED reconcile=%d decision=%d",
                 (int)reconcile, (int)decision);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }

    open_context_t open = {.sequence = state.sequence,
                           .runtime_result = ECONTAINER_RUNTIME_INVALID_STATE};
    const econtainer_slots_result_t slots = esp_base_container_with_firmware_set(
        s_product.claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, open_confirmed, &open);
    if (slots != ECONTAINER_SLOTS_OK ||
        open.runtime_result != ECONTAINER_RUNTIME_OK || open.runtime == NULL) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED open_slots=%d open_runtime=%d",
                 (int)slots, (int)open.runtime_result);
        if (open.runtime != NULL) (void)econtainer_product_close(&open.runtime);
        report_result(slots == ECONTAINER_SLOTS_EMPTY ? ESP_BASE_CONTAINER_EMPTY :
                      ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    const econtainer_runtime_result_t initialized = econtainer_product_init(open.runtime);
    if (initialized != ECONTAINER_RUNTIME_OK) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED init=%d", (int)initialized);
        (void)econtainer_product_close(&open.runtime);
        report_result(ESP_BASE_CONTAINER_BLOCKED);
        return NULL;
    }
    ESP_LOGI(TAG, "ESP_BASE_CONTAINER_RUNNING confirmed_sequence=%u", (unsigned)state.sequence);
    report_result(ESP_BASE_CONTAINER_RUNNING);

    for (;;) {
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
    ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED stopped=%d closed=%d",
             (int)stopped, (int)closed);
    /* The app/otadata owner stays claimed after a guest failure. */
    return NULL;
}

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim)
{
    if (!esp_base_storage_claim_active(claim)) return ESP_BASE_CONTAINER_BLOCKED;
    if (!policy_present()) return ESP_BASE_CONTAINER_NOT_CONFIGURED;
    if (!configure_policy()) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED incomplete product authorization");
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    s_product.claim = claim;
    s_product.storage_lock = xSemaphoreCreateMutex();
    if (s_product.storage_lock == NULL) return ESP_BASE_CONTAINER_BLOCKED;
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
        return ESP_BASE_CONTAINER_BLOCKED;
    }
    s_product.ready = xSemaphoreCreateBinary();
    if (s_product.ready == NULL) return ESP_BASE_CONTAINER_BLOCKED;
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
    return s_product.result;
}
