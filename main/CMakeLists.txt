idf_component_register(
    SRCS "main.c"
         "nfc/pn532.c" "nfc/mifare_classic.c" "nfc/ntag.c"
         "nfc/ndef.c" "nfc/default_keys.c" "nfc/nfc_cancel.c"
         "storage/dump_store.c" "storage/keys_store.c" "storage/hex_util.c"
         "net/wifi_ap.c" "net/captive_dns.c" "net/http_server.c"
         "net/api_nfc.c" "net/api_dumps.c" "net/api_keys.c" "net/api_ota.c"
         "net/ws_progress.c"
    INCLUDE_DIRS "." "nfc" "storage" "net"
    PRIV_REQUIRES
        driver esp_driver_spi
        esp_wifi esp_netif esp_event
        esp_http_server esp_timer
        json littlefs mdns
        app_update esp_app_format
        lwip nvs_flash
    EMBED_FILES "html/index.html" "html/app.js" "html/style.css"
)
