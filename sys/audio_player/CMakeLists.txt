idf_component_register(
    SRCS "audio_player.c"
    INCLUDE_DIRS "."
                "../../vendor/esp-adf/components/audio_board/include"
    REQUIRES audio_pipeline audio_stream esp-adf-libs mbedtls es8156 events flash_store driver
)
