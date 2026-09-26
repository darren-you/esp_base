// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_base_ota_firmware.h"
#include "esp_base_storage_owner.h"
#include "esp_container_slots.h"

/* The caller already holds the same Base owner used by startup and OTA. The
 * callback may perform one Container operation, but cannot retain the set or
 * write app/otadata. Container's slot lock is a distinct lock. UNCERTAIN after
 * a callback means the caller must retain its claim for this boot. */
typedef econtainer_slots_result_t (*esp_base_container_operation_fn)(
    const econtainer_slot_firmware_set_t *firmware_set, void *context);

econtainer_slots_result_t esp_base_container_with_firmware_set(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_firmware_observation_t observation,
    esp_base_container_operation_fn operation, void *context);

/* Startup selection is blocked until the physical signed firmware set and
 * every persisted Container binding agree. This does not install packages. */
econtainer_slots_result_t esp_base_container_reconcile(
    const esp_base_storage_claim_t *claim,
    esp_base_ota_firmware_observation_t observation,
    const econtainer_slots_io_t *io,
    const econtainer_slots_geometry_t *geometry, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision);
