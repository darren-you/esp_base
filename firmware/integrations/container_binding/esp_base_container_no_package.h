// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "esp_container_slots.h"

/* Called only inside Base's CONFIRMED signed-set double observation and claim.
 * The persisted key must be truly absent; damaged or unreadable records block. */
econtainer_slots_result_t esp_base_container_initialize_no_package(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *firmware_set, bool *initialized);
