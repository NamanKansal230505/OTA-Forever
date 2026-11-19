idf_component_register(
    SRCS "main.c" "config.c" "wifi.c" "portal.c" "dns.c" "updater.c" "launcher.c" "button.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "portal.html"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client esp_http_server
             app_update bootloader_support esp_app_format esp_timer mbedtls json driver)
