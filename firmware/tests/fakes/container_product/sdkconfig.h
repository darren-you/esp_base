#pragma once
#include <stdbool.h>

extern bool test_policy_enabled;

#define CONFIG_ESP_BASE_CONTAINER_PRODUCT_ID (test_policy_enabled ? "product" : "")
#define CONFIG_ESP_BASE_CONTAINER_KEY_ID (test_policy_enabled ? "key" : "")
#define CONFIG_ESP_BASE_CONTAINER_PUBLIC_KEY_DER_HEX (test_policy_enabled ? "0102" : "")
#define CONFIG_ESP_BASE_CONTAINER_PACKAGE_LABEL (test_policy_enabled ? "pkgs" : "")
#define CONFIG_ESP_BASE_CONTAINER_PACKAGE_OFFSET (test_policy_enabled ? 0x100000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_PACKAGE_SIZE (test_policy_enabled ? 0x300000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_SLOT_0_OFFSET (test_policy_enabled ? 0x100000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_SLOT_0_SIZE (test_policy_enabled ? 0x100000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_SLOT_1_OFFSET (test_policy_enabled ? 0x200000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_SLOT_1_SIZE (test_policy_enabled ? 0x100000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_SLOT_2_OFFSET (test_policy_enabled ? 0x300000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_SLOT_2_SIZE (test_policy_enabled ? 0x100000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_NVS_LABEL (test_policy_enabled ? "pkg_nvs" : "")
#define CONFIG_ESP_BASE_CONTAINER_NVS_OFFSET (test_policy_enabled ? 0x400000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_NVS_SIZE (test_policy_enabled ? 0x10000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_MAX_WASM_BYTES (test_policy_enabled ? 65536 : 0)
#define CONFIG_ESP_BASE_CONTAINER_MAX_STACK_BYTES (test_policy_enabled ? 4096 : 0)
#define CONFIG_ESP_BASE_CONTAINER_MAX_EVENT_QUEUE (test_policy_enabled ? 1 : 0)
#define CONFIG_ESP_BASE_CONTAINER_MAX_INSTRUCTIONS (test_policy_enabled ? 1000 : 0)
#define CONFIG_ESP_BASE_CONTAINER_MAX_HOST_CALL_MS (test_policy_enabled ? 100 : 0)
#define CONFIG_ESP_BASE_CONTAINER_ALLOWED_CAPABILITIES (test_policy_enabled ? 0 : 0)
#define CONFIG_ESP_BASE_CONTAINER_MAX_TIMERS 0
#define CONFIG_ESP_BASE_CONTAINER_MAX_LOG_BYTES 0
#define CONFIG_ESP_BASE_CONTAINER_MAX_ENTRY_MS (test_policy_enabled ? 100 : 0)
#define CONFIG_ESP_BASE_CONTAINER_OWNER_STACK_BYTES (test_policy_enabled ? 8192 : 0)
