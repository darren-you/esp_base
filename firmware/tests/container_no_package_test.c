// SPDX-License-Identifier: Apache-2.0
#include "esp_base_container_no_package.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static econtainer_slots_result_t loaded_result;
static econtainer_slots_result_t initialized_result;
static unsigned load_calls;
static unsigned initialize_calls;

econtainer_slots_result_t econtainer_slots_load(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    econtainer_slots_state_t *state)
{
    assert(io != NULL && geometry != NULL && state != NULL);
    ++load_calls;
    *state = (econtainer_slots_state_t){0};
    return loaded_result;
}

econtainer_slots_result_t econtainer_slots_initialize(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *set,
    const econtainer_slot_binding_t bindings[ECONTAINER_SLOT_BINDING_COUNT])
{
    assert(io != NULL && geometry != NULL && set != NULL && bindings != NULL);
    ++initialize_calls;
    for (size_t index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        assert(bindings[index].present == (index < set->bootable_count));
        assert(!bindings[index].package_present);
        if (index < set->bootable_count) {
            assert(memcmp(bindings[index].firmware_sha256,
                          set->bootable_firmware_sha256[index], 32) == 0);
        }
    }
    return initialized_result;
}

int main(void)
{
    econtainer_slots_io_t io = {0};
    econtainer_slots_geometry_t geometry = {0};
    econtainer_slot_firmware_set_t set = {.bootable_count = 2};
    memset(set.bootable_firmware_sha256[0], 0xa0, 32);
    memset(set.bootable_firmware_sha256[1], 0xb0, 32);
    memcpy(set.running_firmware_sha256, set.bootable_firmware_sha256[0], 32);
    bool created = true;

    loaded_result = ECONTAINER_SLOTS_EMPTY;
    initialized_result = ECONTAINER_SLOTS_OK;
    assert(esp_base_container_initialize_no_package(&io, &geometry, &set, &created) ==
           ECONTAINER_SLOTS_OK);
    assert(created && load_calls == 1U && initialize_calls == 1U);

    set.bootable_count = 1;
    assert(esp_base_container_initialize_no_package(&io, &geometry, &set, &created) ==
           ECONTAINER_SLOTS_OK);
    assert(created && load_calls == 2U && initialize_calls == 2U);

    loaded_result = ECONTAINER_SLOTS_OK;
    assert(esp_base_container_initialize_no_package(&io, &geometry, &set, &created) ==
           ECONTAINER_SLOTS_OK);
    assert(!created && load_calls == 3U && initialize_calls == 2U);

    loaded_result = ECONTAINER_SLOTS_INVALID;
    assert(esp_base_container_initialize_no_package(&io, &geometry, &set, &created) ==
           ECONTAINER_SLOTS_INVALID);
    assert(!created && load_calls == 4U && initialize_calls == 2U);

    loaded_result = ECONTAINER_SLOTS_IO_FAILED;
    assert(esp_base_container_initialize_no_package(&io, &geometry, &set, &created) ==
           ECONTAINER_SLOTS_IO_FAILED);
    assert(!created && load_calls == 5U && initialize_calls == 2U);

    loaded_result = ECONTAINER_SLOTS_EMPTY;
    initialized_result = ECONTAINER_SLOTS_UNCERTAIN;
    assert(esp_base_container_initialize_no_package(&io, &geometry, &set, &created) ==
           ECONTAINER_SLOTS_UNCERTAIN);
    assert(!created && load_calls == 6U && initialize_calls == 3U);

    set.bootable_count = 0;
    assert(esp_base_container_initialize_no_package(&io, &geometry, &set, &created) ==
           ECONTAINER_SLOTS_INVALID);
    assert(!created && load_calls == 6U && initialize_calls == 3U);
    puts("  container_no_package passed (absent key initialization; existing, damaged and unreadable keys block)");
    return 0;
}
