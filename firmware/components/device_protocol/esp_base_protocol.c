// SPDX-License-Identifier: Apache-2.0
#include "esp_base_protocol.h"
#include "control_state.h"
#include "esp_base_command.h"
#include "esp_base_identity.h"
#include "esp_base_remote_config.h"
#include "esp_base_mqtt_owner.h"
#include "esp_base_frp_owner.h"
#include "esp_base_frp_status_listener.h"
#include "esp_base_wifi.h"
#include "esp_base_time.h"
#include "esp_base_ota_policy.h"
#include "esp_base_ota_receipt.h"
#include "esp_base_container_product.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#include "driver/usb_serial_jtag_vfs.h"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/uart_vfs.h"
#else
#error "ESP Base serial control supports only esp32c3 and esp32"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    const char *device_id;
    const char *firmware_version;
    const char *chip_model;
    uint32_t flash_size_bytes;
    esp_base_remote_config_t config;
    const char *reset_reason;
    esp_base_storage_owner_t *storage_owner;
} protocol_state_t;
static protocol_state_t s_context;
static char s_boot_id[EBASE_ID_BYTES];
static ebase_request_guard_t s_guard;
static ebase_line_reader_t s_reader;
static bool s_started, s_config_loaded, s_config_uncertain, s_trial_active;
static bool s_ota_active, s_ota_boot_uncertain;
static esp_base_storage_claim_t s_ota_storage_claim;
static size_t s_ota_slot;
static esp_base_ota_request_t s_ota_request;
static atomic_bool s_ota_done;
static atomic_bool s_ota_stage_uncertain;
static atomic_int s_ota_result;
static atomic_uint_fast32_t s_ota_received;
static esp_base_control_state_t s_control_state;
static size_t s_trial_slot;
static uint64_t s_trial_deadline;
static esp_base_remote_config_t s_candidate;
static ebase_command_t command;
static bool s_reply_mqtt, s_mqtt_revision_set;
static uint32_t s_mqtt_revision;
static bool s_frp_revision_set;
static uint32_t s_frp_revision;

const char *esp_base_protocol_boot_id(void)
{
    return s_started && ebase_is_uuid(s_boot_id) ? s_boot_id : NULL;
}
static char s_mqtt_result_json[1024], s_mqtt_reported_json[512];
#define FRP_STATUS_REPLAY_SLOTS 8u
typedef struct {
    uint64_t uptime;
    uint32_t revision, free_heap, minimum_free_heap, ota_received, ota_total;
    const char *wifi, *mqtt, *frp, *config, *ota;
} status_snapshot_t;
static struct {
    char request_id[EBASE_ID_BYTES];
    uint64_t expires_at_ms;
    /* Capability names come from static owner state strings, not transient
     * network buffers; retain the first read-only result for same-ID retry. */
    status_snapshot_t status;
} s_frp_status_seen[FRP_STATUS_REPLAY_SLOTS];
typedef struct {
    const char *state, *error;
    bool has_status, via_mqtt;
    status_snapshot_t status;
} command_outcome_t;
static command_outcome_t s_outcomes[EBASE_REQUEST_SLOTS];

static bool fingerprint_config_bytes(const uint8_t *bytes, size_t length, void *context)
{
    uint8_t *fingerprint = context;
    size_t hash_size = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&hash, (const uint8_t *)"config.set", 10) != PSA_SUCCESS ||
        psa_hash_update(&hash, bytes, length) != PSA_SUCCESS ||
        psa_hash_finish(&hash, fingerprint, 32, &hash_size) != PSA_SUCCESS || hash_size != 32) {
        (void)psa_hash_abort(&hash);
        return false;
    }
    return true;
}

static uint64_t uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void reported(void)
{
    const uint64_t now = uptime_ms();
    const bool time_ready = esp_base_time_ready();
    const esp_base_frp_snapshot_t frp = esp_base_frp_owner_snapshot();
    ESP_LOGI("base_reported",
        "ESP_BASE_REPORTED schema=1 boot_id=%s uptime_ms=%" PRIu64
        " free_heap=%" PRIu32 " min_free_heap=%" PRIu32
        " device_id=%s firmware=%s chip=%s flash=%" PRIu32 " config_generation=%" PRIu32
        " reset=%s provisioned=%s wifi_state=%s time_ready=%s mqtt_state=%s frp_state=%s frp_attempts=%" PRIu64 " frp_sessions=%" PRIu64 " frp_pongs=%" PRIu64 " frp_active=%" PRIu32 " frp_error=%" PRId32 " ota_received=%" PRIu32 " ota_total=%" PRIu32,
        s_boot_id, now, esp_get_free_heap_size(),
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT), s_context.device_id,
        s_context.firmware_version, s_context.chip_model, s_context.flash_size_bytes,
        s_context.config.revision, s_context.reset_reason,
        s_context.config.wifi.configured ? "true" : "false", esp_base_wifi_state(),
        time_ready ? "true" : "false", esp_base_mqtt_owner_state(), frp.state,
        frp.attempts, frp.ready_sessions, frp.pongs, frp.work_active, frp.error,
        (uint32_t)atomic_load_explicit(&s_ota_received, memory_order_relaxed),
        s_ota_active ? s_ota_request.image_size_bytes : 0);
    const int size = snprintf(s_mqtt_reported_json, sizeof s_mqtt_reported_json,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"uptime_ms\":%" PRIu64 ",\"revision\":%" PRIu32 ","
        "\"wifi_state\":\"%s\",\"time_ready\":%s,\"frp_state\":\"%s\"}",
        s_context.device_id, s_boot_id, now, s_context.config.revision,
        esp_base_wifi_state(), time_ready ? "true" : "false", frp.state);
    if (size > 0 && (size_t)size < sizeof s_mqtt_reported_json)
        (void)esp_base_mqtt_owner_reported(s_mqtt_reported_json, (size_t)size);
}

static status_snapshot_t snapshot(void)
{
    return (status_snapshot_t){.uptime = uptime_ms(), .revision = s_context.config.revision,
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
        .ota_received = (uint32_t)atomic_load_explicit(&s_ota_received, memory_order_relaxed),
        .ota_total = s_ota_active ? s_ota_request.image_size_bytes : 0,
        .wifi = esp_base_wifi_state(), .mqtt = esp_base_mqtt_owner_state(),
        .frp = esp_base_frp_owner_snapshot().state,
        .config = s_config_uncertain || s_ota_boot_uncertain ? "failed" : "ready",
        .ota = !eota_available() ? "unsupported" : s_ota_active ? "running" : "ready"};
}

static int format_result_json(char *response, size_t capacity,
                              const char *request_id, const char *state,
                              const char *error, const status_snapshot_t *status)
{
    const int written = status ? snprintf(response, capacity,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":%s%s%s,\"state\":\"%s\",\"error_code\":%s%s%s,"
        "\"result\":{\"uptime_ms\":%" PRIu64 ",\"revision\":%" PRIu32
        ",\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32
        ",\"ota_received_bytes\":%" PRIu32 ",\"ota_total_bytes\":%" PRIu32
        ",\"capabilities\":{\"wifi\":\"%s\",\"mqtt\":\"%s\","
        "\"frp\":\"%s\",\"config\":\"%s\",\"ota\":\"%s\"}}}",
        s_context.device_id, s_boot_id,
        request_id ? "\"" : "null", request_id ? request_id : "", request_id ? "\"" : "",
        state, error ? "\"" : "null", error ? error : "", error ? "\"" : "",
        status->uptime, status->revision, status->free_heap, status->minimum_free_heap,
        status->ota_received, status->ota_total, status->wifi, status->mqtt,
        status->frp, status->config, status->ota) : snprintf(response, capacity,
        "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"request_id\":%s%s%s,\"state\":\"%s\",\"error_code\":%s%s%s,\"result\":null}",
        s_context.device_id, s_boot_id,
        request_id ? "\"" : "null", request_id ? request_id : "", request_id ? "\"" : "",
        state, error ? "\"" : "null", error ? error : "", error ? "\"" : "");
    return written >= 0 && (size_t)written < capacity ? written : -1;
}

static int handle_frp_status(const uint8_t *json, size_t json_length,
                             char *response, size_t capacity,
                             size_t *response_length, void *context)
{
    (void)context;
    ebase_request_t request;
    const char *parse_error = ebase_parse_frp_status((const char *)json, json_length, &request);
    const uint64_t now = uptime_ms();
    const char *error = parse_error;
    const char *state = "failed";
    int http_status = 400;
    size_t empty = FRP_STATUS_REPLAY_SLOTS;
    const status_snapshot_t *cached = NULL;
    if (!error) {
        for (size_t i = 0; i < FRP_STATUS_REPLAY_SLOTS; ++i) {
            if (s_frp_status_seen[i].expires_at_ms <= now) {
                if (empty == FRP_STATUS_REPLAY_SLOTS) empty = i;
            } else if (!strcmp(s_frp_status_seen[i].request_id, request.request_id)) {
                if (strcmp(request.device_id, s_context.device_id) ||
                    strcmp(request.boot_id, s_boot_id) ||
                    request.expires_at_ms != s_frp_status_seen[i].expires_at_ms) {
                    error = "request_conflict";
                    http_status = 409;
                } else cached = &s_frp_status_seen[i].status;
                break;
            }
        }
    }
    if (!error && strcmp(request.device_id, s_context.device_id)) {
        error = "wrong_device";
        http_status = 409;
    } else if (!error && strcmp(request.boot_id, s_boot_id)) {
        error = "wrong_boot";
        http_status = 409;
    } else if (!error && request.expires_at_ms <= now) {
        error = "expired";
        state = "expired";
        http_status = 409;
    } else if (!error && request.expires_at_ms - now > EBASE_REQUEST_WINDOW_MS) {
        error = "invalid_deadline";
    }
    if (!error && !cached && empty == FRP_STATUS_REPLAY_SLOTS) error = "capacity_exceeded";
    if (!error && !cached) {
        memcpy(s_frp_status_seen[empty].request_id, request.request_id, EBASE_ID_BYTES);
        s_frp_status_seen[empty].expires_at_ms = request.expires_at_ms;
        s_frp_status_seen[empty].status = snapshot();
        cached = &s_frp_status_seen[empty].status;
    }
    const int formatted = format_result_json(response, capacity,
        parse_error ? NULL : request.request_id,
        error ? state : "succeeded", error, cached && !error ? cached : NULL);
    if (formatted < 0) return 500;
    *response_length = (size_t)formatted;
    return error ? http_status : 200;
}

static void reply(const char *request_id, const char *state, const char *error, const status_snapshot_t *status)
{
    /* USB, MQTT and FRP share one result serializer. Strings are validated
     * UUIDs or closed firmware constants; raw request bytes are never echoed. */
    const int length = format_result_json(s_mqtt_result_json, sizeof s_mqtt_result_json,
        request_id && request_id[0] ? request_id : NULL, state, error, status);
    if (length < 0) return;
    if (s_reply_mqtt) {
        (void)esp_base_mqtt_owner_result(s_mqtt_result_json, (size_t)length);
        return;
    }
    flockfile(stdout);
    fputc('\n', stdout);
    (void)fwrite(s_mqtt_result_json, 1, (size_t)length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void reply_ota_result(const char *request_id, const esp_base_ota_receipt_view_t *view)
{
    const char *state = view->state == ESP_BASE_OTA_OPERATION_RUNNING ? "running" :
        view->state == ESP_BASE_OTA_OPERATION_SUCCEEDED ? "succeeded" :
        view->state == ESP_BASE_OTA_OPERATION_FAILED ? "failed" : "unknown";
    static const char digits[] = "0123456789abcdef";
    char digest[65];
    for (size_t i = 0; i < 32; ++i) {
        digest[i * 2] = digits[view->sha256[i] >> 4];
        digest[i * 2 + 1] = digits[view->sha256[i] & 15];
    }
    digest[64] = '\0';
    if (s_reply_mqtt) {
        const int length = snprintf(s_mqtt_result_json, sizeof s_mqtt_result_json,
            "{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
            "\"request_id\":\"%s\",\"state\":\"%s\",\"error_code\":%s%s%s,"
            "\"result\":{\"operation_id\":\"%s\",\"sha256\":\"%s\","
            "\"image_size_bytes\":%" PRIu32 ",\"target\":\"%s\",\"target_slot\":\"%s\"}}",
            s_context.device_id, s_boot_id, request_id, state,
            view->error_code ? "\"" : "null", view->error_code ? view->error_code : "",
            view->error_code ? "\"" : "", view->operation_id, digest, view->image_size_bytes,
            ESP_BASE_OTA_TARGET,
            view->target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1");
        if (length > 0 && (size_t)length < sizeof s_mqtt_result_json)
            (void)esp_base_mqtt_owner_result(s_mqtt_result_json, (size_t)length);
        return;
    }
    flockfile(stdout);
    printf("\n{\"protocol_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
           "\"request_id\":\"%s\",\"state\":\"%s\",\"error_code\":",
           s_context.device_id, s_boot_id, request_id, state);
    if (view->error_code) printf("\"%s\"", view->error_code); else printf("null");
    printf(",\"result\":{\"operation_id\":\"%s\",\"sha256\":\"%s\","
           "\"image_size_bytes\":%" PRIu32 ",\"target\":\"%s\",\"target_slot\":\"%s\"}}\n",
           view->operation_id, digest, view->image_size_bytes, ESP_BASE_OTA_TARGET,
           view->target_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1");
    fflush(stdout);
    funlockfile(stdout);
}

static void emit_outcome(size_t slot, bool via_mqtt)
{
    command_outcome_t *out = &s_outcomes[slot];
    const bool previous_route = s_reply_mqtt;
    s_reply_mqtt = via_mqtt;
    reply(s_guard.requests[slot].request_id, out->state, out->error, out->has_status ? &out->status : NULL);
    s_reply_mqtt = previous_route;
}

static void save_outcome(size_t slot, const char *state, const char *error, bool status)
{
    const bool via_mqtt = s_outcomes[slot].via_mqtt;
    s_outcomes[slot] = (command_outcome_t){.state = state, .error = error,
                                          .has_status = status, .via_mqtt = via_mqtt};
    if (status) s_outcomes[slot].status = snapshot();
    emit_outcome(slot, via_mqtt);
}

static void restore_committed(uint64_t now)
{
    if (esp_base_wifi_apply(&s_context.config.wifi, now) != ESP_OK) s_config_uncertain = true;
}

static void poll_configuration(uint64_t now)
{
    if (!s_trial_active) return;
    const bool ready = s_candidate.wifi.configured ? esp_base_wifi_ready() :
        !strcmp(esp_base_wifi_state(), "unconfigured");
    if (ready && now < s_trial_deadline) {
        /* The control task is the sole reader/writer of s_context.config after
         * startup. Network owners copy their config before starting workers. */
        esp_err_t error = esp_base_remote_config_commit_verified(&s_candidate, s_candidate.revision,
                                                                  &s_context.config, &command.config);
        s_trial_active = false;
        memset(&s_candidate, 0, sizeof s_candidate);
        if (error == ESP_OK) {
            save_outcome(s_trial_slot, "succeeded", NULL, true);
        } else if (error == ESP_BASE_CONFIG_UNCERTAIN) {
            s_config_uncertain = true;
            /* A write error may follow a durable commit. Reload before selecting
             * connectivity, never claim the old configuration was restored. */
            if (esp_base_remote_config_load(&s_context.config) == ESP_OK) {
                restore_committed(now);
            } else {
                ebase_wifi_config_t disabled = {0};
                (void)esp_base_wifi_apply(&disabled, now);
            }
            save_outcome(s_trial_slot, "unknown", "storage_uncertain", false);
        } else {
            restore_committed(now);
            save_outcome(s_trial_slot, "failed", "storage_failure", false);
        }
    } else if (now >= s_trial_deadline || !strcmp(esp_base_wifi_state(), "failed")) {
        s_trial_active = false;
        memset(&s_candidate, 0, sizeof s_candidate);
        restore_committed(now);
        save_outcome(s_trial_slot, "failed", "connection_proof_failed", false);
    }
}

static void ota_progress(uint32_t received, uint32_t total, void *context)
{
    (void)total;
    (void)context;
    atomic_store_explicit(&s_ota_received, received, memory_order_relaxed);
}

static void ota_task(void *argument)
{
    (void)argument;
    if (!esp_base_container_product_ota_ready()) {
        atomic_store_explicit(&s_ota_result, EOTA_UPDATE_RESOURCE_FAILURE,
                              memory_order_relaxed);
        atomic_store_explicit(&s_ota_done, true, memory_order_release);
        vTaskDelete(NULL);
        return;
    }
    const eota_policy_t policy = esp_base_ota_policy(true);
    eota_image_t image = {
        .image_url = s_ota_request.image_url,
        .image_size_bytes = s_ota_request.image_size_bytes,
    };
    memcpy(image.sha256, s_ota_request.sha256, sizeof image.sha256);
    esp_base_ota_receipt_recovery_t receipt = {0};
    eota_result_t result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
    if (esp_base_ota_receipt_load_for_recovery(
            s_context.device_id, &receipt) != ESP_BASE_OTA_RECEIPT_OK ||
        receipt.status != ESP_BASE_OTA_RECEIPT_PREPARED ||
        strcmp(receipt.operation_id, s_ota_request.operation_id) != 0 ||
        receipt.image_size_bytes != image.image_size_bytes ||
        memcmp(receipt.candidate_sha256, image.sha256, sizeof image.sha256) != 0) {
        /* A missing or changed durable intent cannot authorize app erasure. */
        atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
    } else {
        /* The original receipt, physical A/B and ECS2 sequence are all known
         * before the first target write. Retire B physically, then retire its
         * persistent binding, and only then let IDF download C into the slot. */
        result = eota_retire_inactive(&policy, receipt.target_subtype,
                                      receipt.source_sha256);
        if (result == EOTA_UPDATE_OK &&
            esp_base_container_product_retire_inactive(
                &s_ota_storage_claim, receipt.container_enabled,
                receipt.container_sequence, receipt.source_sha256,
                receipt.inactive_sha256) != ESP_BASE_CONTAINER_RETIRE_COMPLETE) {
            result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        }
        if (result != EOTA_UPDATE_OK) {
            atomic_store_explicit(&s_ota_stage_uncertain, true,
                                  memory_order_relaxed);
        }
    }
    if (result == EOTA_UPDATE_OK) {
        eota_prepared_t prepared = {0};
        result = eota_prepare(&policy, &image, ota_progress, NULL, &prepared);
        if (result != EOTA_UPDATE_OK) {
            /* The target can contain a partially written C with a bootable
             * header. Keep the owner until the same receipt is reconciled. */
            atomic_store_explicit(&s_ota_stage_uncertain, true,
                                  memory_order_relaxed);
        }
        if (result != EOTA_UPDATE_OK) goto done;
        const esp_base_container_stage_result_t stage =
            esp_base_container_product_stage_firmware(
                &s_ota_storage_claim, &prepared, s_ota_request.operation_id);
        if ((receipt.container_enabled &&
             stage == ESP_BASE_CONTAINER_STAGE_PREPARED) ||
            (!receipt.container_enabled &&
             stage == ESP_BASE_CONTAINER_STAGE_NOT_CONFIGURED)) {
            result = eota_select(&policy, &prepared);
            if (result != EOTA_UPDATE_OK) {
                atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
            }
        } else {
            atomic_store_explicit(&s_ota_stage_uncertain, true, memory_order_relaxed);
            result = EOTA_UPDATE_BOOT_STATE_UNKNOWN;
        }
    }
done:
    atomic_store_explicit(&s_ota_result, result, memory_order_relaxed);
    atomic_store_explicit(&s_ota_done, true, memory_order_release);
    vTaskDelete(NULL);
}

static void poll_ota(void)
{
    if (!s_ota_active || !atomic_load_explicit(&s_ota_done, memory_order_acquire)) return;
    const eota_result_t result =
        (eota_result_t)atomic_load_explicit(&s_ota_result, memory_order_relaxed);
    if (atomic_load_explicit(&s_ota_stage_uncertain, memory_order_relaxed)) {
        s_config_uncertain = true;
        s_ota_boot_uncertain = true;
        esp_base_control_state_set_ota_download_active(&s_control_state, false);
        s_ota_active = false;
        atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
        save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
        memset(&s_ota_request, 0, sizeof s_ota_request);
        return;
    }
    if (result == EOTA_UPDATE_OK) {
        /* The slot is selected but not yet confirmed. A new boot must pass the
         * local self-test and stability window before it becomes valid. */
        save_outcome(s_ota_slot, "running", NULL, false);
        (void)fsync(STDOUT_FILENO);
        esp_restart();
        return;
    }
    if (result == EOTA_UPDATE_BOOT_STATE_UNKNOWN) {
        s_ota_boot_uncertain = true;
        ESP_LOGE("base_ota", "ESP_BASE_OTA_RECOVERY_REQUIRED selector readback unavailable; avoid resetting device");
    } else {
        const esp_base_ota_receipt_result_t saved = esp_base_ota_receipt_record_failure(
            s_context.device_id, s_ota_request.operation_id, result);
        if (saved != ESP_BASE_OTA_RECEIPT_OK) {
            s_config_uncertain = true;
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
            save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
            memset(&s_ota_request, 0, sizeof s_ota_request);
            return;
        }
        if (!esp_base_storage_release(&s_ota_storage_claim)) {
            s_ota_boot_uncertain = true;
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            atomic_store_explicit(&s_ota_done, false, memory_order_relaxed);
            save_outcome(s_ota_slot, "unknown", "storage_uncertain", false);
            memset(&s_ota_request, 0, sizeof s_ota_request);
            return;
        }
    }
    esp_base_control_state_set_ota_download_active(&s_control_state, false);
    s_ota_active = false;
    atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
    save_outcome(s_ota_slot, result == EOTA_UPDATE_BOOT_STATE_UNKNOWN ? "unknown" : "failed",
                 eota_error(result), false);
    memset(&s_ota_request, 0, sizeof s_ota_request);
}

static void handle_line(const char *line, size_t length, void *context)
{
    (void)context;
    const char *error = ebase_parse_command(line, length, &command);
    if (error) { reply(command.request.request_id, "failed", error, NULL); return; }
    if (command.kind == EBASE_STATUS) {
        status_snapshot_t current = snapshot();
        reply(command.request.request_id, "succeeded", NULL, &current);
        return;
    }
    if (command.kind == EBASE_OTA_RESULT) {
        esp_base_ota_receipt_view_t view;
        const bool active = s_ota_active && !strcmp(s_ota_request.operation_id, command.operation_id);
        const esp_base_ota_receipt_result_t result = esp_base_ota_receipt_query(
            s_context.device_id, command.operation_id, active, &view);
        if (result == ESP_BASE_OTA_RECEIPT_OK) reply_ota_result(command.request.request_id, &view);
        else reply(command.request.request_id, result == ESP_BASE_OTA_RECEIPT_UNSUPPORTED ? "failed" : "unknown",
                   result == ESP_BASE_OTA_RECEIPT_UNSUPPORTED ? "ota_signing_unavailable" :
                   result == ESP_BASE_OTA_RECEIPT_NOT_FOUND ? "ota_operation_not_found" : "storage_uncertain", NULL);
        return;
    }
    if (command.kind == EBASE_CONFIG_SET) {
        if (!esp_base_remote_config_with_canonical_bytes(&command.config,
                fingerprint_config_bytes, command.request.fingerprint)) {
            reply(command.request.request_id, "failed", "resource_failure", NULL); return;
        }
    }
    if (command.kind == EBASE_OTA_START) {
        uint8_t bytes[10 + ESP_BASE_OTA_OPERATION_ID_BYTES + EOTA_URL_BYTES + 1 + 32 + 4];
        size_t offset = 0;
        memcpy(bytes + offset, "ota.start", 9); offset += 9;
        memcpy(bytes + offset, command.ota.operation_id, ESP_BASE_OTA_OPERATION_ID_BYTES); offset += ESP_BASE_OTA_OPERATION_ID_BYTES;
        const size_t url_bytes = strlen(command.ota.image_url) + 1;
        memcpy(bytes + offset, command.ota.image_url, url_bytes); offset += url_bytes;
        memcpy(bytes + offset, command.ota.sha256, 32); offset += 32;
        for (int i = 3; i >= 0; --i) bytes[offset++] = (uint8_t)(command.ota.image_size_bytes >> (8 * i));
        size_t size = 0;
        if (psa_hash_compute(PSA_ALG_SHA_256, bytes, offset, command.request.fingerprint,
                sizeof command.request.fingerprint, &size) != PSA_SUCCESS || size != 32) {
            reply(command.request.request_id, "failed", "resource_failure", NULL); return;
        }
    }
    size_t slot = 0;
    const ebase_admission_t decision = ebase_admit(&s_guard, &command.request,
        s_context.device_id, s_boot_id, uptime_ms(), &slot);
    if (decision == EBASE_REPLAY) { emit_outcome(slot, s_reply_mqtt); return; }
    if (decision != EBASE_ACCEPT) {
        static const char *const errors[] = {NULL, NULL, "invalid_identity", "wrong_device",
            "wrong_boot", "expired", "invalid_deadline", "request_conflict", "capacity_exceeded"};
        reply(command.request.request_id, decision == EBASE_EXPIRED ? "expired" : "failed", errors[decision], NULL);
        return;
    }
    s_outcomes[slot].via_mqtt = s_reply_mqtt;
    if (s_reply_mqtt && command.kind == EBASE_CONFIG_SET) {
        save_outcome(slot, "failed", "physical_usb_required", false);
        return;
    }
    if (command.kind == EBASE_CONFIG_SET) {
        const char *ota_error = esp_base_control_state_config_write_error(&s_control_state);
        if (ota_error != NULL) { save_outcome(slot, "failed", ota_error, false); return; }
    }
    if (command.kind == EBASE_OTA_START) {
        if (!eota_available()) { save_outcome(slot, "failed", "ota_signing_unavailable", false); return; }
        eota_image_t candidate = {
            .image_url = command.ota.image_url,
            .image_size_bytes = command.ota.image_size_bytes,
        };
        memcpy(candidate.sha256, command.ota.sha256, sizeof candidate.sha256);
        if (eota_validate_image_request(&candidate) != EOTA_UPDATE_OK) {
            save_outcome(slot, "failed", "invalid_request", false); return;
        }
        if (esp_base_control_state_ota_pending(&s_control_state)) { save_outcome(slot, "failed", "ota_verification_pending", false); return; }
        if (s_ota_active) { save_outcome(slot, "failed", "ota_in_progress", false); return; }
        if (s_trial_active) { save_outcome(slot, "failed", "configuration_busy", false); return; }
        if (s_config_uncertain) { save_outcome(slot, "failed", "storage_uncertain", false); return; }
        if (s_ota_boot_uncertain) { save_outcome(slot, "failed", "ota_boot_state_unknown", false); return; }
        if (!esp_base_container_product_ota_ready()) {
            save_outcome(slot, "failed", "product_ota_unavailable", false); return;
        }
        if (!esp_base_wifi_ready()) { save_outcome(slot, "failed", "network_unavailable", false); return; }
        if (!esp_base_time_ready()) { save_outcome(slot, "failed", "time_unavailable", false); return; }
        if (!esp_base_storage_claim(s_context.storage_owner, &s_ota_storage_claim)) {
            save_outcome(slot, "failed", "operation_busy", false); return;
        }
        esp_base_ota_receipt_snapshot_t snapshot = {0};
        if (!esp_base_container_product_snapshot_for_ota(
                &s_ota_storage_claim, &snapshot)) {
            const bool released = esp_base_storage_release(&s_ota_storage_claim);
            if (!released) s_config_uncertain = true;
            save_outcome(slot, released ? "failed" : "unknown",
                         released ? "product_ota_unavailable" : "storage_uncertain", false);
            return;
        }
        const esp_base_ota_receipt_result_t receipt = esp_base_ota_receipt_register(
            s_context.device_id, &command.ota, &snapshot);
        if (receipt != ESP_BASE_OTA_RECEIPT_OK) {
            const char *receipt_error = receipt == ESP_BASE_OTA_RECEIPT_EXISTS ? "ota_operation_exists" :
                receipt == ESP_BASE_OTA_RECEIPT_CONFLICT ? "ota_operation_conflict" :
                receipt == ESP_BASE_OTA_RECEIPT_BUSY ? "ota_previous_unresolved" :
                receipt == ESP_BASE_OTA_RECEIPT_SLOT_UNAVAILABLE ? "ota_slot_unavailable" :
                receipt == ESP_BASE_OTA_RECEIPT_SELECTOR_MISMATCH ? "ota_selector_mismatch" :
                receipt == ESP_BASE_OTA_RECEIPT_SOURCE_NOT_VALID ? "ota_source_not_valid" :
                receipt == ESP_BASE_OTA_RECEIPT_TARGET_NOT_SAFE ? "ota_target_not_safe" :
                receipt == ESP_BASE_OTA_RECEIPT_TARGET_STATE_UNKNOWN ? "ota_target_state_unknown" :
                receipt == ESP_BASE_OTA_RECEIPT_SNAPSHOT_MISMATCH ? "ota_snapshot_mismatch" :
                receipt == ESP_BASE_OTA_RECEIPT_STORAGE_FAILURE ? "storage_failure" : "storage_uncertain";
            bool uncertain = receipt == ESP_BASE_OTA_RECEIPT_STORAGE_UNCERTAIN;
            if (!uncertain && !esp_base_storage_release(&s_ota_storage_claim)) uncertain = true;
            if (uncertain) s_config_uncertain = true;
            save_outcome(slot, uncertain ? "unknown" : "failed",
                         uncertain ? "storage_uncertain" : receipt_error, false);
            return;
        }
        s_ota_request = command.ota;
        s_ota_slot = slot;
        s_ota_active = true;
        atomic_store_explicit(&s_ota_done, false, memory_order_relaxed);
        atomic_store_explicit(&s_ota_stage_uncertain, false, memory_order_relaxed);
        atomic_store_explicit(&s_ota_received, 0, memory_order_relaxed);
        esp_base_control_state_set_ota_download_active(&s_control_state, true);
        if (xTaskCreate(ota_task, "base_ota", 12288, NULL, 4, NULL) != pdPASS) {
            esp_base_control_state_set_ota_download_active(&s_control_state, false);
            s_ota_active = false;
            memset(&s_ota_request, 0, sizeof s_ota_request);
            if (esp_base_ota_receipt_record_failure(s_context.device_id, command.ota.operation_id,
                    EOTA_UPDATE_RESOURCE_FAILURE) == ESP_BASE_OTA_RECEIPT_OK &&
                esp_base_storage_release(&s_ota_storage_claim)) {
                save_outcome(slot, "failed", "resource_failure", false);
            } else {
                s_config_uncertain = true;
                save_outcome(slot, "unknown", "storage_uncertain", false);
            }
            return;
        }
        save_outcome(slot, "running", NULL, false);
        return;
    }
    if (s_ota_active) { save_outcome(slot, "failed", "ota_in_progress", false); return; }
    if (s_ota_boot_uncertain) { save_outcome(slot, "failed", "ota_boot_state_unknown", false); return; }
    if (s_trial_active) { save_outcome(slot, "failed", "configuration_busy", false); return; }
    if (s_config_uncertain) { save_outcome(slot, "failed", "storage_uncertain", false); return; }
    if (command.kind == EBASE_CONFIG_SET) {
        if (command.config.revision != s_context.config.revision) {
            save_outcome(slot, "failed", "revision_conflict", false); return;
        }
        if (command.config.revision == UINT32_MAX) { save_outcome(slot, "failed", "revision_exhausted", false); return; }
        s_candidate = command.config;
        s_trial_slot = slot;
        s_trial_deadline = uptime_ms() + 20000;
        save_outcome(slot, "running", NULL, false);
        if (esp_base_wifi_apply(&s_candidate.wifi, uptime_ms()) != ESP_OK) {
            memset(&s_candidate, 0, sizeof s_candidate);
            restore_committed(uptime_ms());
            save_outcome(slot, "failed", "connection_proof_failed", false);
        } else s_trial_active = true;
        return;
    }
    save_outcome(slot, "running", NULL, false);
    (void)fsync(STDOUT_FILENO);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void handle_mqtt_command(const uint8_t *json, size_t length, void *context)
{
    (void)context;
    s_reply_mqtt = true;
    handle_line((const char *)json, length, NULL);
    s_reply_mqtt = false;
}

static void control_task(void *argument)
{
    (void)argument;
    uint64_t next_report = 0, next_time_poll = 0, last_input = 0;
    unsigned char bytes[256];
    while (true) {
        const uint64_t now = uptime_ms();
        esp_base_wifi_poll(now);
        poll_configuration(now);
        poll_ota();
        if (now >= next_time_poll) {
            esp_base_time_poll();
            next_time_poll = now + 1000;
        }
        if (!esp_base_control_state_ota_pending(&s_control_state)) {
            if (!s_mqtt_revision_set || s_mqtt_revision != s_context.config.revision) {
                (void)esp_base_mqtt_owner_configure(&s_context.config.mqtt, s_context.device_id, s_boot_id);
                s_mqtt_revision = s_context.config.revision;
                s_mqtt_revision_set = true;
            }
            esp_base_mqtt_owner_poll(now, esp_base_wifi_ready(), esp_base_time_ready(),
                                     handle_mqtt_command, NULL);
            if (!s_frp_revision_set || s_frp_revision != s_context.config.revision) {
                /* Revoke the old endpoint before waiting for the old FRP worker
                 * to finish; no stale management key remains reachable. */
                esp_base_frp_status_listener_configure(NULL);
                if (esp_base_frp_owner_configure(&s_context.config.frp, s_context.device_id) == ESP_OK) {
                    esp_base_frp_status_listener_configure(&s_context.config.frp);
                    s_frp_revision = s_context.config.revision;
                    s_frp_revision_set = true;
                }
            }
            esp_base_frp_status_listener_poll(now, handle_frp_status, NULL);
            esp_base_frp_owner_poll(now, esp_base_wifi_ready(), esp_base_time_ready(),
                                    esp_base_frp_status_listener_ready());
        }
        if (now >= next_report) { reported(); next_report = now + 5000; }
        if (s_reader.length && now - last_input >= 2000) {
            s_reader.length = 0;
            s_reader.discard = true; /* Never interpret a timed-out tail as a command. */
        }
        size_t count = 0;
        // Read from the selected console VFS without blocking the control loop.
        // The C3 USB FIFO backpressures the host; UART0 has no such guarantee
        // and needs its own physical overload check before device acceptance.
        while (count < sizeof bytes && read(STDIN_FILENO, bytes + count, 1) == 1) ++count;
        if (count > 0) {
            last_input = uptime_ms();
            ebase_line_feed(&s_reader, bytes, count, handle_line, NULL);
        }
        esp_base_control_state_note_progress(&s_control_state);
        vTaskDelay(1); /* Let idle/WDT and other capabilities run under sustained input. */
    }
}

esp_err_t esp_base_protocol_load_config(uint32_t *revision)
{
    if (!revision) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_config_loaded = false;
    const esp_err_t error = esp_base_remote_config_load(&s_context.config);
    if (error != ESP_OK) return error;
    *revision = s_context.config.revision;
    s_config_loaded = true;
    return ESP_OK;
}

esp_err_t esp_base_protocol_start(const esp_base_protocol_context_t *context)
{
    if (!context || !ebase_is_uuid(context->device_id) || context->storage_owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started || !s_config_loaded) return ESP_ERR_INVALID_STATE;
    esp_err_t error = esp_base_identity_generate_uuid(s_boot_id, sizeof s_boot_id);
    if (error != ESP_OK) return error;
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    usb_serial_jtag_vfs_use_nonblocking();
    // IDF 6.1 USB no-driver VFS prefetches with O_NONBLOCK clear.
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) < 0) return ESP_FAIL;
#else
    uart_vfs_dev_use_nonblocking(CONFIG_ESP_CONSOLE_UART_NUM);
    // UART0 has no USB packet backpressure; keep reads nonblocking so the
    // control loop continues OTA progress and network owner polling.
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return ESP_FAIL;
#endif
    /* Config was loaded into s_context before starting this task. Copy only
     * immutable metadata; replacing the whole context would erase it. */
    s_context.device_id = context->device_id;
    s_context.firmware_version = context->firmware_version;
    s_context.chip_model = context->chip_model;
    s_context.flash_size_bytes = context->flash_size_bytes;
    s_context.reset_reason = context->reset_reason;
    s_context.storage_owner = context->storage_owner;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    error = esp_base_wifi_start(&s_context.config.wifi);
    if (error != ESP_OK) {
        ESP_LOGW("base_wifi", "ESP_BASE_WIFI_UNAVAILABLE error=%s", esp_err_to_name(error));
    }
    if (xTaskCreate(control_task, "base_control", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool esp_base_protocol_control_healthy(void)
{
    return s_started && esp_base_control_state_is_recent(&s_control_state);
}

uint32_t esp_base_protocol_control_progress_count(void)
{
    return esp_base_control_state_progress_count(&s_control_state);
}

void esp_base_protocol_set_ota_verification_pending(bool pending)
{
    esp_base_control_state_set_ota_pending(&s_control_state, pending);
}
