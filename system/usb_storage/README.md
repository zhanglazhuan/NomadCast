# Runtime SD card USB storage (PCB V1.2)

GPIO18 detects USB insertion, active low, debounced for 150 ms.
Settings / Storage shows the USB storage switch only while USB is connected.
The reminder appears below the switch; there is no connection-status label.
A charger can assert insertion too; enumeration needs a data cable and host.

The switch changes ownership without rebooting. Enabling stops audio, pauses
downloads (preserving the user's previous pause setting), and closes logging.
The SD access guard blocks new local path operations and counts existing file
and directory handles. The main loop waits for every handle to close before
unmounting FatFS and exposing raw SDMMC sectors through TinyUSB. If handles
cannot drain within five seconds, entry fails and local operation is restored.
Other filesystems, console and socket descriptors are unaffected by the guard.

Disabling or unplugging detaches the USB device, drains deferred sector writes,
uninstalls MSC, and remounts /sdcard before reopening local SD access and
resuming services. Mount failures keep local SD access blocked. Filesystems
are never formatted; automatic TinyUSB local remounting is disabled.
All transitions run on the LVGL thread via usb_storage_process().

The guard wraps the newlib file entry points and POSIX path/directory APIs at
link time. Direct FatFS calls bypass it; application code must not add direct
FatFS access outside the LVGL thread, and must check local availability first.
Open file duplication (dup/F_DUPFD) is not supported for SD handles.

Hardware checks: unplugged visibility; plug/unplug on the page; no SD; active
playback/downloads; computer enumeration; bidirectional file-copy hash checks;
safe eject then switch off; unplug during transfer; repeated switching; page
navigation; restoration of logs/downloads. The internal USB PHY is shared
with the USB Serial/JTAG console, which can disconnect during MSC operation.
