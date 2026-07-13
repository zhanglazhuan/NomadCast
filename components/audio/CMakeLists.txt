idf_component_register(
  SRCS "audio_bsp.cpp"
  PRIV_REQUIRES espressif__esp_codec_dev codec_board
  INCLUDE_DIRS "./")
