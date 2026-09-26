// SPDX-License-Identifier: Apache-2.0
#include "esp_app_desc.h"
#include "esp_base_identity.h"
#include "eota.h"
#include "esp_ota_ops.h"
#include "esp_base_protocol.h"
#include "esp_base_remote_config.h"
#include "esp_base_safety.h"
#include "esp_base_time.h"
#include "esp_base_container_product.h"
#include "freertos/task.h"

#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void app_main(void);

static eota_state_t image_state;
static eota_state_t state_after_mark;
static esp_err_t inspect_result, nvs_result, identity_result, safety_result, config_result;
static esp_err_t protocol_result, mark_result, rollback_result, time_result;
static uint64_t now_ms;
static uint64_t last_control_progress_ms;
static uint32_t control_progress_count;
static unsigned nvs_calls, config_load_calls, protocol_calls, mark_calls, rollback_calls, time_calls, ready_logs, recovery_logs;
static bool protocol_started, control_never_ready, control_stalls;
static bool control_exits_late, control_pauses_cross_window;
static bool ota_gate_pending;
static bool container_pending_blocked;
static esp_base_container_boot_result_t container_boot_result;
static unsigned container_boot_calls;
static esp_base_storage_owner_t *storage_owner;
static unsigned ota_gate_clears;
static jmp_buf reboot_target;

static void reset_case(void)
{
    image_state = EOTA_STATE_PENDING_VERIFY;
    state_after_mark = EOTA_STATE_VALID;
    inspect_result = nvs_result = identity_result = safety_result = config_result = ESP_OK;
    protocol_result = mark_result = rollback_result = time_result = ESP_OK;
    now_ms = last_control_progress_ms = 0;
    control_progress_count = 0;
    nvs_calls = config_load_calls = protocol_calls = mark_calls = rollback_calls = time_calls = ready_logs = recovery_logs = 0;
    protocol_started = control_never_ready = control_stalls = ota_gate_pending = false;
    container_pending_blocked = false;
    container_boot_result = ESP_BASE_CONTAINER_NOT_CONFIGURED;
    container_boot_calls = 0;
    control_exits_late = control_pauses_cross_window = false;
    ota_gate_clears = 0;
    storage_owner = NULL;
}

void test_log(const char *format, ...)
{
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (strstr(line, "ESP_BASE_READY")) ++ready_logs;
    if (strstr(line, "ESP_BASE_OTA_RECOVERY_REQUIRED")) ++recovery_logs;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "injected";
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)now_ms * 1000;
}

static void maybe_control_progress(void)
{
    if (!protocol_started || control_never_ready || (control_stalls && now_ms >= 10000) ||
        (control_exits_late && now_ms > 29000) ||
        (control_pauses_cross_window && now_ms >= 29000 && now_ms < 33000)) {
        return;
    }
    if (control_progress_count == 0 || last_control_progress_ms != now_ms) {
        ++control_progress_count;
        last_control_progress_ms = now_ms;
    }
}

void vTaskDelay(TickType_t ticks)
{
    now_ms += ticks;
    maybe_control_progress();
}

esp_err_t eota_inspect(eota_current_t *current)
{
    current->running_partition = "ota_1";
    current->state = inspect_result == ESP_OK ? image_state : EOTA_STATE_UNKNOWN;
    return inspect_result;
}
const char *eota_state_name(eota_state_t state)
{
    return state == EOTA_STATE_PENDING_VERIFY ? "pending_verify" :
        state == EOTA_STATE_VALID ? "valid" : "unknown";
}
esp_err_t eota_confirm_pending(eota_current_t *current)
{
    assert(current->state == EOTA_STATE_PENDING_VERIFY);
    ++mark_calls;
    assert(ota_gate_pending);
    esp_base_storage_claim_t competing = {0};
    assert(storage_owner != NULL && !esp_base_storage_claim(storage_owner, &competing));
    image_state = state_after_mark;
    current->state = image_state;
    return image_state == EOTA_STATE_VALID ? ESP_OK :
        mark_result == ESP_OK ? ESP_ERR_INVALID_STATE : mark_result;
}
esp_err_t eota_reject_pending(eota_current_t *current)
{
    assert(current->state == EOTA_STATE_PENDING_VERIFY);
    ++rollback_calls;
    assert(ota_gate_pending);
    if (storage_owner != NULL) {
        esp_base_storage_claim_t competing = {0};
        assert(!esp_base_storage_claim(storage_owner, &competing));
    }
    if (rollback_result == ESP_OK) {
        image_state = EOTA_STATE_INVALID;
        longjmp(reboot_target, 1);
    }
    return rollback_result;
}

esp_err_t nvs_flash_init(void)
{
    ++nvs_calls;
    return nvs_result;
}

esp_err_t esp_base_identity_read(esp_base_identity_t *identity)
{
    if (identity_result == ESP_OK) {
        (void)snprintf(identity->device_id, sizeof identity->device_id,
                       "00000000-0000-4000-8000-000000000001");
        identity->model = "esp32c3";
        identity->revision = 4;
        identity->flash_size_bytes = 4 * 1024 * 1024;
    }
    return identity_result;
}

esp_err_t esp_base_safety_start(esp_base_safety_t *safety)
{
    safety->reset_reason = "power_on";
    return safety_result;
}

esp_err_t esp_base_remote_config_load(esp_base_remote_config_t *config)
{
    *config = (esp_base_remote_config_t){0};
    if (config_result == ESP_OK) config->revision = 7;
    return config_result;
}

esp_err_t esp_base_protocol_load_config(uint32_t *revision)
{
    ++config_load_calls;
    esp_base_remote_config_t config;
    const esp_err_t result = esp_base_remote_config_load(&config);
    if (result == ESP_OK) *revision = config.revision;
    return result;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {.version = "test"};
    return &app;
}

const char *esp_get_idf_version(void)
{
    return "test";
}

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context)
{
    assert(context != NULL);
    assert(context->storage_owner != NULL);
    assert(config_load_calls == 1 && config_result == ESP_OK);
    storage_owner = context->storage_owner;
    esp_base_storage_claim_t competing = {0};
    assert(!esp_base_storage_claim(storage_owner, &competing));
    assert(ota_gate_pending == (image_state == EOTA_STATE_PENDING_VERIFY));
    ++protocol_calls;
    protocol_started = protocol_result == ESP_OK;
    maybe_control_progress();
    return protocol_result;
}

esp_err_t esp_base_time_start(const char *server)
{
    assert(server != NULL && !strcmp(server, "time.example.invalid"));
    ++time_calls;
    return time_result;
}

bool esp_base_protocol_control_healthy(void)
{
    return protocol_started && control_progress_count != 0 &&
           now_ms - last_control_progress_ms <= 5000;
}

uint32_t esp_base_protocol_control_progress_count(void)
{
    return control_progress_count;
}

void esp_base_protocol_set_ota_verification_pending(bool pending)
{
    ota_gate_pending = pending;
    if (!pending) ++ota_gate_clears;
}

bool esp_base_container_product_pending_blocked(void)
{
    return container_pending_blocked;
}

esp_base_container_boot_result_t esp_base_container_product_boot(
    const esp_base_storage_claim_t *claim)
{
    ++container_boot_calls;
    assert(esp_base_storage_claim_active(claim));
    esp_base_storage_claim_t competing = {0};
    assert(!esp_base_storage_claim(storage_owner, &competing));
    return container_boot_result;
}

static bool rebooted(void)
{
    if (setjmp(reboot_target) == 0) {
        app_main();
        return false;
    }
    return true;
}

int main(void)
{
    reset_case();
    image_state = EOTA_STATE_VALID;
    nvs_result = ESP_FAIL;
    assert(!rebooted() && rollback_calls == 0 && recovery_logs == 0);

    reset_case();
    inspect_result = ESP_FAIL;
    assert(!rebooted() && nvs_calls == 0 && rollback_calls == 0 && recovery_logs == 1);

    reset_case();
    nvs_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 0 && mark_calls == 0);

    reset_case();
    identity_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 0);

    reset_case();
    safety_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 0);

    reset_case();
    config_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && config_load_calls == 1 && protocol_calls == 0);

    reset_case();
    protocol_result = ESP_FAIL;
    assert(rebooted() && rollback_calls == 1 && protocol_calls == 1 && time_calls == 0);

    reset_case();
    time_result = ESP_FAIL;
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0 && ready_logs == 1);
    assert(time_calls == 1 && !ota_gate_pending);
    esp_base_storage_claim_t after_ready = {0};
    assert(esp_base_storage_claim(storage_owner, &after_ready));
    assert(esp_base_storage_release(&after_ready));

    reset_case();
    control_never_ready = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms == 5000);

    reset_case();
    control_stalls = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms >= 15000);

    reset_case();
    control_exits_late = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 && now_ms > 30000);

    reset_case();
    control_pauses_cross_window = true;
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0 && ready_logs == 1);
    assert(now_ms >= 33000 && now_ms < 34000);

    reset_case();
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0 && ready_logs == 1);
    assert(!ota_gate_pending && ota_gate_clears == 1);
    assert(now_ms >= 30000 && image_state == EOTA_STATE_VALID);

    reset_case();
    mark_result = ESP_FAIL;
    state_after_mark = EOTA_STATE_PENDING_VERIFY;
    assert(rebooted() && mark_calls == 1 && rollback_calls == 1 && ready_logs == 0);

    reset_case();
    mark_result = ESP_FAIL;
    state_after_mark = EOTA_STATE_VALID;
    assert(!rebooted() && mark_calls == 1 && rollback_calls == 0);
    assert(ready_logs == 1 && recovery_logs == 0 && image_state == EOTA_STATE_VALID);
    assert(!ota_gate_pending && ota_gate_clears == 1);

    reset_case();
    nvs_result = ESP_FAIL;
    rollback_result = ESP_ERR_OTA_ROLLBACK_FAILED;
    assert(!rebooted() && rollback_calls == 1 && recovery_logs == 1);
    assert(image_state == EOTA_STATE_PENDING_VERIFY && ota_gate_pending);

    reset_case();
    container_pending_blocked = true;
    assert(rebooted() && rollback_calls == 1 && mark_calls == 0 &&
           container_boot_calls == 0 && ready_logs == 0);

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_boot_result = ESP_BASE_CONTAINER_RUNNING;
    assert(!rebooted() && container_boot_calls == 1 && ready_logs == 1);
    esp_base_storage_claim_t running_competitor = {0};
    assert(!esp_base_storage_claim(storage_owner, &running_competitor));

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_boot_result = ESP_BASE_CONTAINER_EMPTY;
    assert(!rebooted() && container_boot_calls == 1 && ready_logs == 1);
    esp_base_storage_claim_t empty_competitor = {0};
    assert(!esp_base_storage_claim(storage_owner, &empty_competitor));

    reset_case();
    image_state = EOTA_STATE_VALID;
    container_boot_result = ESP_BASE_CONTAINER_BLOCKED;
    assert(!rebooted() && container_boot_calls == 1 && ready_logs == 0);
    esp_base_storage_claim_t blocked_competitor = {0};
    assert(!esp_base_storage_claim(storage_owner, &blocked_competitor));

    puts("  ota_startup passed (startup faults, control progress, rollback, readback)");
}
