// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_binding.h"

#include <string.h>

#include "esp_base_ota_firmware.h"

_Static_assert(ESP_BASE_OTA_FIRMWARE_LIMIT == ECONTAINER_SLOT_BINDING_COUNT,
               "Base and Container firmware limits differ");
_Static_assert(EOTA_SHA256_BYTES == sizeof(((econtainer_slot_firmware_set_t *)0)->running_firmware_sha256),
               "Base and Container firmware digests differ");

static bool map_firmware_set(const esp_base_ota_firmware_set_t *source,
                             econtainer_slot_firmware_set_t *destination)
{
    if (source->bootable_count == 0U ||
        source->bootable_count > ECONTAINER_SLOT_BINDING_COUNT ||
        memcmp(source->running_firmware_sha256, source->bootable_firmware_sha256[0],
               EOTA_SHA256_BYTES) != 0) return false;
    for (unsigned index = 0; index < ESP_BASE_OTA_FIRMWARE_LIMIT; ++index) {
        unsigned nonzero = 0U;
        for (unsigned byte = 0; byte < EOTA_SHA256_BYTES; ++byte) {
            nonzero |= source->bootable_firmware_sha256[index][byte];
        }
        if ((index < source->bootable_count) != (nonzero != 0U)) return false;
    }
    if (source->bootable_count == 2U &&
        memcmp(source->bootable_firmware_sha256[0],
               source->bootable_firmware_sha256[1], EOTA_SHA256_BYTES) == 0) return false;
    *destination = (econtainer_slot_firmware_set_t){0};
    destination->bootable_count = source->bootable_count;
    memcpy(destination->running_firmware_sha256, source->running_firmware_sha256,
           EOTA_SHA256_BYTES);
    for (unsigned index = 0; index < source->bootable_count; ++index) {
        memcpy(destination->bootable_firmware_sha256[index],
               source->bootable_firmware_sha256[index], EOTA_SHA256_BYTES);
    }
    return true;
}

econtainer_slots_result_t esp_base_container_with_firmware_set(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_firmware_observation_t observation,
    esp_base_container_operation_fn operation, void *context)
{
    if (operation == NULL ||
        (observation != ESP_BASE_OTA_FIRMWARE_CONFIRMED &&
         observation != ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL)) {
        return ECONTAINER_SLOTS_INVALID;
    }
    if (!esp_base_storage_claim_active(claim)) return ECONTAINER_SLOTS_BUSY;

    esp_base_ota_firmware_set_t before = {0};
    esp_base_ota_firmware_set_t after = {0};
    econtainer_slot_firmware_set_t mapped = {0};
    econtainer_slots_result_t result = ECONTAINER_SLOTS_UNCERTAIN;
    if (esp_base_ota_observe_firmware_set(
            observation, NULL, &before) == ESP_BASE_OTA_FIRMWARE_OK &&
        map_firmware_set(&before, &mapped)) {
        result = operation(&mapped, context);
        if (esp_base_ota_observe_firmware_set(
                observation, NULL, &after) != ESP_BASE_OTA_FIRMWARE_OK ||
            memcmp(&before, &after, sizeof before) != 0) {
            result = ECONTAINER_SLOTS_UNCERTAIN;
        }
    }
    return result;
}

typedef struct {
    const econtainer_slots_io_t *io;
    const econtainer_slots_geometry_t *geometry;
    econtainer_slots_state_t *state;
    econtainer_slot_boot_decision_t *decision;
} reconcile_context_t;

static econtainer_slots_result_t reconcile_callback(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    reconcile_context_t *reconcile = context;
    return econtainer_slots_reconcile(reconcile->io, reconcile->geometry,
                                      firmware_set, reconcile->state, reconcile->decision);
}

econtainer_slots_result_t esp_base_container_reconcile(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_firmware_observation_t observation,
    const econtainer_slots_io_t *io,
    const econtainer_slots_geometry_t *geometry, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision)
{
    if (decision != NULL) *decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    if (state != NULL) *state = (econtainer_slots_state_t){0};
    if (claim == NULL || io == NULL || geometry == NULL || state == NULL || decision == NULL) {
        return ECONTAINER_SLOTS_INVALID;
    }
    reconcile_context_t context = {.io = io, .geometry = geometry,
                                   .state = state, .decision = decision};
    const econtainer_slots_result_t result = esp_base_container_with_firmware_set(
        claim, observation, reconcile_callback, &context);
    if (result == ECONTAINER_SLOTS_UNCERTAIN) {
        *state = (econtainer_slots_state_t){0};
        *decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    }
    return result;
}
