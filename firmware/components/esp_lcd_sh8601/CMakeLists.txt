if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.5")
    set(PRIV_REQ esp_driver_gpio)
else()
    set(PRIV_REQ driver)
endif()

idf_component_register(SRCS "esp_lcd_sh8601.c" INCLUDE_DIRS "include" PRIV_REQUIRES ${PRIV_REQ} REQUIRES "esp_lcd")


target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP_LCD_SH8601_VER_MAJOR=1 ESP_LCD_SH8601_VER_MINOR=0 ESP_LCD_SH8601_VER_PATCH=0)
