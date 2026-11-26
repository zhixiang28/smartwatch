file(GLOB UI_SRCS "*.c" "assets/*.c" "fonts/*.c")
idf_component_register(SRCS ${UI_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES bsp esp_codec_dev esp_http_client json mbedtls lvgl esp_lvgl_port esp_wifi esp_netif nvs_flash bt esp_timer esp_driver_gpio esp_driver_i2c fatfs espcoredump esp_partition esp_app_format helix_mp3 opus esp_websocket_client esp-sr)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-stringop-truncation -Wno-unused-function)
