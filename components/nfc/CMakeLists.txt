idf_component_register(
    SRCS
        "src/pn532.c"
        "src/mifare_classic.c"
        "src/ntag.c"
        "src/ndef.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        config
        driver
        esp_driver_spi
)
