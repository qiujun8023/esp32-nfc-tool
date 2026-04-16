idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        config
        nfc
        storage
        network
    PRIV_REQUIRES
        nvs_flash
)
