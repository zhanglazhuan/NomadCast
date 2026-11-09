# CST836U GitHub driver comparison

Latest test directly compiles the local reference module under
refer/read-pico-reader-main/components/cst836u without source/header changes.
The earlier implementations below are historical. See README.md for current
integration and test-results/cst836u_08_direct_refer.log for the original module's
chip-info initialization failure on COM12.

Sources inspected:
- VESC pinned commit: https://github.com/vedderb/vesc_express/blob/33631150a271754122be88a2ec7c58ad3ca0ceea/main/touch/touch_cst836u.c
- VESC bus wrapper: https://github.com/vedderb/vesc_express/blob/33631150a271754122be88a2ec7c58ad3ca0ceea/main/drivers/hwi2c.c
- VESC setup: https://github.com/vedderb/vesc_express/blob/33631150a271754122be88a2ec7c58ad3ca0ceea/main/touch/lispif_touch_extensions.c
- Arduino: https://github.com/noerbany/CST836U_library/blob/main/cst836u.cpp
- MindReset: https://github.com/MindReset/read_pico_firmware/blob/main/components/cst836u/cst836u.c

| Item | Original test / Arduino | VESC | MindReset |
| --- | --- | --- | --- |
| 7-bit address | 0x15 | 0x15 | 0x15 |
| Point read | 0x00, 7 bytes | 0x02, 5 bytes | 0x00, 15 bytes, two point records |
| Transaction | Repeated START | Same ESP-IDF transmit_receive API | Separate write and read, STOP + 5 us delay |
| Normal mode command | None | None in init/read path | FE 00 in wake/set-mode path |
| Info | Not read | Not read | 0xA6, 6 bytes |
| Reset | Initially 10/50 ms, then extended diagnostic | LOW 10 ms, HIGH 50 ms | Same pulse/boot timings |
| IRQ | Counted only, polling continues | Load function forces touch_has_int=false | Input, active LOW |
| Coordinate processing | Raw diagnostic, initially one point only | First point, track ID, transforms and median filter | Two points, event/ID validation |

0x02/5 and 0x00/7 address the same five point bytes; this is a shorter read,
not evidence that the previous register map was wrong. Address ACK occurs before
register selection, so changing the read window cannot by itself explain an
address probe failure. Coordinate filtering cannot repair an I2C failure.

Current test independently implements VESC's 0x02/5 window (400 kHz, 20 ms polling).
It retains raw unfiltered coordinates for diagnostics, parses event/ID, and does
not use IRQ activity to gate reads. It reports the first point only (count up to 2).
Startup compares the three transports plus info/normal mode; no probe/scan precedes
the first VESC transfer. I2C controller reset separates comparison attempts.
Only documented protocol facts were used; VESC GPL source was not copied.

## read-pico-reader comparison (2026-11-07)

Latest test now uses this transport continuously: 0x00/15, STOP + 5 us,
100 ms timeout, up to three attempts for malformed contact counts. Startup
comparisons/software I2C are disabled by default (CST836U_COMPARE_TRANSPORTS=0).
The diagnostic still tracks only the first point, not both reference points.
Earlier descriptions of the default VESC window above describe previous builds.

Inspected commit 7e50d4be94075f7c80e1bc47e51d3fef96b5337a:
https://github.com/wegooo-cell/read-pico-reader/blob/7e50d4be94075f7c80e1bc47e51d3fef96b5337a/components/cst836u/cst836u.c

The CST836U implementation retains MindReset's protocol: register 0x00, 15 bytes,
separate transmit/receive with STOP and 5 us delay, 100 ms timeout; info 0xA6/6;
FE 00 normal and A5 03 deep sleep; reset LOW 10 ms then boot wait 50 ms in wake.
It checks data[2] < 3, retries malformed packets up to three times, and decodes two
six-byte records with event/ID checks. Transport failures return immediately.
Init requires a successful info read; our diagnostic deliberately continues and
logs failures. These differences cannot repair a failed register write.

Its header selects address 0x38 only for KIIKO_BOARD_METALIO47; the ordinary
CST836U address remains 0x15. That board has a separate metalio_ft6336.c driver;
the conditional address is not evidence that CST836U on our board uses 0x38.
Our existing MindReset startup comparisons cover this CST836U transport already.

The one-shot software I2C diagnostic runs before hardware bus creation, to sample
clock/ACK levels independently. Capture cst836u_06_sw_diag.log shows clock-high
timeouts and failed STOPs, with no complete report. Neither this result nor an
idle HIGH reading identifies the physical cause; a valid NACK cannot be concluded
from attempts whose STOP or clock timing failed.
