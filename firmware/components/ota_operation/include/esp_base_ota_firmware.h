// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#include "eota.h"

#define ESP_BASE_OTA_FIRMWARE_LIMIT 2

typedef struct {
    uint8_t bootable_count;
    uint8_t bootable_firmware_sha256[ESP_BASE_OTA_FIRMWARE_LIMIT][EOTA_SHA256_BYTES];
    uint8_t running_firmware_sha256[EOTA_SHA256_BYTES];
} esp_base_ota_firmware_set_t;

typedef enum {
    ESP_BASE_OTA_FIRMWARE_OK,
    ESP_BASE_OTA_FIRMWARE_UNSUPPORTED,
    ESP_BASE_OTA_FIRMWARE_INVALID_ARGUMENT,
    ESP_BASE_OTA_FIRMWARE_UNCERTAIN,
} esp_base_ota_firmware_result_t;

typedef enum {
    ESP_BASE_OTA_FIRMWARE_CONFIRMED,
    ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL,
    ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE,
} esp_base_ota_firmware_observation_t;

/* Observe the complete set of independently bootable signed Base images for
 * this chip in the fixed two-slot layout. CONFIRMED requires a VALID running
 * image. PENDING_TRIAL requires a PENDING_VERIFY running image and an independently
 * VALID rollback image. PREPARED_CANDIDATE is only for the serialized interval
 * after eota_prepare succeeded and before eota_select: pass that exact prepared
 * receipt, require running/selected A VALID, require inactive C unselected and
 * untracked/invalid/aborted in otadata, then verify both signed images and the
 * receipt's exact C length/digest. Other modes require prepared == NULL.
 * These observations supply identities, not package or trial authorization.
 * An ambiguous inactive image or any inconsistent SDK observation yields no set. The
 * caller must serialize all app Flash and otadata writers through the entire
 * call and while using the result; this function neither takes a product lock
 * nor changes Flash, otadata, NVS or eFuse. The first entry is always running.
 * Equal signed images in both slots have one distinct identity. */
esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation,
    const eota_prepared_t *prepared,
    esp_base_ota_firmware_set_t *firmware_set);
