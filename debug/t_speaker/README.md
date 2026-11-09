# Speaker test — PCB V1.2

Uses the active board profile `nomadcast.h` (V1.2).
Speaker path: ES7111 -> HT6872. BCLK=12, LRCLK=10, DOUT=11,
power=46, amplifier enable=17. ES7111 does not require MCLK.
Jack detection GPIO14 is active HIGH; insertion mutes the speaker after 50 ms debounce.
The ES8156 headphone DAC is not initialized in this test.

Verified 2026-11-07: built and flashed on COM12; serial capture showed continuous
I2S transmission, POWER=1, AP_EN=1, HP_DETECT=0 beyond the former 5-second mute.
User confirmed audible speaker output. Jack insertion/removal remains unverified.
