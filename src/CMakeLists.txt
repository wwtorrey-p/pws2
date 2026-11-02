file(GLOB_RECURSE SRCS *.cpp)
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS .
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_https_server
             app_update esp_app_format esp_partition json mbedtls driver esp_timer lwip espressif__led_strip
)
