// SPDX-License-Identifier: Apache-2.0
#include "esp_base_storage_owner.h"

#include <limits.h>
#include <stddef.h>

void esp_base_storage_owner_init(esp_base_storage_owner_t *owner)
{
    if (owner == NULL) return;
    atomic_init(&owner->next_token, 0U);
    atomic_init(&owner->active_token, 0U);
}

bool esp_base_storage_claim(esp_base_storage_owner_t *owner,
                            esp_base_storage_claim_t *claim)
{
    if (owner == NULL || claim == NULL || claim->owner != NULL || claim->token != 0U) {
        return false;
    }
    unsigned empty = 0U;
    if (!atomic_compare_exchange_strong_explicit(&owner->active_token, &empty, UINT_MAX,
                                                 memory_order_acquire, memory_order_relaxed)) {
        return false;
    }
    /* The reserved active value excludes other claimants without spending a token on BUSY. */
    const unsigned previous = atomic_load_explicit(&owner->next_token, memory_order_relaxed);
    if (previous >= UINT_MAX - 1U) {
        atomic_store_explicit(&owner->active_token, 0U, memory_order_release);
        return false; /* Never wrap or publish the reserved value as a claim. */
    }
    const unsigned token = previous + 1U;
    atomic_store_explicit(&owner->next_token, token, memory_order_relaxed);
    atomic_store_explicit(&owner->active_token, token, memory_order_release);
    claim->owner = owner;
    claim->token = token;
    return true;
}

bool esp_base_storage_release(esp_base_storage_claim_t *claim)
{
    if (claim == NULL || claim->owner == NULL || claim->token == 0U || claim->token == UINT_MAX) {
        return false;
    }
    unsigned token = claim->token;
    if (!atomic_compare_exchange_strong_explicit(&claim->owner->active_token, &token, 0U,
                                                 memory_order_release, memory_order_relaxed)) {
        return false;
    }
    claim->owner = NULL;
    claim->token = 0U;
    return true;
}

bool esp_base_storage_claim_active(const esp_base_storage_claim_t *claim)
{
    return claim != NULL && claim->owner != NULL && claim->token != 0U &&
           claim->token != UINT_MAX &&
           atomic_load_explicit(&claim->owner->active_token, memory_order_acquire) ==
               claim->token;
}
