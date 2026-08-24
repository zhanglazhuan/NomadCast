idf_component_register(SRCS "t_play.c"
                       INCLUDE_DIRS "."
                       REQUIRES audio_pipeline audio_stream esp-adf-libs
                                 board_leisound driver)
