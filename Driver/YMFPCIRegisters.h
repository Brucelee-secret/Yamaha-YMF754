/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) by Jaroslav Kysela.
 * YMF724/740/744/754 register offsets distilled from the supplied ALSA header.
 */

#ifndef YMFPCI_REGISTERS_H
#define YMFPCI_REGISTERS_H

enum {
    YMF_REG_INTFLAG             = 0x0004,
    YMF_REG_ACTIVITY            = 0x0006,
    YMF_REG_GLOBALCTRL          = 0x0008,
    YMF_REG_TIMERCTRL           = 0x0010,
    YMF_REG_TIMERCOUNT          = 0x0012,
    YMF_REG_SPDIFOUTCTRL        = 0x0018,
    YMF_REG_SPDIFOUTSTATUS      = 0x001C,
    YMF_SPDIF_DEFAULT_STATUS    = 0x8200,
    YMF_REG_SPDIFINCTRL         = 0x0034,
    YMF_REG_SPDIFINSTATUS       = 0x0038,
    YMF_REG_DSPPROGRAMDL        = 0x0048,
    YMF_REG_DLCNTRL             = 0x004C,
    YMF_REG_AC97CMDDATA         = 0x0060,
    YMF_REG_AC97CMDADR          = 0x0062,
    YMF_REG_PRISTATUSDATA       = 0x0064,
    YMF_REG_PRISTATUSADR        = 0x0066,
    YMF_REG_SECSTATUSDATA       = 0x0068,
    YMF_REG_SECSTATUSADR        = 0x006A,
    YMF_REG_SECCONFIG           = 0x0070,
    YMF_REG_LEGACYOUTVOL        = 0x0080,
    YMF_REG_NATIVEDACOUTVOL     = 0x0084,
    YMF_REG_ZVOUTVOL            = 0x0088,
    YMF_REG_SECADCOUTVOL        = 0x008C,
    YMF_REG_PRIADCOUTVOL        = 0x0090,
    YMF_REG_NATIVEDACLOOPVOL    = 0x0098,
    YMF_REG_ZVLOOPVOL           = 0x009C,
    YMF_REG_SECADCLOOPVOL       = 0x00A0,
    YMF_REG_PRIADCLOOPVOL       = 0x00A4,
    YMF_REG_NATIVEADCINVOL      = 0x00A8,
    YMF_REG_NATIVEDACINVOL      = 0x00AC,
    YMF_REG_BUF441OUTVOL        = 0x00B0,
    YMF_REG_SPDIFOUTVOL         = 0x00B8,
    YMF_REG_ADCSLOTSR           = 0x00C0,
    YMF_REG_RECSLOTSR           = 0x00C4,
    YMF_REG_ADCFORMAT           = 0x00C8,
    YMF_REG_RECFORMAT           = 0x00CC,
    YMF_REG_STATUS              = 0x0100,
    YMF_REG_CTRLSELECT          = 0x0104,
    YMF_REG_MODE                = 0x0108,
    YMF_REG_SAMPLECOUNT         = 0x010C,
    YMF_REG_NUMOFSAMPLES        = 0x0110,
    YMF_REG_CONFIG              = 0x0114,
    YMF_REG_PLAYCTRLSIZE        = 0x0140,
    YMF_REG_RECCTRLSIZE         = 0x0144,
    YMF_REG_EFFCTRLSIZE         = 0x0148,
    YMF_REG_WORKSIZE            = 0x014C,
    YMF_REG_MAPOFREC            = 0x0150,
    YMF_REG_MAPOFEFFECT         = 0x0154,
    YMF_REG_PLAYCTRLBASE        = 0x0158,
    YMF_REG_RECCTRLBASE         = 0x015C,
    YMF_REG_EFFCTRLBASE         = 0x0160,
    YMF_REG_WORKBASE            = 0x0164,
    YMF_REG_DSPINSTRAM          = 0x1000,
    YMF_REG_CTRLINSTRAM         = 0x4000,
    YMF_PCI_DSXG_CTRL           = 0x48,
    YMF_PCI_PWRCTRL1            = 0x4A,
    YMF_PCI_PWRCTRL2            = 0x4E,
    YMF_DSP_LENGTH              = 0x0080,
    YMF_CTRL_LENGTH             = 0x3000,
    YMF_MMIO_LENGTH             = 0x8000
};

#endif /* YMFPCI_REGISTERS_H */
