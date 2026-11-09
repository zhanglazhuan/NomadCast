# CST836U test results — 2026-11-07

COM12, ESP32-S3 MAC 50:78:7d:db:1b:58, ESP-IDF 5.5.3.
Protocol reference: https://github.com/noerbany/CST836U_library/blob/main/cst836u.cpp

Both versions compiled and flashed successfully, with flash hash verification.
The previous CST726 logs in this folder used the wrong protocol and are historical.

| Capture | Setup | Observations |
| --- | --- | --- |
| cst836u_01.log | 400 kHz, reset LOW >=10 ms / HIGH >=50 ms | Probe 0x15 failed; at 30 seconds reads=0, errors=3002, IRQ_edges=0. Capture ended early around 34 seconds due to USB serial ClearCommError. User interaction cannot be fully correlated with this capture. |
| cst836u_02_reconnect.log | Reconnected without issuing reset | Reads=0, errors=5491, IRQ_edges=5 at firmware uptime 55 seconds. |
| cst836u_03_100k.log | 100 kHz, reset LOW >=110 ms / HIGH >=100 ms | Probe 0x15 failed; at 20 seconds reads=0, errors=2002, IRQ_edges=17513, INT=0. No PRESS/MOVE/RELEASE reports. |

Result: NOT PASSED. Correct register transaction and packet decoding are implemented,
but there are no successful report reads on the connected board. Interrupt activity
alone is not proof of a valid touch report or of the cause of the I2C failures.
Runtime errors: ESP_ERR_INVALID_STATE; no fabricated release on communication errors.

Current flashed binary: 100 kHz diagnostic, SHA256
F46A6FE1B19210CAD475BCDEB9169725DDA7FD3C017D7F4028A012D78A5FC499.
Backlight remains off; production driver/system integration is not changed.

## Follow-up: VESC / MindReset comparison

cst836u_04_reference_compare.log: all three reads, info read and FE 00 command
failed; bus-busy errors occurred after the startup probe. This round alone does
not isolate the effects of that probe.

cst836u_05_noprobe.log: removed probe entirely, first transaction is VESC 02/5.
Controller reset separates subsequent comparisons. All variants still return
ESP_ERR_INVALID_STATE; the separate MindReset transfers fail at register write,
before any read or coordinate decoding. FE 00 also fails. At 10 seconds:
reads=0, errors=502, presses=0, releases=0, IRQ_edges=0. No new touch interaction
was requested during these two captures; these validate communication only.

Latest flashed setup: 400 kHz, VESC 02/5, 20 ms polling, startup comparisons.
Build/flash passed, communication did not. Earlier 100 kHz binary above is historical.
See ../REFERENCE_COMPARISON.md for exact source links and differences.

## Software I2C isolation / read-pico-reader review

Build and COM12 flash passed, including flash hash verification. Current binary
SHA256: 48C0725D8079B5E4A1D52AA109E1040E750D3AB8DDB0FF21E9DCC005B6A5C59C.
Capture: cst836u_06_sw_diag.log (15 seconds, reset at capture start).
The software diagnostic runs before hardware bus creation. Initial SDA/SCL=1/1
and recovery STOP succeeded, but address attempts subsequently showed SCL=0,
clock-high timeouts and STOP failures. No complete software read occurred.
This is not a clean address-NACK test and does not establish a wiring fault or
identify which device/condition holds the clock low. Hardware polling also failed:
at 10 seconds reads=0, errors=502, presses=0, releases=0, IRQ_edges=0.
No new touch interaction was requested during this capture.

read-pico-reader commit 7e50d4b uses the same MindReset CST836U register transport
already included in our comparisons. See ../REFERENCE_COMPARISON.md for details.
Communication remains NOT PASSED; no production integration made.

## Continuous read-pico-reader transport

Built/flashed successfully with flash hash verification. Capture:
cst836u_07_reader_continuous.log. Default now performs only 0x00/15 via separate
write, STOP, 5 us delay, read; timeout 100 ms. Malformed counts retry up to three
times. Startup software I2C, reference comparisons, controller resets and FE 00
command are disabled, so this round isolates repeated point reads after reset.
At 10 seconds: reads=0, errors=502, invalid=0, presses=0, releases=0, IRQ_edges=0.
No interaction requested; communication remains NOT PASSED.
The user's preceding log contained one ESP_OK all-zero VESC startup response;
that isolated result does not demonstrate sustained communication or valid touch.

## Direct unmodified module from refer/ (latest)

User requested direct use of the local reference module. Removed main/cst836u.c
and main/cst836u.h; a local component CMake wrapper directly compiles
refer/read-pico-reader-main/components/cst836u/cst836u.c with its original include
directory. No source/header changes were made to the reference module. The build
log confirms this absolute reference source path was compiled into libcst836u.a.
Software I2C is excluded. Only the harness handles V1.2 power/reset and logging;
it calls the original init/get_info/read APIs, retaining both point records.

Build passed. First flash attempt failed because COM12 was busy; after the user
closed the monitor, flash and hash verification passed. Binary SHA256:
F5BE80FAA2D60698309EBACE3E94D2F5814D16CEFCD69B0F4C78FFCB98643E1E.
Capture: cst836u_08_direct_refer.log (12 seconds, reset at capture start).
At boot 248 ms the original module logs:
Chip information read failed: ESP_ERR_INVALID_STATE.
cst836u_init returns ESP_ERR_INVALID_STATE; point reads are skipped because the
module returns no initialized handle. Communication NOT PASSED; no touch gesture
validation was requested. This reproduces an initialization failure using the
reference module itself, rather than the previous independently written driver.
