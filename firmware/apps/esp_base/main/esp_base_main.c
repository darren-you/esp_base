#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_base_identity.h"
#include "esp_base_container_product.h"
#include "eota.h"
#include "esp_base_ota_policy.h"
#include "esp_base_ota_receipt.h"
#include "esp_base_protocol.h"
#include "esp_base_remote_config.h"
#include "esp_base_storage_owner.h"
#include "esp_base_safety.h"
#include "esp_base_time.h"

static const char *TAG = "esp_base";
static esp_base_storage_owner_t s_storage_owner;
static esp_base_storage_claim_t s_boot_storage_claim;

#define ESP_BASE_CONTROL_START_TIMEOUT_MS UINT64_C(5000)
#define ESP_BASE_OTA_STABLE_WINDOW_MS UINT64_C(30000)

static esp_err_t initialise_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    return result;
}

static uint64_t uptime_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static void stop_after_local_failure(eota_current_t *ota, bool pending_boot,
                                     const char *check, esp_err_t failure)
{
    ESP_LOGE(TAG, "ESP_BASE_LOCAL_CHECK_FAILED check=%s error=%s", check, esp_err_to_name(failure));
    if (!pending_boot) {
        return;
    }
    if (!esp_base_container_product_stop_trial(&s_boot_storage_claim)) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED candidate guest still active");
        return;
    }
    if (ota->state != EOTA_STATE_PENDING_VERIFY) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED slot=%s state=%s rollback=not_safe",
                 ota->running_partition ? ota->running_partition : "unknown", eota_state_name(ota->state));
        return;
    }
    ESP_LOGE(TAG, "Pending OTA slot %s failed %s; requesting IDF rollback", ota->running_partition, check);
    const esp_err_t rollback_status = eota_reject_pending(ota);
    /* ESP_OK never returns from the IDF rollback API. If it does return,
     * preserve this boot rather than force a reset without a viable slot. */
    ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED slot=%s state=%s rollback_error=%s",
             ota->running_partition ? ota->running_partition : "unknown",
             eota_state_name(ota->state), esp_err_to_name(rollback_status));
}

static esp_err_t wait_for_control_start(void)
{
    const uint64_t started_ms = uptime_ms();
    while (!esp_base_protocol_control_healthy()) {
        const uint64_t now_ms = uptime_ms();
        if (now_ms < started_ms || now_ms - started_ms >= ESP_BASE_CONTROL_START_TIMEOUT_MS) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

/* The durable receipt is the only authority for a target-slot cleanup after
 * reset. No Container guest has been started in this boot and the startup
 * claim still serializes all app, otadata and package state changes. */
static bool reconcile_interrupted_ota(const char *device_id, const char *boot_id,
                                     bool *needs_success_receipt)
{
    *needs_success_receipt = false;
    if (!eota_available()) return true;
    esp_base_ota_receipt_recovery_t receipt = {0};
    const esp_base_ota_receipt_result_t loaded =
        esp_base_ota_receipt_load_for_recovery(device_id, &receipt);
    if (loaded == ESP_BASE_OTA_RECEIPT_NOT_FOUND) return true;
    if (loaded != ESP_BASE_OTA_RECEIPT_OK) return false;
    if (receipt.status == ESP_BASE_OTA_RECEIPT_FAILED) return true;
    if ((receipt.status != ESP_BASE_OTA_RECEIPT_PREPARED &&
         receipt.status != ESP_BASE_OTA_RECEIPT_SUCCEEDED) ||
        receipt.container_enabled != esp_base_container_product_configured()) {
        return false;
    }

    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots = {0};
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK ||
        slots.running_subtype != slots.boot_subtype ||
        slots.running_address_bytes != slots.boot_address_bytes) {
        return false;
    }
    if (slots.running_subtype == receipt.target_subtype) {
        /* A selected C belongs to pending trial or confirmed recovery. Never
         * erase it as though it were an interrupted inactive download. */
        uint8_t digest[EOTA_SHA256_BYTES] = {0};
        if ((slots.running_state != EOTA_STATE_PENDING_VERIFY &&
             slots.running_state != EOTA_STATE_VALID) ||
            (receipt.status == ESP_BASE_OTA_RECEIPT_SUCCEEDED &&
             slots.running_state != EOTA_STATE_VALID) ||
            eota_sha256_running(&policy, receipt.image_size_bytes, digest) !=
                EOTA_UPDATE_OK ||
            memcmp(digest, receipt.candidate_sha256, sizeof digest) != 0 ||
            !esp_base_container_product_verify_selected_ota(
                &s_boot_storage_claim, &receipt, slots.running_state)) {
            return false;
        }
        *needs_success_receipt =
            receipt.status == ESP_BASE_OTA_RECEIPT_PREPARED;
        return true;
    }
    if (receipt.status != ESP_BASE_OTA_RECEIPT_PREPARED ||
        slots.running_subtype != receipt.source_subtype ||
        slots.running_state != EOTA_STATE_VALID ||
        slots.target_subtype != receipt.target_subtype ||
        eota_retire_inactive(&policy, receipt.target_subtype,
                             receipt.source_sha256) != EOTA_UPDATE_OK) {
        return false;
    }
    if (esp_base_container_product_recover_retired_firmware(
            &s_boot_storage_claim, receipt.container_enabled,
            receipt.container_sequence, receipt.source_sha256,
            receipt.inactive_sha256, receipt.candidate_sha256,
            receipt.operation_id, boot_id) != ESP_BASE_CONTAINER_RETIRE_COMPLETE) {
        return false;
    }
    return esp_base_ota_receipt_record_failure(
        device_id, receipt.operation_id,
        EOTA_UPDATE_RESOURCE_FAILURE) == ESP_BASE_OTA_RECEIPT_OK;
}

void app_main(void)
{
    /* Hold the same owner as OTA and the optional Container adapter through
     * startup's storage operations. The guest's lifetime is not a claim. */
    esp_base_storage_owner_init(&s_storage_owner);
    s_boot_storage_claim = (esp_base_storage_claim_t){0};
    if (!esp_base_storage_claim(&s_storage_owner, &s_boot_storage_claim)) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED storage owner unavailable");
        return;
    }
    eota_current_t ota = {0};
    const esp_err_t ota_status = eota_inspect(&ota);
    if (ota_status != ESP_OK) {
        ESP_LOGE(TAG, "OTA slot state unavailable (%s); initialization stopped", esp_err_to_name(ota_status));
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED slot=%s state=%s rollback=not_safe",
                 ota.running_partition ? ota.running_partition : "unknown", eota_state_name(ota.state));
        return;
    }
    const bool pending_boot = ota.state == EOTA_STATE_PENDING_VERIFY;
    /* The control task may start before durable receipt recovery finishes.
     * Keep configuration writes and network owners gated until reconciliation. */
    esp_base_protocol_set_ota_verification_pending(true);

    const esp_err_t storage_status = initialise_nvs();
    if (storage_status != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); storage preserved, initialization stopped", esp_err_to_name(storage_status));
        stop_after_local_failure(&ota, pending_boot, "nvs", storage_status);
        return;
    }

    static esp_base_identity_t identity = {0};
    const esp_err_t identity_status = esp_base_identity_read(&identity);
    if (identity_status != ESP_OK) {
        ESP_LOGE(TAG, "Identity unavailable (%s); initialization stopped", esp_err_to_name(identity_status));
        stop_after_local_failure(&ota, pending_boot, "identity", identity_status);
        return;
    }

    esp_base_safety_t safety = {0};
    const esp_err_t safety_status = esp_base_safety_start(&safety);
    if (safety_status != ESP_OK) {
        stop_after_local_failure(&ota, pending_boot, "safety", safety_status);
        return;
    }

    uint32_t config_revision = 0;
    const esp_err_t config_status = esp_base_protocol_load_config(&config_revision);
    if (config_status != ESP_OK) {
        ESP_LOGE(TAG, "Configuration unavailable (%s); storage preserved, initialization stopped", esp_err_to_name(config_status));
        stop_after_local_failure(&ota, pending_boot, "config", config_status);
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG,
             "ESP_BASE_BOOT project=ESP Base device_id=%s version=%s idf=%s chip=%s revision=%d.%d flash=%" PRIu32
             " slot=%s reset=%s config_generation=%" PRIu32,
             identity.device_id,
             app->version,
             esp_get_idf_version(),
             identity.model,
             identity.revision / 100,
             identity.revision % 100,
             identity.flash_size_bytes,
             ota.running_partition,
             safety.reset_reason,
             config_revision);

    const esp_base_protocol_context_t protocol = {
        .device_id = identity.device_id,
        .firmware_version = app->version,
        .chip_model = identity.model,
        .flash_size_bytes = identity.flash_size_bytes,
        .reset_reason = safety.reset_reason,
        .storage_owner = &s_storage_owner,
    };
    const esp_err_t protocol_status = esp_base_protocol_start(&protocol);
    if (protocol_status != ESP_OK) {
        ESP_LOGE(TAG, "Control unavailable (%s); initialization stopped", esp_err_to_name(protocol_status));
        stop_after_local_failure(&ota, pending_boot, "control_start", protocol_status);
        return;
    }

    bool needs_success_receipt = false;
    if (!reconcile_interrupted_ota(identity.device_id,
                                   esp_base_protocol_boot_id(),
                                   &needs_success_receipt)) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED original receipt or slot recovery uncertain");
        stop_after_local_failure(&ota, pending_boot, "ota_recovery",
                                 ESP_ERR_INVALID_STATE);
        return;
    }
    /* Time is needed by future strict TLS consumers, but it is not part of
     * the pending slot's local self-test and must not block USB control. */
    const esp_err_t time_status = esp_base_time_start(CONFIG_ESP_BASE_TIME_SERVER);
    if (time_status != ESP_OK) {
        ESP_LOGW(TAG, "ESP_BASE_TIME_UNAVAILABLE error=%s", esp_err_to_name(time_status));
    }

    const bool container_configured = esp_base_container_product_configured();
    esp_base_container_boot_result_t product = ESP_BASE_CONTAINER_NOT_CONFIGURED;
    if (pending_boot && container_configured) {
        product = esp_base_container_product_start_trial(
            &s_boot_storage_claim, esp_base_protocol_boot_id());
        if (product == ESP_BASE_CONTAINER_BLOCKED) {
            stop_after_local_failure(&ota, true, "container_trial", ESP_ERR_INVALID_STATE);
            return;
        }
    }

    if (pending_boot) {
        /* Startup checks above are local: no Broker, FRPS or Wi-Fi connection is
         * required. The control task must also make progress throughout this boot. */
        const esp_err_t control_status = wait_for_control_start();
        if (control_status != ESP_OK) {
            stop_after_local_failure(&ota, pending_boot, "control_ready", control_status);
            return;
        }
        ESP_LOGI(TAG, "pending OTA slot %s: waiting %" PRIu64 " ms of local stability",
                 ota.running_partition, ESP_BASE_OTA_STABLE_WINDOW_MS);
        const uint64_t stable_started_ms = uptime_ms();
        uint64_t now_ms;
        for (;;) {
            if (!esp_base_protocol_control_healthy()) {
                stop_after_local_failure(&ota, pending_boot, "control_progress", ESP_ERR_TIMEOUT);
                return;
            }
            now_ms = uptime_ms();
            if (now_ms < stable_started_ms) {
                stop_after_local_failure(&ota, pending_boot, "stable_clock", ESP_ERR_INVALID_STATE);
                return;
            }
            if (now_ms - stable_started_ms >= ESP_BASE_OTA_STABLE_WINDOW_MS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        /* A heartbeat from before the 30-second boundary is insufficient:
         * require one complete control pass afterwards before confirming. */
        const uint32_t boundary_progress = esp_base_protocol_control_progress_count();
        const uint64_t boundary_ms = now_ms;
        while (esp_base_protocol_control_progress_count() == boundary_progress) {
            if (!esp_base_protocol_control_healthy()) {
                stop_after_local_failure(&ota, pending_boot, "control_progress", ESP_ERR_TIMEOUT);
                return;
            }
            now_ms = uptime_ms();
            if (now_ms < boundary_ms || now_ms - boundary_ms >= ESP_BASE_CONTROL_START_TIMEOUT_MS) {
                stop_after_local_failure(&ota, pending_boot, "control_boundary", ESP_ERR_TIMEOUT);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!esp_base_protocol_control_healthy()) {
            stop_after_local_failure(&ota, pending_boot, "control_boundary", ESP_ERR_TIMEOUT);
            return;
        }
        if (container_configured &&
            !esp_base_container_product_mark_healthy(&s_boot_storage_claim)) {
            stop_after_local_failure(&ota, true, "container_health", ESP_ERR_INVALID_STATE);
            return;
        }
        now_ms = uptime_ms();
        const esp_err_t confirm_status = now_ms >= stable_started_ms &&
            now_ms - stable_started_ms >= ESP_BASE_OTA_STABLE_WINDOW_MS ?
            eota_confirm_pending(&ota) : ESP_ERR_NOT_FINISHED;
        if (confirm_status != ESP_OK) {
            stop_after_local_failure(&ota, pending_boot, "confirm", confirm_status);
            return;
        }
        if (container_configured) {
            eota_current_t confirmed = {0};
            if (eota_inspect(&confirmed) != ESP_OK || confirmed.state != EOTA_STATE_VALID ||
                confirmed.running_partition == NULL || ota.running_partition == NULL ||
                strcmp(confirmed.running_partition, ota.running_partition) != 0 ||
                !esp_base_container_product_confirm_firmware(&s_boot_storage_claim)) {
                ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED VALID/container confirmation readback incomplete");
                (void)esp_base_container_product_stop_trial(&s_boot_storage_claim);
                return;
            }
        }
    }
    if (!pending_boot) product = esp_base_container_product_boot(
        &s_boot_storage_claim, esp_base_protocol_boot_id());
    if (product == ESP_BASE_CONTAINER_BLOCKED) {
        ESP_LOGE(TAG, "ESP_BASE_CONTAINER_BLOCKED startup claim retained");
        return;
    }
    if (needs_success_receipt &&
        esp_base_ota_receipt_record_success(identity.device_id) !=
            ESP_BASE_OTA_RECEIPT_OK) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED success receipt not durable");
        return;
    }
    if (!esp_base_storage_release(&s_boot_storage_claim)) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED storage owner release failed");
        return;
    }
    esp_base_protocol_set_ota_verification_pending(false);
    ESP_LOGI(TAG, "ESP_BASE_READY hardware_outputs=untouched provisioning=required container=%s",
             product == ESP_BASE_CONTAINER_RUNNING ? "running" :
             product == ESP_BASE_CONTAINER_EMPTY ? "empty" : "not_configured");
}
