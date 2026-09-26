// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the real command branch and its asynchronous completion branch. */
#include "../components/device_protocol/esp_base_protocol.c"

static esp_base_storage_owner_t owner;
static esp_base_ota_receipt_result_t register_result, failure_record_result;
static eota_result_t prepare_result, select_result;
static esp_base_container_stage_result_t stage_result;
static bool product_configured, product_ota_ready, ota_ready_after_first;
static unsigned ota_ready_calls;
static bool worker_created;
static unsigned register_calls, failure_record_calls, task_calls, prepare_calls, stage_calls, select_calls, restart_calls;
static char latest_reply[1200];
static uint32_t fake_free_heap = 1000;

static void reset_case(void)
{
    esp_base_storage_owner_init(&owner);
    memset(&s_context, 0, sizeof s_context);
    s_context.device_id = "22222222-2222-4222-8222-222222222222";
    s_context.storage_owner = &owner;
    strcpy(s_boot_id, "33333333-3333-4333-8333-333333333333");
    memset(&s_guard, 0, sizeof s_guard);
    memset(s_outcomes, 0, sizeof s_outcomes);
    memset(s_frp_status_seen, 0, sizeof s_frp_status_seen);
    memset(&s_ota_storage_claim, 0, sizeof s_ota_storage_claim);
    memset(&s_ota_request, 0, sizeof s_ota_request);
    s_config_uncertain = s_ota_boot_uncertain = s_ota_active = s_trial_active = false;
    atomic_store(&s_ota_done, false);
    atomic_store(&s_ota_stage_uncertain, false);
    atomic_store(&s_ota_received, 0);
    esp_base_control_state_set_ota_pending(&s_control_state, false);
    esp_base_control_state_set_ota_download_active(&s_control_state, false);
    register_result = failure_record_result = ESP_BASE_OTA_RECEIPT_OK;
    prepare_result = select_result = EOTA_UPDATE_OK;
    stage_result = ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED;
    product_configured = false;
    product_ota_ready = true;
    ota_ready_after_first = true;
    ota_ready_calls = 0;
    worker_created = true;
    register_calls = failure_record_calls = task_calls = prepare_calls = stage_calls = select_calls = restart_calls = 0;
    latest_reply[0] = '\0';
    fake_free_heap = 1000;
}

static void start(unsigned request_number)
{
    char line[32];
    const int length = snprintf(line, sizeof line, "start-%u", request_number);
    assert(length > 0 && (size_t)length < sizeof line);
    s_reply_mqtt = true;
    handle_line(line, (size_t)length, NULL);
    s_reply_mqtt = false;
}

static void expect_reply(const char *state, const char *error)
{
    char field[100];
    (void)snprintf(field, sizeof field, "\"state\":\"%s\"", state);
    assert(strstr(latest_reply, field));
    if (error) {
        (void)snprintf(field, sizeof field, "\"error_code\":\"%s\"", error);
        assert(strstr(latest_reply, field));
    }
}

static void check_frp_status(const char *request, int expected_http,
                             const char *expected_error)
{
    char response[1024] = {0};
    size_t length = 0;
    const int http = handle_frp_status((const uint8_t *)request, strlen(request),
                                       response, sizeof response, &length, NULL);
    assert(http == expected_http && length > 0 && length < sizeof response);
    if (expected_error) assert(strstr(response, expected_error));
    else {
        char heap_field[48];
        snprintf(heap_field, sizeof heap_field, "\"free_heap\":%u", !strcmp(request, "status-1") ? 1000u : fake_free_heap);
        assert(strstr(response, "\"state\":\"succeeded\"") &&
               strstr(response, "\"frp\":\"stopped\"") && strstr(response, heap_field));
    }
}

int main(void)
{
    reset_case();
    check_frp_status("status-1", 200, NULL);
    fake_free_heap = 500;
    check_frp_status("status-1", 200, NULL);
    check_frp_status("changed-deadline", 409, "request_conflict");
    check_frp_status("changed-boot", 409, "request_conflict");
    check_frp_status("status-2", 200, NULL);
    check_frp_status("wrong-boot", 409, "wrong_boot");
    check_frp_status("wrong-device", 409, "wrong_device");
    check_frp_status("expired", 409, "expired");
    check_frp_status("far", 400, "invalid_deadline");
    check_frp_status("invalid", 400, "invalid_request");
    reset_case();
    esp_base_storage_claim_t other = {0};
    assert(esp_base_storage_claim(&owner, &other));
    start(1);
    expect_reply("failed", "operation_busy");
    assert(register_calls == 0 && task_calls == 0);
    assert(atomic_load(&owner.next_token) == other.token);
    assert(esp_base_storage_release(&other));

    reset_case();
    product_ota_ready = false;
    start(13);
    expect_reply("failed", "product_ota_unavailable");
    assert(register_calls == 0 && prepare_calls == 0 && stage_calls == 0);
    assert(atomic_load(&owner.active_token) == 0);

    reset_case();
    ota_ready_after_first = false;
    start(14);
    poll_ota();
    expect_reply("failed", "resource_failure");
    assert(ota_ready_calls == 2 && register_calls == 1 &&
           prepare_calls == 0 && stage_calls == 0 && select_calls == 0 &&
           atomic_load(&owner.active_token) == 0);

    reset_case();
    register_result = ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE;
    start(2);
    expect_reply("failed", "ota_slot_unavailable");
    assert(register_calls == 1 && task_calls == 0);
    assert(atomic_load(&owner.active_token) == 0);

    reset_case();
    register_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    start(3);
    expect_reply("unknown", "storage_uncertain");
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    assert(s_config_uncertain);

    reset_case();
    worker_created = false;
    start(4);
    expect_reply("failed", "resource_failure");
    assert(register_calls == 1 && failure_record_calls == 1 && task_calls == 1);
    assert(atomic_load(&owner.active_token) == 0);

    reset_case();
    worker_created = false;
    failure_record_result = ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
    start(5);
    expect_reply("unknown", "storage_uncertain");
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    prepare_result = EOTA_UPDATE_DOWNLOAD_FAILED;
    start(6);
    expect_reply("running", NULL);
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    poll_ota();
    expect_reply("failed", "download_failed");
    assert(failure_record_calls == 1 && atomic_load(&owner.active_token) == 0);

    reset_case();
    product_configured = true;
    prepare_result = EOTA_UPDATE_DOWNLOAD_FAILED;
    start(15);
    assert(prepare_calls == 1 && stage_calls == 0 && select_calls == 0);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    stage_result = ESP_BASE_CONTAINER_STAGE_PREPARED;
    start(9);
    assert(stage_calls == 1 && select_calls == 1);
    poll_ota();
    assert(restart_calls == 1 && failure_record_calls == 0);

    reset_case();
    stage_result = ESP_BASE_CONTAINER_STAGE_REJECTED;
    start(10);
    assert(stage_calls == 1 && select_calls == 0);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && s_config_uncertain &&
           atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    stage_result = ESP_BASE_CONTAINER_STAGE_UNCERTAIN;
    start(11);
    assert(stage_calls == 1 && select_calls == 0);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && s_config_uncertain && s_ota_boot_uncertain &&
           atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    stage_result = ESP_BASE_CONTAINER_STAGE_PREPARED;
    select_result = EOTA_UPDATE_RESOURCE_FAILURE;
    start(12);
    assert(stage_calls == 1 && select_calls == 1);
    poll_ota();
    expect_reply("unknown", "storage_uncertain");
    assert(failure_record_calls == 0 && atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    select_result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    start(8);
    assert(prepare_calls == 1 && select_calls == 1);
    poll_ota();
    expect_reply("unknown", "boot_state_unknown");
    assert(s_ota_boot_uncertain && failure_record_calls == 0);
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);

    reset_case();
    start(7);
    expect_reply("running", NULL);
    assert(prepare_calls == 1 && select_calls == 1);
    assert(atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    poll_ota();
    assert(restart_calls == 1 && atomic_load(&owner.active_token) == s_ota_storage_claim.token);
    start(7); /* Admission replay must not register or download again. */
    assert(register_calls == 1 && task_calls == 1);
    puts("  protocol_ota_owner passed (busy, receipt failure, worker failure, async success and retained owner)");
}

const char *ebase_parse_command(const char *line, size_t length, ebase_command_t *out)
{
    unsigned number = 0;
    assert(length > 6 && sscanf(line, "start-%u", &number) == 1);
    memset(out, 0, sizeof *out);
    out->kind = EBASE_OTA_START;
    snprintf(out->request.request_id, sizeof out->request.request_id,
             "11111111-1111-4111-8111-%012u", number);
    strcpy(out->request.device_id, "22222222-2222-4222-8222-222222222222");
    strcpy(out->request.boot_id, "33333333-3333-4333-8333-333333333333");
    out->request.expires_at_ms = 10000;
    snprintf(out->ota.operation_id, sizeof out->ota.operation_id,
             "44444444-4444-4444-8444-%012u", number);
    strcpy(out->ota.image_url, "https://example.invalid/signed.bin");
    out->ota.image_size_bytes = 4096;
    memset(out->ota.sha256, 0x5a, sizeof out->ota.sha256);
    return NULL;
}

const char *ebase_parse_frp_status(const char *json, size_t length, ebase_request_t *out)
{
    (void)length;
    memset(out, 0, sizeof *out);
    if (!strcmp(json, "invalid")) return "invalid_request";
    strcpy(out->request_id, "11111111-1111-4111-8111-111111111111");
    out->request_id[35] = !strcmp(json, "status-2") ? '2' :
        !strcmp(json, "wrong-boot") ? '3' :
        !strcmp(json, "wrong-device") ? '4' :
        !strcmp(json, "expired") ? '5' :
        !strcmp(json, "far") ? '6' : '1';
    strcpy(out->device_id, !strcmp(json, "wrong-device") ?
        "99999999-9999-4999-8999-999999999999" : "22222222-2222-4222-8222-222222222222");
    strcpy(out->boot_id, (!strcmp(json, "wrong-boot") || !strcmp(json, "changed-boot")) ?
        "88888888-8888-4888-8888-888888888888" : "33333333-3333-4333-8333-333333333333");
    out->expires_at_ms = !strcmp(json, "expired") ? 1000 :
        !strcmp(json, "far") ? 32000 :
        !strcmp(json, "changed-deadline") ? 10001 : 10000;
    return NULL;
}

psa_status_t psa_hash_compute(int algorithm, const uint8_t *bytes, size_t length,
                              uint8_t *out, size_t out_size, size_t *actual)
{
    assert(algorithm == PSA_ALG_SHA_256 && bytes && length && out_size >= 32);
    memset(out, 0xa5, 32);
    *actual = 32;
    return PSA_SUCCESS;
}

bool eota_available(void) { return true; }
bool esp_base_wifi_ready(void) { return true; }
bool esp_base_time_ready(void) { return true; }
const char *esp_base_wifi_state(void) { return "ready"; }
const char *esp_base_mqtt_owner_state(void) { return "ready"; }
esp_base_frp_snapshot_t esp_base_frp_owner_snapshot(void) { return (esp_base_frp_snapshot_t){.state = "stopped"}; }
uint32_t esp_get_free_heap_size(void) { return fake_free_heap; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 1000; }
int64_t esp_timer_get_time(void) { return 1000000; }
bool esp_base_mqtt_owner_result(const char *json, size_t length)
{
    assert(length < sizeof latest_reply);
    memcpy(latest_reply, json, length);
    latest_reply[length] = '\0';
    return true;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_register(
    const char *device_id, const esp_base_ota_request_t *request)
{
    assert(device_id && request);
    ++register_calls;
    return register_result;
}

esp_base_ota_receipt_result_t esp_base_ota_receipt_record_failure(
    const char *device_id, const char *operation_id, eota_result_t error)
{
    assert(device_id && operation_id && error != EOTA_UPDATE_OK);
    ++failure_record_calls;
    return failure_record_result;
}

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack_depth,
                       void *argument, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack_depth; (void)priority; (void)handle;
    ++task_calls;
    if (!worker_created) return pdFALSE;
    task(argument);
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void esp_restart(void) { ++restart_calls; }

eota_policy_t esp_base_ota_policy(bool trusted_time)
{
    assert(trusted_time);
    return (eota_policy_t){0};
}
eota_result_t eota_prepare(const eota_policy_t *policy, const eota_image_t *image,
                           eota_progress_t progress, void *context, eota_prepared_t *prepared)
{
    assert(policy && image && progress && prepared);
    (void)context;
    ++prepare_calls;
    return prepare_result;
}
eota_result_t eota_select(const eota_policy_t *policy, const eota_prepared_t *prepared)
{
    assert(policy && prepared);
    ++select_calls;
    return select_result;
}
const char *eota_error(eota_result_t result)
{
    return result == EOTA_UPDATE_DOWNLOAD_FAILED ? "download_failed" :
        result == EOTA_UPDATE_RESOURCE_FAILURE ? "resource_failure" : "boot_state_unknown";
}

esp_base_container_stage_result_t esp_base_container_product_stage_firmware(
    const esp_base_storage_claim_t *claim, const eota_prepared_t *prepared,
    const char operation_id[37])
{
    assert(esp_base_storage_claim_active(claim) && prepared != NULL &&
           operation_id != NULL && operation_id[0] == '4');
    ++stage_calls;
    return stage_result;
}

bool esp_base_container_product_ota_ready(void)
{
    ++ota_ready_calls;
    return product_ota_ready && (ota_ready_calls == 1 || ota_ready_after_first);
}

bool esp_base_container_product_configured(void)
{
    return product_configured;
}

bool ebase_config_encode(const esp_base_remote_config_t *config,
                         uint8_t out[EBASE_CONFIG_MAX_BYTES], size_t *written)
{
    (void)config; (void)out; (void)written;
    assert(false && "config.set is outside this test");
    return false;
}
bool esp_base_remote_config_with_canonical_bytes(const esp_base_remote_config_t *config,
                                                 esp_base_config_bytes_consumer_t consume,
                                                 void *context)
{
    (void)config; (void)consume; (void)context;
    assert(false && "config.set is outside this test");
    return false;
}
esp_base_ota_receipt_result_t esp_base_ota_receipt_query(
    const char *device_id, const char *operation_id, bool worker_active,
    esp_base_ota_receipt_view_t *view)
{
    (void)device_id; (void)operation_id; (void)worker_active; (void)view;
    assert(false && "ota.result is outside this test");
    return ESP_BASE_OTA_RECEIPT_NOT_FOUND;
}
esp_err_t esp_base_wifi_apply(const ebase_wifi_config_t *config, uint64_t now_ms)
{
    (void)config; (void)now_ms;
    assert(false && "config.set is outside this test");
    return ESP_FAIL;
}
psa_status_t psa_hash_setup(psa_hash_operation_t *operation, int algorithm)
{
    (void)operation; (void)algorithm;
    assert(false && "config.set is outside this test");
    return -1;
}
psa_status_t psa_hash_update(psa_hash_operation_t *operation, const uint8_t *bytes, size_t length)
{
    (void)operation; (void)bytes; (void)length;
    assert(false && "config.set is outside this test");
    return -1;
}
psa_status_t psa_hash_finish(psa_hash_operation_t *operation, uint8_t *out, size_t out_size, size_t *actual)
{
    (void)operation; (void)out; (void)out_size; (void)actual;
    assert(false && "config.set is outside this test");
    return -1;
}
psa_status_t psa_hash_abort(psa_hash_operation_t *operation)
{
    (void)operation;
    assert(false && "config.set is outside this test");
    return -1;
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    assert(false && "restart is outside this test");
}
