idf_component_register(SRCS "esp_base_container_binding.c" "esp_base_container_no_package.c" "esp_base_container_product.c"
    INCLUDE_DIRS "include"
    REQUIRES ota_operation esp_container nvs_flash pthread esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
