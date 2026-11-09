# Debug projects — PCB V1.2

Board pins come from `boards/esp32s3/nomadcast.h` for schematic revision V1.2. The obsolete V1 header was removed.
Do not copy pin numbers into individual tests.

| Peripheral | V1.2 pins |
| --- | --- |
| Keys: Vol-/Vol+/Stop/Power | 6 / 4 / 7 / 5 |
| Battery sense / charging | GPIO3, ADC1_CH2 / GPIO8 (LOW=charging) |
| USB detect / USB wake | GPIO18 (LOW=inserted) / GPIO9 (HIGH=wake, ADC1_CH8) |
| SD CLK/CMD/DAT0 | 2 / 40 / 1 |
| Audio MCLK/BCLK/LRCLK/DOUT | 13 / 12 / 10 / 11 |
| Speaker amplifier / headphone detect | 17 / 14 |
| Peripheral power / LCD power / backlight | 46 / 21 / 39 |
| LCD CS/DC/RST/SCK/MOSI | 48 / 47 / 43 / 45 / 38; MISO unused |
| Shared I2C SDA/SCL; touch RST/INT | 41 / 42; 43 / 44 |

`t_key` uses digital, active-LOW USB detection; the old GPIO3 USB ADC diagnostic
was removed because GPIO3 is now the battery voltage input.
`t_wakeup` tests the independent active-HIGH USB wake input on GPIO9.

`t_touch`, `t_display_touch`, and `t_ui` still contain legacy GT911 protocols.
Their wiring follows V1.2, but changing pins does not make them CST836U drivers.
Use `t_touch_CST836U` for CST836U diagnostics; see its recorded limitations.

`t_hello`, `t_ota`, and `t_podcast` have no board GPIO definitions to migrate.
Vendored STM32 examples under `t_touch/H7_GT911_I2C_Touch-master` are reference
material and are not part of the ESP32 test firmware.

Speaker output has been confirmed audible on V1.2 with `t_speaker`.
Other pin migrations require hardware validation; compilation alone does not
establish that the corresponding peripheral works.

Build verification (2026-11-07, ESP-IDF 5.5.3): production firmware, t_key,
t_battery, t_wakeup, t_ui, t_wifi, t_usb_msc, t_display_touch, t_touch,
t_display, and t_touch_CST836U passed. t_TDP DOWNLOAD mode passed with
IDF_CCACHE_ENABLE=0 after a GCC internal compiler error; SCREEN/AUDIO modes
were not compiled in this check. t_flashdb could not configure because
vendor/FlashDB/inc is missing. t_play failed with the ADF component dependency
resolver error for esp_peripherals. These two projects have updated pin
references but have not passed compilation.
