#include "esp_base_ota_firmware.h"
#include "esp_base_ota_policy.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_partition.h"

static bool signed_enabled, rollback_possible, change_during_hash;
static unsigned observe_calls, verify_calls, rollback_calls;
static uint8_t running_subtype, boot_subtype, image_seed[2];
static eota_state_t running_state, target_state;
static eota_result_t image_result[2];
static esp_partition_t partitions[2];
static uint16_t image_chip_id[2];
static uint8_t image_magic[2];
static uint32_t description_magic[2];
static char image_project[2][32];
static bool read_failure[2], description_failure[2];

static void reset(void)
{
    signed_enabled = rollback_possible = true;
    change_during_hash = false;
    observe_calls = verify_calls = rollback_calls = 0;
    running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    running_state = target_state = EOTA_STATE_VALID;
    image_seed[0] = 0xa0;
    image_seed[1] = 0xb0;
    image_result[0] = image_result[1] = EOTA_UPDATE_OK;
    partitions[0] = (esp_partition_t){.type = ESP_PARTITION_TYPE_APP,
        .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .address = ESP_BASE_OTA_0_ADDRESS_BYTES, .size = ESP_BASE_OTA_SLOT_SIZE_BYTES};
    partitions[1] = (esp_partition_t){.type = ESP_PARTITION_TYPE_APP,
        .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1, .address = ESP_BASE_OTA_1_ADDRESS_BYTES, .size = ESP_BASE_OTA_SLOT_SIZE_BYTES};
    for (size_t i = 0; i < 2; ++i) {
        image_chip_id[i] = CONFIG_IDF_FIRMWARE_CHIP_ID;
        image_magic[i] = ESP_IMAGE_HEADER_MAGIC;
        description_magic[i] = ESP_APP_DESC_MAGIC_WORD;
        strcpy(image_project[i], "esp_base");
        read_failure[i] = description_failure[i] = false;
    }
}

const esp_partition_t *esp_partition_find_first(uint8_t type, uint8_t subtype,
                                                 const char *label)
{
    assert(type == ESP_PARTITION_TYPE_APP && label == NULL);
    if (subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return &partitions[0];
    if (subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return &partitions[1];
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *destination, size_t size)
{
    assert(partition == &partitions[0] || partition == &partitions[1]);
    assert(offset == 0 && (size == sizeof(esp_image_header_t) || size == 1));
    const size_t index = (size_t)(partition - partitions);
    if (read_failure[index]) return ESP_FAIL;
    if (size == 1) {
        *(uint8_t *)destination = image_magic[index];
        return ESP_OK;
    }
    *(esp_image_header_t *)destination = (esp_image_header_t){
        .magic = image_magic[index], .chip_id = image_chip_id[index]};
    return ESP_OK;
}

esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition,
                                             esp_app_desc_t *description)
{
    assert(partition == &partitions[0] || partition == &partitions[1]);
    const size_t index = (size_t)(partition - partitions);
    if (description_failure[index]) return ESP_FAIL;
    memset(description, 0, sizeof *description);
    description->magic_word = description_magic[index];
    memcpy(description->project_name, image_project[index],
           sizeof description->project_name);
    return ESP_OK;
}

bool eota_available(void) { return signed_enabled; }

eota_result_t eota_observe_slots(const eota_policy_t *policy, eota_slots_t *slots)
{
    assert(policy && slots && !strcmp(policy->project_name, "esp_base") &&
           policy->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID &&
           policy->ota_0_address_bytes == ESP_BASE_OTA_0_ADDRESS_BYTES &&
           policy->ota_1_address_bytes == ESP_BASE_OTA_1_ADDRESS_BYTES &&
           policy->ota_size_bytes == ESP_BASE_OTA_SLOT_SIZE_BYTES);
    const uint8_t other = running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ?
                          ESP_PARTITION_SUBTYPE_APP_OTA_1 : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    *slots = (eota_slots_t){
        .running_subtype = running_subtype,
        .boot_subtype = boot_subtype,
        .target_subtype = other,
        .running_address_bytes = running_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? ESP_BASE_OTA_0_ADDRESS_BYTES : ESP_BASE_OTA_1_ADDRESS_BYTES,
        .boot_address_bytes = boot_subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? ESP_BASE_OTA_0_ADDRESS_BYTES : ESP_BASE_OTA_1_ADDRESS_BYTES,
        .target_address_bytes = other == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? ESP_BASE_OTA_0_ADDRESS_BYTES : ESP_BASE_OTA_1_ADDRESS_BYTES,
        .running_size_bytes = ESP_BASE_OTA_SLOT_SIZE_BYTES,
        .boot_size_bytes = ESP_BASE_OTA_SLOT_SIZE_BYTES,
        .target_size_bytes = ESP_BASE_OTA_SLOT_SIZE_BYTES,
        .running_state = running_state,
        .target_state = target_state,
    };
    ++observe_calls;
    if (change_during_hash && observe_calls == 2) slots->target_state = EOTA_STATE_INVALID;
    return EOTA_UPDATE_OK;
}

eota_result_t eota_sha256_verified_image(const eota_policy_t *policy, uint8_t subtype,
                                         uint32_t *image_size_bytes, uint8_t digest[32])
{
    assert(policy && image_size_bytes && digest &&
           (subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1));
    ++verify_calls;
    const unsigned index = subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
    *image_size_bytes = 0;
    memset(digest, 0, 32);
    if (image_result[index] != EOTA_UPDATE_OK) return image_result[index];
    *image_size_bytes = 123456;
    memset(digest, image_seed[index], 32);
    return EOTA_UPDATE_OK;
}

bool esp_ota_check_rollback_is_possible(void)
{
    ++rollback_calls;
    return rollback_possible;
}

static void expect_uncertain(esp_base_ota_firmware_observation_t observation)
{
    esp_base_ota_firmware_set_t set;
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(observation, NULL, &set) ==
           ESP_BASE_OTA_FIRMWARE_UNCERTAIN);
    const esp_base_ota_firmware_set_t empty = {0};
    assert(memcmp(&set, &empty, sizeof set) == 0);
}

static eota_prepared_t prepared_receipt(void)
{
    eota_slots_t slots = {0};
    const eota_policy_t policy = esp_base_ota_policy(false);
    assert(eota_observe_slots(&policy, &slots) == EOTA_UPDATE_OK);
    eota_prepared_t prepared = {.slots = slots, .image_size_bytes = 123456};
    const unsigned target_index = slots.target_subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
    memset(prepared.sha256, image_seed[target_index], sizeof prepared.sha256);
    /* esp_ota_begin invalidates the old B otadata before eota_prepare succeeds. */
    target_state = EOTA_STATE_UNTRACKED;
    observe_calls = 0;
    return prepared;
}

static void expect_prepared_uncertain(const eota_prepared_t *prepared)
{
    esp_base_ota_firmware_set_t set;
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, prepared, &set) ==
        ESP_BASE_OTA_FIRMWARE_UNCERTAIN);
    const esp_base_ota_firmware_set_t empty = {0};
    assert(memcmp(&set, &empty, sizeof set) == 0);
}

int main(void)
{
    esp_base_ota_firmware_set_t set;
    reset();
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, NULL) ==
        ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT);
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(
               (esp_base_ota_firmware_observation_t)3, NULL, &set) ==
           ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT);
    const esp_base_ota_firmware_set_t empty = {0};
    assert(memcmp(&set, &empty, sizeof set) == 0 && observe_calls == 0);
    signed_enabled = false;
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &set) == ESP_BASE_OTA_FIRMWARE_UNSUPPORTED);
    assert(memcmp(&set, &empty, sizeof set) == 0 && observe_calls == 0);

    reset();
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 2 && set.running_firmware_sha256[0] == 0xa0 &&
           set.bootable_firmware_sha256[0][0] == 0xa0 &&
           set.bootable_firmware_sha256[1][0] == 0xb0 &&
           observe_calls == 2 && verify_calls == 2 && rollback_calls == 2);

    reset(); running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 2 && set.running_firmware_sha256[0] == 0xb0 &&
           set.bootable_firmware_sha256[1][0] == 0xa0);

    reset(); image_seed[1] = image_seed[0];
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 1 && set.bootable_firmware_sha256[0][0] == 0xa0 &&
           set.bootable_firmware_sha256[1][0] == 0);

    reset(); target_state = EOTA_STATE_UNTRACKED;
    image_result[1] = EOTA_UPDATE_IMAGE_INVALID;
    image_magic[1] = 0xff;
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 1 && set.running_firmware_sha256[0] == 0xa0 && rollback_calls == 0);

    reset(); running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    target_state = EOTA_STATE_INVALID;
    image_result[0] = EOTA_UPDATE_IMAGE_INVALID;
    image_magic[0] = 0xff;
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &set) == ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 1 && set.running_firmware_sha256[0] == 0xb0);

    reset(); target_state = EOTA_STATE_UNTRACKED;
    image_result[1] = EOTA_UPDATE_IMAGE_INVALID;
    expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED); /* Bad app signature, bootable header. */
    image_magic[1] = 0xff;
    read_failure[1] = true;
    expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);

    reset(); running_state = EOTA_STATE_PENDING_VERIFY;
    expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL, NULL, &set) ==
           ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 2 && set.running_firmware_sha256[0] == 0xa0 &&
           set.bootable_firmware_sha256[1][0] == 0xb0 && rollback_calls == 2);
    reset(); running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    running_state = EOTA_STATE_PENDING_VERIFY;
    assert(esp_base_ota_observe_firmware_set(ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL, NULL, &set) ==
           ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 2 && set.running_firmware_sha256[0] == 0xb0 &&
           set.bootable_firmware_sha256[1][0] == 0xa0);
    reset(); expect_uncertain(ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL);
    reset(); running_state = EOTA_STATE_PENDING_VERIFY;
    target_state = EOTA_STATE_UNTRACKED; image_result[1] = EOTA_UPDATE_IMAGE_INVALID;
    expect_uncertain(ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL);
    reset(); running_state = EOTA_STATE_PENDING_VERIFY; rollback_possible = false;
    expect_uncertain(ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL);
    reset(); running_state = EOTA_STATE_PENDING_VERIFY;
    image_result[1] = EOTA_UPDATE_IMAGE_INVALID;
    expect_uncertain(ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL);
    reset(); running_state = EOTA_STATE_PENDING_VERIFY; change_during_hash = true;
    expect_uncertain(ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL);
    reset(); boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); target_state = EOTA_STATE_NEW; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); target_state = EOTA_STATE_UNDEFINED; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); target_state = EOTA_STATE_PENDING_VERIFY; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); rollback_possible = false; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); image_result[0] = EOTA_UPDATE_IMAGE_INVALID; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); image_result[1] = EOTA_UPDATE_IMAGE_INVALID; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); image_chip_id[0] = 0x0009; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); image_chip_id[1] = 0x0009; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); strcpy(image_project[0], "other_product"); expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); strcpy(image_project[1], "other_product"); expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); image_magic[0] = 0; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); description_magic[1] = 0; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); read_failure[0] = true; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); description_failure[1] = true; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); partitions[1].address += 0x1000; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); target_state = EOTA_STATE_INVALID; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); target_state = EOTA_STATE_ABORTED; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); target_state = EOTA_STATE_UNTRACKED; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); target_state = EOTA_STATE_UNTRACKED;
    image_result[1] = EOTA_UPDATE_RESOURCE_FAILURE; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); change_during_hash = true; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);
    reset(); image_seed[0] = 0; expect_uncertain(ESP_BASE_OTA_FIRMWARE_CONFIRMED);

    reset();
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, NULL, &set) ==
        ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT);
    assert(memcmp(&set, &empty, sizeof set) == 0);
    eota_prepared_t prepared = prepared_receipt();
    memset(&set, 0xff, sizeof set);
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, &prepared, &set) ==
        ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT);
    assert(memcmp(&set, &empty, sizeof set) == 0);
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, &prepared, &set) ==
        ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.bootable_count == 2 && set.running_firmware_sha256[0] == 0xa0 &&
           set.bootable_firmware_sha256[1][0] == 0xb0 &&
           observe_calls == 2 && verify_calls == 2 && rollback_calls == 0);

    reset(); running_subtype = boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    prepared = prepared_receipt();
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, &prepared, &set) ==
        ESP_BASE_OTA_FIRMWARE_OK);
    assert(set.running_firmware_sha256[0] == 0xb0 &&
           set.bootable_firmware_sha256[1][0] == 0xa0);

    reset(); prepared = prepared_receipt(); prepared.image_size_bytes = 0;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); --prepared.image_size_bytes;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); memset(prepared.sha256, 0, sizeof prepared.sha256);
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); prepared.sha256[0] ^= 1U;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); prepared.slots.target_address_bytes += 0x1000;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); prepared.slots.running_state = EOTA_STATE_PENDING_VERIFY;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); prepared.slots.target_state = EOTA_STATE_NEW;
    expect_prepared_uncertain(&prepared);

    const eota_state_t rejected_target_states[] = {
        EOTA_STATE_VALID, EOTA_STATE_NEW, EOTA_STATE_PENDING_VERIFY,
        EOTA_STATE_UNDEFINED, EOTA_STATE_OTHER,
    };
    for (size_t index = 0; index < sizeof rejected_target_states / sizeof rejected_target_states[0]; ++index) {
        reset(); prepared = prepared_receipt(); target_state = rejected_target_states[index];
        expect_prepared_uncertain(&prepared);
    }
    reset(); prepared = prepared_receipt(); target_state = EOTA_STATE_INVALID;
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, &prepared, &set) ==
        ESP_BASE_OTA_FIRMWARE_OK);
    reset(); prepared = prepared_receipt(); target_state = EOTA_STATE_ABORTED;
    assert(esp_base_ota_observe_firmware_set(
        ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE, &prepared, &set) ==
        ESP_BASE_OTA_FIRMWARE_OK);

    reset(); prepared = prepared_receipt(); running_state = EOTA_STATE_PENDING_VERIFY;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); boot_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); image_result[0] = EOTA_UPDATE_IMAGE_INVALID;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); image_result[1] = EOTA_UPDATE_IMAGE_INVALID;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); image_seed[1] = image_seed[0];
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); image_chip_id[1] = 0x0009;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); strcpy(image_project[1], "other_product");
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); partitions[1].address += 0x1000;
    expect_prepared_uncertain(&prepared);
    reset(); prepared = prepared_receipt(); change_during_hash = true;
    expect_prepared_uncertain(&prepared);

    puts("  ota_firmware passed (confirmed, trial and prepared A/C signed observation; failure rejection)");
}
