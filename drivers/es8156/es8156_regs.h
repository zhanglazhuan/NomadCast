// SPDX-License-Identifier: MIT
// Register map for Everest ES8156 audio DAC
#pragma once

// Page 0 registers
#define ES8156_REG_RESET_CONTROL          0x00
#define ES8156_REG_MAIN_CLOCK_CONTROL     0x01
#define ES8156_REG_MODE_CONFIG_1          0x02
#define ES8156_REG_MASTER_LRCK_DIVIDER_1  0x03
#define ES8156_REG_MASTER_LRCK_DIVIDER_0  0x04
#define ES8156_REG_MASTER_CLOCK_CONTROL   0x05
#define ES8156_REG_NFS_CONFIG             0x06
#define ES8156_REG_MISC_CONTROL_1         0x07
#define ES8156_REG_CLOCK_OFF              0x08
#define ES8156_REG_MISC_CONTROL_2         0x09
#define ES8156_REG_TIME_CONTROL_1         0x0A
#define ES8156_REG_TIME_CONTROL_2         0x0B
#define ES8156_REG_CHIP_STATUS            0x0C
#define ES8156_REG_P2S_CONTROL            0x0D
#define ES8156_REG_DAC_COUNTER_PARAMETER  0x10
#define ES8156_REG_SDP_INTERFACE_CONFIG_1 0x11
#define ES8156_REG_AUTOMUTE_CONTROL       0x12
#define ES8156_REG_MUTE_CONTROL           0x13
#define ES8156_REG_VOLUME_CONTROL         0x14
#define ES8156_REG_ALC_CONFIG_1           0x15
#define ES8156_REG_ALC_CONFIG_2           0x16
#define ES8156_REG_ALC_LEVEL              0x17
#define ES8156_REG_MISC_CONTROL_3         0x18
#define ES8156_REG_EQ_CONTROL_1           0x19
#define ES8156_REG_EQ_CONFIG_2            0x1A
#define ES8156_REG_ANALOG_SYSTEM_1        0x20
#define ES8156_REG_ANALOG_SYSTEM_2        0x21
#define ES8156_REG_ANALOG_SYSTEM_3        0x22
#define ES8156_REG_ANALOG_SYSTEM_4        0x23
#define ES8156_REG_ANALOG_SYSTEM_5        0x24
#define ES8156_REG_ANALOG_SYSTEM_6        0x25
#define ES8156_REG_PAGE_SELECT            0xFC
#define ES8156_REG_CHIP_ID1               0xFD
#define ES8156_REG_CHIP_ID0               0xFE
#define ES8156_REG_CHIP_VERSION           0xFF
