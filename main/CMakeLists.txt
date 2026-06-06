idf_component_register(SRCS "main.c" "dns_server.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_https_ota esp_http_client esp_netif esp_wifi esp_driver_gpio esp_timer nvs_flash esp_http_server esp_event lwip esp-tls)
