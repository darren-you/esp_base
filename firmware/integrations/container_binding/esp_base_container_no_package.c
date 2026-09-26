// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_no_package.h"

#include <string.h>

econtainer_slots_result_t esp_base_container_initialize_no_package(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set, bool *initialized)
{
    if (initialized != NULL) *initialized = false;
    if (io == NULL || geometry == NULL || firmware_set == NULL || initialized == NULL ||
        firmware_set->bootable_count == 0U ||
        firmware_set->bootable_count > ECONTAINER_SLOT_BINDING_COUNT) {
        return ECONTAINER_SLOTS_INVALID;
    }
    econtainer_slots_state_t state = {0};
    const econtainer_slots_result_t loaded = econtainer_slots_load(
        io, geometry, &state);
    if (loaded != ECONTAINER_SLOTS_EMPTY) return loaded;

    econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT] = {0};
    for (size_t index = 0; index < firmware_set->bootable_count; ++index) {
        bindings[index].present = true;
        memcpy(bindings[index].firmware_sha256,
               firmware_set->bootable_firmware_sha256[index],
               sizeof bindings[index].firmware_sha256);
    }
    const econtainer_slots_result_t result = econtainer_slots_initialize(
        io, geometry, firmware_set, bindings);
    *initialized = result == ECONTAINER_SLOTS_OK;
    return result;
}
