idf_component_register(
    SRCS
        "src/dump_store.c"
        "src/keys_store.c"
        "src/hex_util.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nfc
    PRIV_REQUIRES
        config
        nvs_flash
        littlefs
        json
)
