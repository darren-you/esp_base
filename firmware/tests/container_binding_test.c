// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_binding.h"
#include "esp_base_ota_firmware.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static esp_base_ota_firmware_result_t observed_result;
static esp_base_ota_firmware_set_t observed_set;
static unsigned observe_calls, operation_calls, reconcile_calls;
static bool mutate_after_operation;
static esp_base_storage_owner_t *shared_owner;

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation,
    esp_base_ota_firmware_set_t *firmware_set)
{
    assert(observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    ++observe_calls;
    *firmware_set = (esp_base_ota_firmware_set_t){0};
    if (observed_result == ESP_BASE_OTA_FIRMWARE_OK) {
        *firmware_set = observed_set;
        if (mutate_after_operation && observe_calls == 2U) {
            firmware_set->bootable_firmware_sha256[1][0] ^= 1U;
        }
    }
    return observed_result;
}

static econtainer_slots_result_t inspect_set(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    ++operation_calls;
    assert(context == shared_owner);
    esp_base_storage_claim_t nested = {0};
    assert(!esp_base_storage_claim(shared_owner, &nested));
    assert(firmware_set->bootable_count == 2U);
    assert(firmware_set->running_firmware_sha256[0] == 0xa0);
    assert(firmware_set->bootable_firmware_sha256[0][0] == 0xa0);
    assert(firmware_set->bootable_firmware_sha256[1][0] == 0xb0);
    return ECONTAINER_SLOTS_OK;
}

static econtainer_slots_result_t uncertain_operation(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    assert(firmware_set != NULL && context == shared_owner);
    return ECONTAINER_SLOTS_UNCERTAIN;
}

econtainer_slots_result_t econtainer_slots_reconcile(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision)
{
    assert(io && geometry && firmware_set);
    ++reconcile_calls;
    assert(firmware_set->bootable_count == 2U);
    state->sequence = 17;
    *decision = ECONTAINER_SLOT_BOOT_CONFIRMED;
    return ECONTAINER_SLOTS_OK;
}

int main(void)
{
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    shared_owner = &owner;
    observed_result = ESP_BASE_OTA_FIRMWARE_OK;
    observed_set.bootable_count = 2;
    memset(observed_set.running_firmware_sha256, 0xa0, 32);
    memset(observed_set.bootable_firmware_sha256[0], 0xa0, 32);
    memset(observed_set.bootable_firmware_sha256[1], 0xb0, 32);
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) == ECONTAINER_SLOTS_OK);
    assert(observe_calls == 2U && operation_calls == 1U);

    esp_base_storage_claim_t busy = {0};
    assert(esp_base_storage_claim(&owner, &busy));
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) == ECONTAINER_SLOTS_BUSY);
    assert(observe_calls == 2U && operation_calls == 1U);
    assert(esp_base_storage_release(&busy));

    observed_result = ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    assert(operation_calls == 1U);
    observed_result = ESP_BASE_OTA_FIRMWARE_OK;
    observed_set.bootable_count = 0;
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    assert(operation_calls == 1U);
    observed_set.bootable_count = 2;
    observed_set.running_firmware_sha256[0] = 0xcc;
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    observed_set.running_firmware_sha256[0] = 0xa0;
    observed_set.bootable_count = 1;
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    observed_set.bootable_count = 2;
    memset(observed_set.bootable_firmware_sha256[1], 0xa0, 32);
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    memset(observed_set.bootable_firmware_sha256[1], 0xb0, 32);

    observe_calls = 0;
    mutate_after_operation = true;
    assert(esp_base_container_with_firmware_set(&owner, inspect_set, &owner) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    assert(operation_calls == 2U);
    esp_base_storage_claim_t quarantined = {0};
    assert(!esp_base_storage_claim(&owner, &quarantined));
    /* Resetting the owner here models a new boot, not same-boot recovery. */
    esp_base_storage_owner_init(&owner);
    mutate_after_operation = false;
    observe_calls = 0;

    econtainer_slots_state_t state = {.sequence = 99};
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_CONFIRMED;
    econtainer_slots_io_t io = {0};
    econtainer_slots_geometry_t geometry = {0};
    assert(esp_base_container_reconcile(&owner, &io, &geometry, &state, &decision) ==
           ECONTAINER_SLOTS_OK);
    assert(reconcile_calls == 1U && state.sequence == 17 &&
           decision == ECONTAINER_SLOT_BOOT_CONFIRMED);

    observed_result = ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    assert(esp_base_container_reconcile(&owner, &io, &geometry, &state, &decision) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    assert(reconcile_calls == 1U && state.sequence == 0U &&
           decision == ECONTAINER_SLOT_BOOT_BLOCKED);
    assert(esp_base_storage_claim(&owner, &quarantined));
    assert(esp_base_storage_release(&quarantined));
    observed_result = ESP_BASE_OTA_FIRMWARE_OK;
    assert(esp_base_container_with_firmware_set(&owner, uncertain_operation, &owner) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    assert(!esp_base_storage_claim(&owner, &quarantined));
    puts("  container_binding passed (exact set, shared owner, uncertain boot block)");
}
