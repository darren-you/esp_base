#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
case "${ESP_BASE_TEST_TARGET:-esp32c3}" in
  esp32c3) TARGET_DEFINE=CONFIG_IDF_TARGET_ESP32C3 ;;
  esp32) TARGET_DEFINE=CONFIG_IDF_TARGET_ESP32 ;;
  *) printf 'esp-base host tests\n  error  ESP_BASE_TEST_TARGET must be esp32c3 or esp32.\n' >&2; exit 2 ;;
esac
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf -- "$BUILD_DIR"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/components/device_protocol/include" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/tests/command_guard_test.c" -o "$BUILD_DIR/command_guard_test"
"$BUILD_DIR/command_guard_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/network_auth" -I "$ROOT/components/device_protocol/include" \
  "$ROOT/components/device_protocol/network_auth.c" \
  "$ROOT/components/device_protocol/mqtt_command.c" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/tests/network_auth_test.c" \
  -o "$BUILD_DIR/network_auth_test"
"$BUILD_DIR/network_auth_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/components/device_protocol/include" -I "$ROOT/components/remote_config/include" \
  "$ROOT/components/device_protocol/frp_status_listener.c" \
  "$ROOT/tests/frp_status_listener_test.c" -o "$BUILD_DIR/frp_status_listener_test"
"$BUILD_DIR/frp_status_listener_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/device_protocol/include" \
  -I "$ROOT/components/remote_config/include" \
  -I "$ROOT/managed_components/mqtt/runtime/include" \
  "$ROOT/components/device_protocol/mqtt_owner.c" \
  "$ROOT/components/device_protocol/mqtt_command.c" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/managed_components/mqtt/runtime/emqtt_contract.c" \
  "$ROOT/tests/mqtt_owner_test.c" -o "$BUILD_DIR/mqtt_owner_test"
"$BUILD_DIR/mqtt_owner_test"
CJSON_DIR="$ROOT/managed_components/espressif__cjson/cJSON"
EOTA_DIR="$ROOT/managed_components/esp_ota"
EFRP_DIR="$ROOT/managed_components/esp_frp"
if [[ ! -f "$CJSON_DIR/cJSON.c" || ! -f "$EOTA_DIR/include/eota.h" || ! -f "$EFRP_DIR/include/esp_frp.h" ]]; then
  printf 'esp-base host tests\n  error  Run idf.py -C firmware reconfigure to resolve the locked cJSON and esp-ota dependencies.\n' >&2
  exit 1
fi
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/device_protocol/include" \
  -I "$ROOT/components/remote_config/include" -I "$ROOT/components/time_runtime/include" \
  -I "$EFRP_DIR/include" \
  "$ROOT/components/device_protocol/frp_owner.c" "$ROOT/tests/frp_owner_test.c" \
  -o "$BUILD_DIR/frp_owner_test"
"$BUILD_DIR/frp_owner_test"
# Keep strict diagnostics on our sources; the locked third-party cJSON uses
# sprintf internally, which the macOS SDK marks deprecated.
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -Wno-deprecated-declarations \
  -I "$CJSON_DIR" -c "$CJSON_DIR/cJSON.c" -o "$BUILD_DIR/cJSON.o"
"${CC:-cc}" -std=c11 -D"$TARGET_DEFINE"=1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/device_protocol/include" -I "$ROOT/components/remote_config/include" \
  -I "$ROOT/components/ota_operation/include" -I "$EOTA_DIR/include" -I "$CJSON_DIR" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/components/device_protocol/command_decoder.c" "$ROOT/components/remote_config/config_codec.c" "$BUILD_DIR/cJSON.o" \
  "$ROOT/tests/command_decoder_test.c" -lm -o "$BUILD_DIR/command_decoder_test"
printf 'esp-base host tests\n  command_guard  passed\n'
"$BUILD_DIR/command_decoder_test"
printf '  hardware       not used\n'
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/remote_config/include" \
  "$ROOT/components/remote_config/config_codec.c" \
  "$ROOT/components/remote_config/esp_base_remote_config.c" \
  "$ROOT/tests/config_store_test.c" -o "$BUILD_DIR/config_store_test"
"$BUILD_DIR/config_store_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D"$TARGET_DEFINE"=1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/ota_update" -I "$ROOT/tests/fakes" -I "$ROOT/components/ota_operation/include" -I "$EOTA_DIR/include" \
  "$ROOT/components/ota_operation/esp_base_ota_policy.c" "$ROOT/components/ota_operation/esp_base_ota_receipt.c" \
  "$ROOT/tests/ota_receipt_test.c" \
  -o "$BUILD_DIR/ota_receipt_test"
"$BUILD_DIR/ota_receipt_test"
"${CC:-cc}" -std=c11 -D"$TARGET_DEFINE"=1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/ota_firmware" -I "$ROOT/tests/fakes/ota_update" -I "$ROOT/tests/fakes" \
  -I "$ROOT/components/ota_operation/include" -I "$EOTA_DIR/include" \
  "$ROOT/components/ota_operation/esp_base_ota_policy.c" \
  "$ROOT/components/ota_operation/esp_base_ota_firmware.c" \
  "$ROOT/tests/ota_firmware_test.c" -o "$BUILD_DIR/ota_firmware_test"
"$BUILD_DIR/ota_firmware_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
  -I "$ROOT/components/ota_operation/include" \
  "$ROOT/components/ota_operation/esp_base_storage_owner.c" \
  "$ROOT/tests/storage_owner_test.c" -o "$BUILD_DIR/storage_owner_test"
"$BUILD_DIR/storage_owner_test"
if [[ "$(uname -s)" == Darwin ]]; then
  PROTOCOL_LINK_GC=(-Wl,-dead_strip)
else
  PROTOCOL_LINK_GC=(-Wl,--gc-sections)
fi
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D"$TARGET_DEFINE"=1 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -ffunction-sections -fdata-sections "${PROTOCOL_LINK_GC[@]}" \
  -I "$ROOT/tests/fakes/protocol-path" -I "$ROOT/tests/fakes/ota_update" -I "$ROOT/tests/fakes" \
  -I "$ROOT/components/device_protocol/include" -I "$ROOT/components/device_protocol" \
  -I "$ROOT/components/device_identity/include" -I "$ROOT/components/remote_config/include" \
  -I "$ROOT/components/wifi_runtime/include" -I "$ROOT/components/time_runtime/include" \
  -I "$ROOT/components/ota_operation/include" -I "$EOTA_DIR/include" \
  "$ROOT/components/device_protocol/command_guard.c" \
  "$ROOT/components/device_protocol/control_state.c" \
  "$ROOT/components/ota_operation/esp_base_storage_owner.c" \
  "$ROOT/tests/protocol_ota_owner_test.c" -o "$BUILD_DIR/protocol_ota_owner_test"
"$BUILD_DIR/protocol_ota_owner_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/ota_update" -I "$ROOT/tests/fakes" \
  -I "$ROOT/tests/fakes/container_binding" \
  -I "$ROOT/components/ota_operation/include" \
  -I "$ROOT/integrations/container_binding/include" -I "$EOTA_DIR/include" \
  "$ROOT/components/ota_operation/esp_base_storage_owner.c" \
  "$ROOT/integrations/container_binding/esp_base_container_binding.c" \
  "$ROOT/tests/container_binding_test.c" -o "$BUILD_DIR/container_binding_test"
"$BUILD_DIR/container_binding_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D"$TARGET_DEFINE"=1 \
  -Wall -Wextra -Werror -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections "${PROTOCOL_LINK_GC[@]}" \
  -I "$ROOT/tests/fakes/container_product" -I "$ROOT/tests/fakes" \
  -I "$ROOT/integrations/container_binding" \
  -I "$ROOT/integrations/container_binding/include" \
  -I "$ROOT/components/ota_operation/include" \
  -I "$EOTA_DIR/include" \
  -I "$ROOT/managed_components/esp_container/include" \
  "$ROOT/components/ota_operation/esp_base_storage_owner.c" \
  "$ROOT/integrations/container_binding/esp_base_container_binding.c" \
  "$ROOT/tests/container_product_retire_test.c" \
  -o "$BUILD_DIR/container_product_retire_test"
"$BUILD_DIR/container_product_retire_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/integrations/container_binding" \
  -I "$ROOT/managed_components/esp_container/include" \
  "$ROOT/integrations/container_binding/esp_base_container_no_package.c" \
  "$ROOT/tests/container_no_package_test.c" -o "$BUILD_DIR/container_no_package_test"
"$BUILD_DIR/container_no_package_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/device_protocol" \
  "$ROOT/components/device_protocol/control_state.c" "$ROOT/tests/control_state_test.c" \
  -o "$BUILD_DIR/control_state_test"
"$BUILD_DIR/control_state_test"
"${CC:-cc}" -std=c11 -D"$TARGET_DEFINE"=1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/app_main" -I "$ROOT/tests/fakes" \
  -I "$ROOT/components/device_identity/include" -I "$ROOT/components/device_protocol/include" \
  -I "$EOTA_DIR/include" -I "$ROOT/components/remote_config/include" \
  -I "$ROOT/components/safety_runtime/include" -I "$ROOT/components/time_runtime/include" \
  -I "$ROOT/components/ota_operation/include" \
  "$ROOT/components/ota_operation/esp_base_storage_owner.c" \
  "$ROOT/apps/esp_base/main/esp_base_main.c" \
  "$ROOT/tests/ota_startup_test.c" -o "$BUILD_DIR/ota_startup_test"
"$BUILD_DIR/ota_startup_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes" -I "$ROOT/components/time_runtime/include" \
  "$ROOT/components/time_runtime/esp_base_time.c" "$ROOT/tests/time_runtime_test.c" \
  -o "$BUILD_DIR/time_runtime_test"
"$BUILD_DIR/time_runtime_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$ROOT/tests/fakes/wifi_runtime" -I "$ROOT/tests/fakes" \
  -I "$ROOT/components/wifi_runtime/include" -I "$ROOT/components/remote_config/include" \
  "$ROOT/components/wifi_runtime/esp_base_wifi.c" "$ROOT/components/remote_config/config_codec.c" \
  "$ROOT/tests/wifi_startup_test.c" -o "$BUILD_DIR/wifi_startup_test"
for stage in {0..11}; do "$BUILD_DIR/wifi_startup_test" "$stage"; done
printf '  wifi_startup passed (SDK init faults preserve failed state and release resources)\n'
