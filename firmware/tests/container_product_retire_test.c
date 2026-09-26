// SPDX-License-Identifier: Apache-2.0
/* Host-only product glue test: Container and IDF persistence are fake, while
 * the Base double-observation adapter and owner are the real implementations. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

bool test_policy_enabled = true;
#include "esp_base_container_product.c"

static esp_base_ota_firmware_set_t physical;
static econtainer_slots_state_t persisted;
static unsigned observe_calls;
static unsigned bind_calls;
static unsigned retire_calls;
static unsigned abandon_calls;
static unsigned drop_calls;
static bool observation_changes;
static bool load_fails;
static esp_base_storage_owner_t owner;
static esp_base_storage_claim_t claim;
static const char operation_id[] = "11111111-1111-4111-8111-111111111111";
static const char boot_id[] = "22222222-2222-4222-8222-222222222222";

static void fill_sha(uint8_t sha256[32], uint8_t value)
{
    memset(sha256, value, 32);
}

static void physical_set(bool two)
{
    physical = (esp_base_ota_firmware_set_t){0};
    physical.bootable_count = two ? 2U : 1U;
    fill_sha(physical.running_firmware_sha256, 0xa1);
    fill_sha(physical.bootable_firmware_sha256[0], 0xa1);
    if (two) fill_sha(physical.bootable_firmware_sha256[1], 0xb2);
}

static void persisted_set(bool two, uint32_t sequence)
{
    persisted = (econtainer_slots_state_t){0};
    persisted.sequence = sequence;
    persisted.phase = ECONTAINER_SLOT_IDLE;
    persisted.bindings[0].present = true;
    fill_sha(persisted.bindings[0].firmware_sha256, 0xa1);
    if (two) {
        persisted.bindings[1].present = true;
        fill_sha(persisted.bindings[1].firmware_sha256, 0xb2);
    }
}

static void fixture(bool configured, bool two)
{
    memset(&s_product, 0, sizeof s_product);
    test_policy_enabled = configured;
    physical_set(two);
    persisted_set(two, 7U);
    observe_calls = bind_calls = retire_calls = abandon_calls = drop_calls = 0U;
    observation_changes = load_fails = false;
    esp_base_storage_owner_init(&owner);
    claim = (esp_base_storage_claim_t){0};
    assert(esp_base_storage_claim(&owner, &claim));
    if (configured) {
        s_product.provider_bound = true;
        atomic_store(&s_product.boot_admitted, true);
        atomic_store(&s_product.result, ESP_BASE_CONTAINER_EMPTY);
    }
}

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation,
    const eota_prepared_t *prepared, esp_base_ota_firmware_set_t *firmware_set)
{
    assert(observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED && prepared == NULL);
    *firmware_set = physical;
    ++observe_calls;
    if (observation_changes && (observe_calls % 2U) == 0U)
        firmware_set->running_firmware_sha256[0] ^= 1U;
    return ESP_BASE_OTA_FIRMWARE_OK;
}

static bool fake_set_matches(const econtainer_slot_firmware_set_t *set)
{
    const uint8_t zero[32] = {0};
    const uint8_t *other = set->bootable_count == 2U ?
        set->bootable_firmware_sha256[1] : zero;
    return state_has_firmware(&persisted, set->running_firmware_sha256, other, false);
}

econtainer_slots_result_t econtainer_slots_load(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    econtainer_slots_state_t *state)
{
    assert(io && geometry && state);
    if (load_fails) return ECONTAINER_SLOTS_IO_FAILED;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_reconcile(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    const econtainer_slot_firmware_set_t *set, econtainer_slots_state_t *state,
    econtainer_slot_boot_decision_t *decision)
{
    assert(io && geometry && set && state && decision);
    if (load_fails) return ECONTAINER_SLOTS_IO_FAILED;
    if (!fake_set_matches(set)) return ECONTAINER_SLOTS_CONFLICT;
    *state = persisted;
    *decision = ECONTAINER_SLOT_BOOT_CONFIRMED;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_retire_inactive_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *actual_set,
    const uint8_t retired_sha256[32], econtainer_slots_state_t *state)
{
    assert(io && geometry && state && actual_set && retired_sha256);
    ++retire_calls;
    if (persisted.sequence != expected_sequence ||
        actual_set->bootable_count != 1U ||
        !state_has_firmware(&persisted, actual_set->running_firmware_sha256,
                            retired_sha256, false)) return ECONTAINER_SLOTS_CONFLICT;
    persisted.bindings[1] = (econtainer_slot_binding_t){0};
    persisted.phase = ECONTAINER_SLOT_IDLE;
    persisted.operation = (econtainer_slot_operation_t){0};
    ++persisted.sequence;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_abandon(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const uint8_t current_boot_id[16],
    econtainer_slot_trial_stopped_fn stopped_fn, void *stopped_context,
    econtainer_slots_state_t *state)
{
    assert(io && geometry && state && current_boot_id);
    assert(stopped_fn == NULL && stopped_context == NULL);
    ++abandon_calls;
    if (persisted.sequence != expected_sequence ||
        persisted.phase < ECONTAINER_SLOT_WRITING ||
        persisted.phase > ECONTAINER_SLOT_HEALTH_VERIFIED)
        return ECONTAINER_SLOTS_CONFLICT;
    persisted.phase = ECONTAINER_SLOT_ABORTED;
    ++persisted.sequence;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

econtainer_slots_result_t econtainer_slots_drop_aborted_firmware(
    const econtainer_slots_io_t *io, const econtainer_slots_geometry_t *geometry,
    uint32_t expected_sequence, const econtainer_slot_firmware_set_t *actual_set,
    econtainer_slots_state_t *state)
{
    assert(io && geometry && state && actual_set);
    ++drop_calls;
    if (persisted.sequence != expected_sequence ||
        persisted.phase != ECONTAINER_SLOT_ABORTED ||
        actual_set->bootable_count != 1U) return ECONTAINER_SLOTS_CONFLICT;
    persisted.bindings[1] = (econtainer_slot_binding_t){0};
    persisted.phase = ECONTAINER_SLOT_IDLE;
    persisted.operation = (econtainer_slot_operation_t){0};
    ++persisted.sequence;
    *state = persisted;
    return ECONTAINER_SLOTS_OK;
}

bool econtainer_slots_idf_bind(econtainer_slots_idf_provider_t *provider,
                               const econtainer_slots_idf_config_t *config)
{
    assert(provider && config && config->storage_lock);
    ++bind_calls;
    memset(provider, 0, sizeof *provider);
    return true;
}

esp_err_t nvs_flash_init_partition(const char *label)
{
    assert(label && label[0]);
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }

static void staged_candidate(uint32_t sequence)
{
    persisted_set(false, sequence);
    persisted.bindings[1].present = true;
    fill_sha(persisted.bindings[1].firmware_sha256, 0xc3);
    persisted.phase = ECONTAINER_SLOT_PREPARED;
    persisted.operation.kind = ECONTAINER_SLOT_NO_PACKAGE;
    persisted.operation.firmware_transition = true;
    fill_sha(persisted.operation.target_firmware_sha256, 0xc3);
    assert(decode_uuid(operation_id, persisted.operation.operation_id));
}

static esp_base_container_retire_result_t recover(bool enabled, uint32_t sequence,
                                                   uint8_t candidate_value,
                                                   const char *operation)
{
    uint8_t source[32], inactive[32], candidate[32];
    fill_sha(source, 0xa1);
    fill_sha(inactive, 0xb2);
    fill_sha(candidate, candidate_value);
    return esp_base_container_product_recover_retired_firmware(
        &claim, enabled, sequence, source, inactive, candidate,
        operation, boot_id);
}

int main(void)
{
    uint8_t source[32], inactive[32];
    fill_sha(source, 0xa1);
    fill_sha(inactive, 0xb2);
    esp_base_ota_receipt_snapshot_t snapshot = {0};

    fixture(true, true);
    assert(esp_base_container_product_snapshot_for_ota(&claim, &snapshot));
    assert(snapshot.container_enabled && snapshot.container_sequence == 7U &&
           memcmp(snapshot.source_sha256, source, 32) == 0 &&
           memcmp(snapshot.inactive_sha256, inactive, 32) == 0 &&
           observe_calls == 2U);
    persisted.bindings[0].package_present = true;
    assert(!esp_base_container_product_snapshot_for_ota(&claim, &snapshot));
    assert(snapshot.container_sequence == 0U);

    fixture(false, true);
    assert(esp_base_container_product_snapshot_for_ota(&claim, &snapshot));
    assert(!snapshot.container_enabled && snapshot.container_sequence == 0U &&
           memcmp(snapshot.inactive_sha256, inactive, 32) == 0);
    physical_set(false);
    assert(esp_base_container_product_retire_inactive(&claim, false, 0U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);

    fixture(true, true);
    physical_set(false); /* eota_retire proved first sector erased. */
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 8U && retire_calls == 1U && observe_calls == 2U);
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(retire_calls == 1U);
    assert(esp_base_container_product_retire_inactive(&claim, true, 6U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    observation_changes = true;
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, inactive) == ESP_BASE_CONTAINER_RETIRE_UNCERTAIN);
    observation_changes = false;

    fixture(true, false);
    uint8_t no_inactive[32] = {0};
    assert(esp_base_container_product_snapshot_for_ota(&claim, &snapshot));
    assert(snapshot.container_sequence == 7U &&
           memcmp(snapshot.inactive_sha256, no_inactive, 32) == 0);
    assert(esp_base_container_product_retire_inactive(&claim, true, 7U,
        source, no_inactive) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 7U && retire_calls == 0U);

    fixture(true, true);
    physical_set(false);
    s_product.provider_bound = false;
    atomic_store(&s_product.boot_admitted, false);
    persisted.phase = ECONTAINER_SLOT_CONFIRMED;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 8U && bind_calls == 1U && retire_calls == 1U);
    assert(ensure_provider(&claim) && bind_calls == 1U);

    fixture(true, true);
    physical_set(false);
    s_product.provider_bound = false;
    atomic_store(&s_product.boot_admitted, false);
    staged_candidate(9U); /* S=7, retire=8, stage=9. */
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.phase == ECONTAINER_SLOT_IDLE && persisted.sequence == 11U &&
           abandon_calls == 1U && drop_calls == 1U);
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(abandon_calls == 1U && drop_calls == 1U);

    fixture(true, false);
    s_product.provider_bound = false;
    staged_candidate(8U); /* Original A-only S=7, stage=8. */
    uint8_t candidate[32];
    fill_sha(candidate, 0xc3);
    assert(esp_base_container_product_recover_retired_firmware(
        &claim, true, 7U, source, no_inactive, candidate,
        operation_id, boot_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 10U && abandon_calls == 1U && drop_calls == 1U);

    fixture(true, true);
    physical_set(false);
    staged_candidate(9U);
    assert(recover(true, 7U, 0xc3,
        "99999999-9999-4999-8999-999999999999") == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    assert(recover(true, 7U, 0xd4, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    ++persisted.sequence;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);

    fixture(true, true);
    physical_set(false);
    staged_candidate(10U);
    persisted.phase = ECONTAINER_SLOT_TRIAL_STARTED;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(persisted.sequence == 12U && abandon_calls == 1U && drop_calls == 1U);

    fixture(true, true);
    physical_set(false);
    s_product.ready = (void *)1;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    assert(retire_calls == 0U);

    fixture(true, true);
    physical_set(false);
    staged_candidate(10U); /* trial began after stage */
    persisted.phase = ECONTAINER_SLOT_TRIAL_STARTED;
    assert(decode_uuid(boot_id, persisted.operation.trial_boot_id));
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);

    fixture(true, true);
    physical_set(false);
    staged_candidate(10U);
    persisted.phase = ECONTAINER_SLOT_ABORTED;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(abandon_calls == 0U && drop_calls == 1U && persisted.sequence == 11U);

    fixture(true, true);
    physical_set(false);
    load_fails = true;
    assert(recover(true, 7U, 0xc3, operation_id) == ESP_BASE_CONTAINER_RETIRE_UNCERTAIN);
    assert(esp_base_storage_claim_active(&claim));
    puts("  container_product_retire passed (snapshot, retirement, recovery, replay guards)");
}
