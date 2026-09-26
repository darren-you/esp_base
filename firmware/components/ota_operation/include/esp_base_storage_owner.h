// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One owner for Base app/otadata changes and product package operations.
 * A claim may pass from the USB control task to its OTA worker. No FreeRTOS
 * mutex is held across tasks; each claim has a unique, checked token. */
typedef struct {
    atomic_uint next_token;
    atomic_uint active_token;
} esp_base_storage_owner_t;

typedef struct {
    esp_base_storage_owner_t *owner;
    unsigned token;
} esp_base_storage_claim_t;

/* Call once on each boot before starting any operation task. */
void esp_base_storage_owner_init(esp_base_storage_owner_t *owner);
bool esp_base_storage_claim(esp_base_storage_owner_t *owner,
                            esp_base_storage_claim_t *claim);
bool esp_base_storage_claim_active(const esp_base_storage_claim_t *claim);
bool esp_base_storage_release(esp_base_storage_claim_t *claim);
