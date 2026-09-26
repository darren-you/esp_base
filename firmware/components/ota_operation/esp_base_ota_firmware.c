// SPDX-License-Identifier: Apache-2.0
#include "esp_base_ota_firmware.h"

#include <stdbool.h>
#include <string.h>

#include "esp_base_ota_policy.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static bool same_geometry(const eota_slots_t *first, const eota_slots_t *second)
{
    return first->running_subtype == second->running_subtype &&
           first->boot_subtype == second->boot_subtype &&
           first->target_subtype == second->target_subtype &&
           first->running_address_bytes == second->running_address_bytes &&
           first->boot_address_bytes == second->boot_address_bytes &&
           first->target_address_bytes == second->target_address_bytes &&
           first->running_size_bytes == second->running_size_bytes &&
           first->boot_size_bytes == second->boot_size_bytes &&
           first->target_size_bytes == second->target_size_bytes;
}

static bool same_slots(const eota_slots_t *first, const eota_slots_t *second)
{
    return same_geometry(first, second) &&
           first->running_state == second->running_state &&
           first->target_state == second->target_state;
}

static bool zero_digest(const uint8_t digest[EOTA_SHA256_BYTES])
{
    uint8_t any = 0;
    for (size_t i = 0; i < EOTA_SHA256_BYTES; ++i) any |= digest[i];
    return any == 0;
}

static bool prepared_candidate_valid(const eota_prepared_t *prepared,
                                     const eota_slots_t *current)
{
    if (prepared == NULL || prepared->image_size_bytes == 0 ||
        prepared->image_size_bytes > current->target_size_bytes ||
        zero_digest(prepared->sha256) ||
        !same_geometry(&prepared->slots, current) ||
        prepared->slots.running_subtype != prepared->slots.boot_subtype ||
        prepared->slots.running_state != EOTA_STATE_VALID ||
        (prepared->slots.target_state != EOTA_STATE_UNTRACKED &&
         prepared->slots.target_state != EOTA_STATE_UNDEFINED &&
         prepared->slots.target_state != EOTA_STATE_VALID &&
         prepared->slots.target_state != EOTA_STATE_INVALID &&
         prepared->slots.target_state != EOTA_STATE_ABORTED)) return false;
    return current->target_state == EOTA_STATE_UNTRACKED ||
           current->target_state == EOTA_STATE_INVALID ||
           current->target_state == EOTA_STATE_ABORTED;
}

/* The OTA library deliberately reports signed image identity without product
 * authorization. A rollback image offered to Container must also be a Base
 * image for this chip, or a valid signature from another product could enter
 * the bootable firmware set. The caller holds the app/otadata write owner. */
static bool matching_base_image(const eota_policy_t *policy, uint8_t subtype,
                                uint32_t address, uint32_t size)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, subtype, NULL);
    if (!partition || partition->type != ESP_PARTITION_TYPE_APP ||
        partition->subtype != subtype || partition->address != address ||
        partition->size != size) return false;

    esp_image_header_t header = {0};
    esp_app_desc_t description = {0};
    return esp_partition_read(partition, 0, &header, sizeof header) == ESP_OK &&
           header.magic == ESP_IMAGE_HEADER_MAGIC &&
           header.chip_id == policy->chip_id &&
           esp_ota_get_partition_description(partition, &description) == ESP_OK &&
           description.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strncmp(description.project_name, policy->project_name,
                   sizeof description.project_name) == 0;
}

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation,
    const eota_prepared_t *prepared,
    esp_base_ota_firmware_set_t *firmware_set)
{
    if (firmware_set == NULL) return ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT;
    *firmware_set = (esp_base_ota_firmware_set_t){0};
    if ((observation != ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE && prepared != NULL) ||
        (observation == ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE && prepared == NULL) ||
        (observation != ESP_BASE_OTA_FIRMWARE_CONFIRMED &&
         observation != ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL &&
         observation != ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE)) {
        return ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT;
    }
    if (!eota_available()) return ESP_BASE_OTA_FIRMWARE_UNSUPPORTED;

    const eota_policy_t policy = esp_base_ota_policy(false);
    const bool pending_trial = observation == ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL;
    const bool prepared_candidate = observation == ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE;
    eota_slots_t before;
    if (eota_observe_slots(&policy, &before) != EOTA_UPDATE_OK ||
        before.running_subtype != before.boot_subtype ||
        before.running_address_bytes != before.boot_address_bytes ||
        before.running_size_bytes != before.boot_size_bytes ||
        before.running_state != (pending_trial ? EOTA_STATE_PENDING_VERIFY :
                                EOTA_STATE_VALID) ||
        (prepared_candidate && !prepared_candidate_valid(prepared, &before))) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }

    /* A pending trial must retain a proven VALID rollback image. A confirmed
     * image may have no rollback if the inactive image is provably invalid.
     * Any other state might still be loaded by the bootloader's fallback scan. */
    const bool has_rollback = before.target_state == EOTA_STATE_VALID;
    if (pending_trial && !has_rollback) return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    if (!pending_trial && !prepared_candidate && !has_rollback &&
        before.target_state != EOTA_STATE_UNTRACKED &&
        before.target_state != EOTA_STATE_INVALID &&
        before.target_state != EOTA_STATE_ABORTED) return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    if (has_rollback && !esp_ota_check_rollback_is_possible()) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }

    uint32_t running_size = 0, target_size = 0;
    uint8_t running_sha256[EOTA_SHA256_BYTES], target_sha256[EOTA_SHA256_BYTES];
    if (eota_sha256_verified_image(&policy, before.running_subtype, &running_size,
                                   running_sha256) != EOTA_UPDATE_OK ||
        running_size == 0 || zero_digest(running_sha256) ||
        !matching_base_image(&policy, before.running_subtype,
                             before.running_address_bytes, before.running_size_bytes)) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }
    const eota_result_t target_result = eota_sha256_verified_image(
        &policy, before.target_subtype, &target_size, target_sha256);
    if (prepared_candidate || has_rollback) {
        if (target_result != EOTA_UPDATE_OK || target_size == 0 ||
            zero_digest(target_sha256) ||
            !matching_base_image(&policy, before.target_subtype,
                                 before.target_address_bytes, before.target_size_bytes)) {
            return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
        }
        if (prepared_candidate &&
            (target_size != prepared->image_size_bytes ||
             memcmp(target_sha256, prepared->sha256, EOTA_SHA256_BYTES) != 0 ||
             memcmp(target_sha256, running_sha256, EOTA_SHA256_BYTES) == 0)) {
            return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
        }
    } else if (target_result != EOTA_UPDATE_IMAGE_INVALID) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }

    eota_slots_t after;
    if (eota_observe_slots(&policy, &after) != EOTA_UPDATE_OK ||
        !same_slots(&before, &after) ||
        (has_rollback && !esp_ota_check_rollback_is_possible())) {
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    }

    memcpy(firmware_set->running_firmware_sha256, running_sha256, EOTA_SHA256_BYTES);
    memcpy(firmware_set->bootable_firmware_sha256[0], running_sha256, EOTA_SHA256_BYTES);
    firmware_set->bootable_count = 1;
    if ((prepared_candidate || has_rollback) &&
        memcmp(running_sha256, target_sha256, EOTA_SHA256_BYTES) != 0) {
        memcpy(firmware_set->bootable_firmware_sha256[1], target_sha256, EOTA_SHA256_BYTES);
        firmware_set->bootable_count = 2;
    }
    return ESP_BASE_OTA_FIRMWARE_OK;
}
