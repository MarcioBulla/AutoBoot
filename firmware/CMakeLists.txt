cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(autoboot)

target_sources(app PRIVATE src/main.c src/certificate.c src/wifi_credentials.c
                           src/usb_maintenance.c src/mqtt_client.c)
target_link_libraries(app PRIVATE mbedTLS)

# Avoid Newlib locale-dependent ROM comparisons with Picolibc on ESP32-S2.
if(CONFIG_SOC_ESP32S2 AND CONFIG_PICOLIBC)
  target_sources(app PRIVATE src/esp32s2_string_compat.c)
  target_link_options(zephyr_interface INTERFACE
    -Wl,--wrap=strncasecmp -Wl,--wrap=strcasecmp)
endif()

# Zephyr 4.5's ESP32-S2 DWC2 UDC needs the ESP-IDF USB wrapper HAL.  Keep this
# compatibility glue in the application so a west update is never modified.
if(CONFIG_UDC_DWC2)
  set(AUTOBOOT_ESP_HAL_DIR "${WEST_TOPDIR}/modules/hal/espressif")
  zephyr_include_directories(
    "${AUTOBOOT_ESP_HAL_DIR}/components/esp_hal_usb/include"
    "${AUTOBOOT_ESP_HAL_DIR}/components/esp_hal_usb/esp32s2/include"
    "${AUTOBOOT_ESP_HAL_DIR}/components/usb/include"
  )
  target_sources(app PRIVATE
    "${AUTOBOOT_ESP_HAL_DIR}/components/esp_hal_usb/usb_wrap_hal.c"
  )
endif()
