# CST836U

Extracted from `refer/read-pico-reader-main/components/cst836u`, the module
compiled by `debug/t_touch_CST836U`. Source and header retain the upstream
Apache-2.0 attribution; protocol code is unchanged.

Default: address 0x15, 400 kHz; register write + STOP + 5 us + read.
The caller owns the hardware I2C bus. Initialization reads chip information
at 0xA6; errors are returned without creating a usable handle.

Production integration: `system/controller/touch_controller.c` polls every
20 ms on the LVGL thread. Board pins come from `nomadcast.h`. Main resets
the shared LCD/TP line before initializing the display; the controller does
not reset that line afterward. Screen sleep disables LVGL polling without
putting the chip into deep sleep, so wake does not reset the display.

The checked-in test capture reports an initialization failure. Integration
build success does not replace hardware validation on the new screen assembly.
