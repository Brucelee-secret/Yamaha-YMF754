/* Host-side tests only. No IOKit, kernel, or real PCI access. */
#include "../Driver/YMFPCIHardware.h"
#include "../Driver/YMFPCIRegisters.h"
#include "../Driver/YMFPCIFirmware.h"
#include <cassert>
#include <iostream>
#include <map>
#include <vector>

struct Fake {
    struct Write {
        unsigned offset;
        YMFDword value;
        unsigned width;
        Write(unsigned o, YMFDword v, unsigned w) : offset(o), value(v), width(w) {}
    };
    std::map<unsigned, YMFDword> regs;
    std::vector<Write> writes;
    std::vector<Write> configWrites;
    bool codecBusy, dspBusy, unavailable, stuckConfig;
    unsigned ms, us, stopBusyIterations;
    Fake() : codecBusy(false), dspBusy(false), unavailable(false),
        stuckConfig(false), ms(0), us(0), stopBusyIterations(0) {}
    static Fake &self(void *p) { return *static_cast<Fake *>(p); }
    static YMFWord r16(void *p, unsigned o) {
        Fake &f = self(p);
        if (f.unavailable) return 0xffff;
        if (o == YMF_REG_PRISTATUSADR && f.codecBusy) return 0x8000;
        return static_cast<YMFWord>(f.regs[o]);
    }
    static YMFDword r32(void *p, unsigned o) {
        Fake &f = self(p);
        if (f.unavailable) return 0xffffffffU;
        if (o == YMF_REG_CONFIG && f.stuckConfig) return 1;
        if (o == YMF_REG_STATUS && f.stopBusyIterations) {
            --f.stopBusyIterations;
            return 2;
        }
        if (o == YMF_REG_STATUS && f.dspBusy) return 2;
        return f.regs[o];
    }
    static void w16(void *p, unsigned o, YMFWord v) {
        self(p).regs[o] = v;
        self(p).writes.push_back(Write(o, v, 16));
    }
    static void w32(void *p, unsigned o, YMFDword v) {
        assert(o <= YMF_MMIO_LENGTH - 4 && (o & 3) == 0);
        self(p).regs[o] = v;
        self(p).writes.push_back(Write(o, v, 32));
    }
    static YMFByte cr8(void *p, unsigned) { return self(p).unavailable ? 0xff : 0xa4; }
    static void cw8(void *p, unsigned o, YMFByte v) {
        self(p).configWrites.push_back(Write(o, v, 8));
    }
    static void cw16(void *p, unsigned o, YMFWord v) {
        self(p).configWrites.push_back(Write(o, v, 16));
    }
    static void delay(void *p, unsigned v) { self(p).us += v; }
    static void sleep(void *p, unsigned v) { self(p).ms += v; }
    YMFPCIHardwareIO io() {
        YMFPCIHardwareIO value = {this, r16, r32, w16, w32, cr8, cw8, cw16, delay, sleep};
        return value;
    }
};

static YMFPCIResult upload(Fake &f, unsigned dspSize = YMF_DSP_LENGTH)
{
    return ymfUploadFirmware(f.io(), gYMFDSPFirmware, dspSize,
        gYMFControllerFirmware, gYMFControllerFirmwareSize);
}

static YMFDword le(const YMFByte *b)
{
    return (YMFDword)b[0] | ((YMFDword)b[1] << 8) |
        ((YMFDword)b[2] << 16) | ((YMFDword)b[3] << 24);
}

int main()
{
    assert(sizeof(YMFWord) == 2 && sizeof(YMFDword) == 4);
    assert(gYMFDSPFirmwareSize == YMF_DSP_LENGTH);
    Fake good;
    good.regs[YMF_REG_CONFIG] = 1; /* Existing DSP state must be stopped first. */
    good.regs[YMF_REG_GLOBALCTRL] = 0x123f;
    assert(upload(good) == YMFPCI_OK);
    assert(good.configWrites.size() == 5);
    assert(good.configWrites[0].offset == YMF_PCI_DSXG_CTRL);
    assert(good.configWrites[0].value == 0xa4);
    assert(good.configWrites[1].value == 0xa7);
    assert(good.configWrites[2].value == 0xa4);
    assert(good.configWrites[3].offset == YMF_PCI_PWRCTRL1);
    assert(good.configWrites[4].offset == YMF_PCI_PWRCTRL2);
    assert(good.regs[YMF_REG_GLOBALCTRL] == 0x1238);
    unsigned dspWrites = 0, controllerWrites = 0;
    for (unsigned i = 0; i < good.writes.size(); ++i) {
        const Fake::Write &w = good.writes[i];
        if (w.offset == YMF_REG_CONFIG) assert(w.value == 0);
        if (w.offset >= YMF_REG_DSPINSTRAM && w.offset < YMF_REG_DSPINSTRAM + YMF_DSP_LENGTH) {
            assert(w.value == le(gYMFDSPFirmware + w.offset - YMF_REG_DSPINSTRAM));
            ++dspWrites;
        }
        if (w.offset >= YMF_REG_CTRLINSTRAM && w.offset < YMF_REG_CTRLINSTRAM + YMF_CTRL_LENGTH) {
            assert(w.value == le(gYMFControllerFirmware + w.offset - YMF_REG_CTRLINSTRAM));
            ++controllerWrites;
        }
    }
    assert(dspWrites == 32 && controllerWrites == 3072);
    assert(good.regs[YMF_REG_CONFIG] == 0 && good.regs[YMF_REG_MODE] == 0);
    assert(good.us == 100 && good.ms == 0);

    /* Normal PCM stop must quiesce the DSP before turning CONFIG off; in
     * particular it must tolerate the device processing several bank ticks. */
    Fake gracefulStop;
    gracefulStop.regs[YMF_REG_CONFIG] = 1;
    gracefulStop.regs[YMF_REG_MODE] = 3;
    gracefulStop.stopBusyIterations = 3;
    assert(ymfStopPCM(gracefulStop.io()) == YMFPCI_OK);
    assert(gracefulStop.regs[YMF_REG_CONFIG] == 1);
    assert(gracefulStop.regs[YMF_REG_MODE] == 0);
    assert(gracefulStop.us == 150);
    assert(ymfStopDSP(gracefulStop.io()) == YMFPCI_OK);
    assert(gracefulStop.regs[YMF_REG_CONFIG] == 0);

    Fake wedgedEngine;
    wedgedEngine.regs[YMF_REG_CONFIG] = 1;
    wedgedEngine.regs[YMF_REG_MODE] = 3;
    wedgedEngine.stopBusyIterations = 401;
    assert(ymfStopPCM(wedgedEngine.io()) == YMFPCI_ENGINE_STOP_TIMEOUT);
    assert(wedgedEngine.regs[YMF_REG_CONFIG] == 1); /* never kill a busy DSP */
    assert(wedgedEngine.regs[YMF_REG_MODE] == 0);
    assert(wedgedEngine.us == 20000);

    Fake badSize;
    assert(upload(badSize, 127) == YMFPCI_BAD_FIRMWARE_SIZE);
    assert(badSize.writes.empty() && badSize.configWrites.empty());
    Fake codecTimeout;
    codecTimeout.codecBusy = true;
    assert(upload(codecTimeout) == YMFPCI_CODEC_TIMEOUT);
    assert(codecTimeout.ms == 750 && codecTimeout.writes.empty());
    Fake dspTimeout;
    dspTimeout.dspBusy = true;
    assert(upload(dspTimeout) == YMFPCI_DSP_STOP_TIMEOUT);
    assert(dspTimeout.us == 1000);
    assert(dspTimeout.regs.find(YMF_REG_DSPINSTRAM) == dspTimeout.regs.end());
    Fake cannotStop;
    cannotStop.stuckConfig = true;
    assert(upload(cannotStop) == YMFPCI_DSP_STOP_TIMEOUT);
    assert(cannotStop.regs.find(YMF_REG_DSPINSTRAM) == cannotStop.regs.end());
    Fake absent;
    absent.unavailable = true;
    assert(upload(absent) == YMFPCI_DEVICE_UNAVAILABLE);
    assert(absent.writes.empty() && absent.configWrites.empty());
    YMFPCIHardwareIO invalid = good.io();
    invalid.write32 = 0;
    assert(ymfStopDSP(invalid) == YMFPCI_BAD_ARGUMENT);
    assert(ymfStopDSP(good.io()) == YMFPCI_OK);
    assert(ymfStopDSP(absent.io()) == YMFPCI_DEVICE_UNAVAILABLE);
    std::cout << "PASS: firmware words, reset, stopped DSP, size validation, timeouts, missing device, shutdown\n";
}
