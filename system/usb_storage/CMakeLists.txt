idf_component_register(SRCS "usb_storage.c" "sd_access_guard.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio PRIV_REQUIRES esp_driver_sdmmc sdmmc fatfs esp_tinyusb esp_timer freertos)
# All app/ADF stdio and POSIX SD accesses pass through these entry points.
foreach(func _open_r _close_r opendir closedir _stat_r _unlink_r _rename_r _link_r mkdir rmdir access truncate utime)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${func}")
endforeach()
