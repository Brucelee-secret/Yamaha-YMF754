/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Mac OS X IOKit adapter for staged Yamaha YMF754 initialization.
 */

#include "YMFPCIAudioDevice.h"
#include "YMFPCIRegisters.h"
#include "YMFPCIFirmware.h"
#include "YMFPCIPCM.h"
#include "YMFPCIAudioEngine.h"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSAtomic.h>

#if !defined(__x86_64__)
#error This adapter currently supports the requested x86_64 target only.
#endif

#define super IOAudioDevice
OSDefineMetaClassAndStructors(YMFPCIAudioDevice, IOAudioDevice)

/* SDK 10.6 exposes OSSynchronizeIO, not the userspace OSMemoryBarrier.
 * OSSynchronizeIO is a no-op on x86: uncached device accesses are ordered by
 * the CPU. Also prevent the compiler from moving ordinary memory accesses
 * across MMIO. CONFIG readbacks below still drain posted PCI writes. */
static inline void synchronizeMMIO()
{
    __asm__ __volatile__("" : : : "memory");
    OSSynchronizeIO();
}

bool YMFPCIAudioDevice::init(OSDictionary *properties)
{
    fAudioEngine = 0;
    fDigitalAudioEngine = 0;
    fMicEngine = 0;
    fPCIDevice = 0;
    fRegisterMap = 0;
    fInterruptSource = 0;
    fConfiguredAudioEngine = 0;
    fRegisters = 0;
    fProviderOpen = false;
    fInterruptSourceAdded = false;
    fIOConfigured = false;
    fIOWasEnabled = false;
    fMemoryConfigured = false;
    fMemoryWasEnabled = false;
    fHardwareTouched = false;
    fHardwareIO.context = this;
    fHardwareIO.read16 = read16;
    fHardwareIO.read32 = read32;
    fHardwareIO.write16 = write16;
    fHardwareIO.write32 = write32;
    fHardwareIO.configRead8 = configRead8;
    fHardwareIO.configWrite8 = configWrite8;
    fHardwareIO.configWrite16 = configWrite16;
    fHardwareIO.delayMicroseconds = delayMicroseconds;
    fHardwareIO.sleepMilliseconds = sleepMilliseconds;
    return super::init(properties);
}

bool YMFPCIAudioDevice::initHardware(IOService *provider)
{
    IOPCIDevice *pciDevice;

    if (!super::initHardware(provider))
        return false;

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (pciDevice == 0) {
        IOLog("YMFPCI: provider is not an IOPCIDevice\n");
        return false;
    }

    fPCIDevice = pciDevice;
    if (pciDevice->configRead16(0x00) != 0x1073 ||
        pciDevice->configRead16(0x02) != 0x0012 ||
        (pciDevice->configRead32(kIOPCIConfigBaseAddress0) & 1U) != 0) {
        IOLog("YMFPCI: refusing unexpected PCI identity or I/O-space BAR0\n");
        fPCIDevice = 0;
        return false;
    }
    if (!pciDevice->open(this)) {
        IOLog("YMFPCI: provider could not be opened exclusively\n");
        fPCIDevice = 0;
        return false;
    }
    fProviderOpen = true;
    const UInt16 pciCommandBeforeSetup =
        pciDevice->configRead16(kIOPCIConfigCommand);
    const UInt32 bar0 = pciDevice->configRead32(kIOPCIConfigBaseAddress0 + 0);
    const UInt32 bar1 = pciDevice->configRead32(kIOPCIConfigBaseAddress0 + 4);
    const UInt32 bar2 = pciDevice->configRead32(kIOPCIConfigBaseAddress0 + 8);
    const UInt32 bar3 = pciDevice->configRead32(kIOPCIConfigBaseAddress0 + 12);
    const UInt32 bar4 = pciDevice->configRead32(kIOPCIConfigBaseAddress0 + 16);
    const UInt32 bar5 = pciDevice->configRead32(kIOPCIConfigBaseAddress0 + 20);
    IOLog("YMFPCI: PCI pre-setup command=%04x BAR0-5=%08x/%08x/%08x/%08x/%08x/%08x\n",
        pciCommandBeforeSetup, bar0, bar1, bar2, bar3, bar4, bar5);
    const UInt32 ioBase1 = bar1 & ~3U;
    const UInt32 ioBase2 = bar2 & ~3U;
    const bool legacyIOBarsValid = bar1 != 0xffffffffU &&
        bar2 != 0xffffffffU && (bar1 & 1U) && (bar2 & 1U) &&
        ioBase1 != 0 && ioBase2 != 0 && ioBase1 != ioBase2 &&
        ioBase1 <= 0xfffcU && ioBase2 <= 0xfffcU;
    const UInt16 legacy = pciDevice->configRead16(0x40);
    const UInt16 elegacy = pciDevice->configRead16(0x42);
    const UInt8 dsxgControl = pciDevice->configRead8(0x48);
    const UInt16 powerControl1 = pciDevice->configRead16(0x4a);
    const UInt16 powerControl2 = pciDevice->configRead16(0x4e);
    const UInt16 fmBase = pciDevice->configRead16(0x60);
    const UInt16 sbBase = pciDevice->configRead16(0x62);
    const UInt16 mpuBase = pciDevice->configRead16(0x64);
    const UInt16 joystickBase = pciDevice->configRead16(0x66);
    IOLog("YMFPCI: PCI legacy/power snapshot 40=%04x 42=%04x 48=%02x 4a=%04x 4e=%04x 60=%04x 62=%04x 64=%04x 66=%04x\n",
        legacy, elegacy, dsxgControl, powerControl1, powerControl2,
        fmBase, sbBase, mpuBase, joystickBase);
    fMemoryWasEnabled = (pciCommandBeforeSetup & 2U) != 0;
    fIOWasEnabled = (pciCommandBeforeSetup & 1U) != 0;
    /*
     * Clear inherited bus-master state while claiming/mapping resources. On
     * the tested YMF754 cold-start path, bus mastering must be re-enabled
     * before firmware and DSP initialization, not only at PCM start.
     */
    pciDevice->setBusMasterEnable(false);

    /*
     * Linux ALSA maps BAR0 as a 0x8000-byte register area. Require a mapping
     * at least that large before any MMIO access.
     */
    fRegisterMap =
        pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
            kIOMapInhibitCache);
    if (fRegisterMap == 0 || fRegisterMap->getLength() < YMF_MMIO_LENGTH ||
        fRegisterMap->getVirtualAddress() == 0) {
        IOLog("YMFPCI: BAR0 could not be mapped as the expected MMIO area\n");
        releaseHardware();
        return false;
    }

    pciDevice->setMemoryEnable(true);
    fMemoryConfigured = true;
    /*
     * The YMF754 reports legacy I/O resources in BAR1/BAR2. The working
     * DS-XG driver enables I/O decoding; do so only when both assigned BARs
     * are valid 16-bit I/O addresses, and restore the original command bit
     * during teardown.
     */
    if (legacyIOBarsValid) {
        pciDevice->setIOEnable(true);
        fIOConfigured = true;
    }
    pciDevice->setBusMasterEnable(true);
    const UInt16 pciCommandAfterSetup =
        pciDevice->configRead16(kIOPCIConfigCommand);
    const bool ioDecodeReady = !legacyIOBarsValid ||
        (pciCommandAfterSetup & 1U) != 0;
    if ((pciCommandAfterSetup & 6U) != 6U || !ioDecodeReady) {
        IOLog("YMFPCI: PCI decode/DMA check failed command=%04x io-bars-valid=%u\n",
            pciCommandAfterSetup, legacyIOBarsValid ? 1U : 0U);
        releaseHardware();
        return false;
    }
    IOLog("YMFPCI: PCI after safe setup command=%04x I/O BAR1/2=%s\n",
        pciCommandAfterSetup, legacyIOBarsValid ? "valid; decode enabled" :
        "invalid; decode unchanged");
    fRegisters = reinterpret_cast<volatile UInt8 *>(fRegisterMap->getVirtualAddress());

    setDeviceName("Yamaha DS-1E");
    setDeviceShortName("YMF754");
    setManufacturerName("Yamaha");

    IOLog("YMFPCI: YMF754 revision 0x%02x; uploading DS-1E firmware\n",
        (unsigned)pciDevice->configRead8(0x08));
    fHardwareTouched = true;
    /* Diagnostic-only AC97 reads before the firmware routine resets AC-link.
     * A failed snapshot must not prevent normal hardware initialization. */
    logAC97Snapshot("pre-link-reset");
    const YMFPCIResult result = ymfUploadFirmware(fHardwareIO,
        gYMFDSPFirmware, gYMFDSPFirmwareSize,
        gYMFControllerFirmware, gYMFControllerFirmwareSize);
    if (result != YMFPCI_OK) {
        IOLog("YMFPCI: initialization failed: %s\n", ymfResultName(result));
        releaseHardware();
        return false;
    }
    IOLog("YMFPCI: DS-1E firmware uploaded: DSP=%u controller=%u bytes\n",
        gYMFDSPFirmwareSize, gYMFControllerFirmwareSize);
    logAC97Snapshot("post-link-reset");
    IOLog("YMFPCI: PCI after firmware reset CTRL=%02x PWR1=%04x PWR2=%04x\n",
        pciDevice->configRead8(0x48), pciDevice->configRead16(0x4a),
        pciDevice->configRead16(0x4e));
    fAudioEngine = new YMFPCIAudioEngine;
    if (!fAudioEngine || !fAudioEngine->init() ||
        activateAudioEngine(fAudioEngine) != kIOReturnSuccess) {
        IOLog("YMFPCI: stereo engine activation failed\n");
        releaseHardware();
        return false;
    }
    fMicEngine = new YMFPCIMicEngine;
    if (!fMicEngine || !fMicEngine->init()) {
        IOLog("YMFPCI: microphone engine allocation failed\n");
        releaseHardware();
        return false;
    }
    fAudioEngine->fMicEngine = fMicEngine;
    if (activateAudioEngine(fMicEngine) != kIOReturnSuccess) {
        IOLog("YMFPCI: microphone engine activation failed\n");
        releaseHardware();
        return false;
    }
    fDigitalAudioEngine = new YMFPCIAudioEngine;
    if (fDigitalAudioEngine) {
        if (!fDigitalAudioEngine->init()) {
            fDigitalAudioEngine->release();
            fDigitalAudioEngine = 0;
        } else {
            fDigitalAudioEngine->setDigitalOutput(true);
            if (activateAudioEngine(fDigitalAudioEngine) != kIOReturnSuccess) {
                IOLog("YMFPCI: separate S/PDIF PCM engine unavailable; analog and microphone remain active\n");
                fDigitalAudioEngine->shutdown();
                fDigitalAudioEngine->release();
                fDigitalAudioEngine = 0;
            } else {
                IOLog("YMFPCI: separate S/PDIF PCM engine registered: 48000 Hz, signed 16-bit\n");
            }
        }
    }
    if (!fDigitalAudioEngine)
        IOLog("YMFPCI: S/PDIF engine unavailable; analog and microphone remain active\n");
    if (armInterruptSource() != kIOReturnSuccess) {
        IOLog("YMFPCI: device-level IRQ source setup failed\n");
        releaseHardware();
        return false;
    }
    setProperty("YMFPCIDSPState", "AnalogPCMReady");
    IOLog("YMFPCI: analog and microphone engines registered: 48000 Hz, signed 16-bit\n");
    return true;
}

IOReturn YMFPCIAudioDevice::armInterruptSource()
{
    if (!fPCIDevice || !fRegisters)
        return kIOReturnNoDevice;

    if (!fInterruptSource) {
        IOWorkLoop *workLoop = getWorkLoop();
        if (!workLoop)
            return kIOReturnNoResources;

        fInterruptSource =
            IOFilterInterruptEventSource::filterInterruptEventSource(this,
                interruptAction, interruptFilter, fPCIDevice, 0);
        if (!fInterruptSource)
            return kIOReturnNoResources;

        if (workLoop->addEventSource(fInterruptSource) != kIOReturnSuccess) {
            fInterruptSource->release();
            fInterruptSource = 0;
            return kIOReturnError;
        }
        fInterruptSourceAdded = true;
    }

    fInterruptSource->disable();
    fHardwareIO.write32(fHardwareIO.context, YMF_REG_STATUS, 0xffffffffU);
    fHardwareIO.read32(fHardwareIO.context, YMF_REG_STATUS);
    fHardwareIO.write16(fHardwareIO.context, YMF_REG_INTFLAG, 0xffffU);
    fHardwareIO.read16(fHardwareIO.context, YMF_REG_INTFLAG);
    fHardwareIO.write16(fHardwareIO.context, YMF_REG_TIMERCTRL, 0);
    fHardwareIO.read16(fHardwareIO.context, YMF_REG_TIMERCTRL);
    fInterruptSource->enable();

    IOLog("YMFPCI: device-owned shared IRQ source armed\n");
    return kIOReturnSuccess;
}

void YMFPCIAudioDevice::disarmInterruptSource()
{
    if (fInterruptSource)
        fInterruptSource->disable();
}

void YMFPCIAudioDevice::releaseInterruptSource()
{
    if (!fInterruptSource)
        return;

    fInterruptSource->disable();
    if (fInterruptSourceAdded) {
        IOWorkLoop *workLoop = getWorkLoop();
        if (workLoop)
            workLoop->removeEventSource(fInterruptSource);
        fInterruptSourceAdded = false;
    }
    fInterruptSource->release();
    fInterruptSource = 0;
}

bool YMFPCIAudioDevice::interruptFilter(OSObject *owner,
    IOFilterInterruptEventSource *source)
{
    YMFPCIAudioDevice *device =
        OSDynamicCast(YMFPCIAudioDevice, owner);
    if (!device)
        return false;

    YMFPCIAudioEngine *engine = device->fConfiguredAudioEngine;
    if (!engine)
        engine = device->fAudioEngine;
    return engine &&
        YMFPCIAudioEngine::interruptFilter(engine, source);
}

void YMFPCIAudioDevice::interruptAction(OSObject *owner,
    IOInterruptEventSource *source, int count)
{
    YMFPCIAudioDevice *device =
        OSDynamicCast(YMFPCIAudioDevice, owner);
    if (!device)
        return;

    YMFPCIAudioEngine *engine = device->fConfiguredAudioEngine;
    if (!engine)
        engine = device->fAudioEngine;
    if (engine) {
        YMFPCIAudioEngine::interruptAction(engine, source, count);
        return;
    }

    /* A stray shared IRQ can arrive while engines are being torn down. */
    const UInt32 status =
        device->fHardwareIO.read32(device->fHardwareIO.context, YMF_REG_STATUS);
    if (status != 0xffffffffU && (status & 0x80000000U)) {
        device->fHardwareIO.write32(device->fHardwareIO.context,
            YMF_REG_STATUS, 0x80000000U);
        device->fHardwareIO.read32(device->fHardwareIO.context, YMF_REG_STATUS);
    }
    const UInt16 flags =
        device->fHardwareIO.read16(device->fHardwareIO.context, YMF_REG_INTFLAG);
    if (flags != 0xffffU && flags != 0) {
        device->fHardwareIO.write16(device->fHardwareIO.context,
            YMF_REG_INTFLAG, flags);
        device->fHardwareIO.read16(device->fHardwareIO.context, YMF_REG_INTFLAG);
    }
}

void YMFPCIAudioDevice::releaseHardware()
{
    disarmInterruptSource();

    if (fDigitalAudioEngine) {
        fDigitalAudioEngine->shutdown();
        if (fConfiguredAudioEngine == fDigitalAudioEngine)
            fConfiguredAudioEngine = 0;
    }
    if (fMicEngine) {
        fMicEngine->shutdown();
    }
    if (fAudioEngine) {
        /* Stop/remove the shared IRQ source before releasing the mic engine:
         * the IRQ action may otherwise hold a stale mic-engine pointer. */
        fAudioEngine->shutdown();
        if (fConfiguredAudioEngine == fAudioEngine)
            fConfiguredAudioEngine = 0;
    }
    releaseInterruptSource();

    if (fDigitalAudioEngine) {
        YMFPCIAudioEngine *digital = fDigitalAudioEngine;
        fDigitalAudioEngine = 0;
        digital->release();
    }
    if (fAudioEngine)
        fAudioEngine->fMicEngine = 0;
    if (fMicEngine) {
        YMFPCIMicEngine *mic = fMicEngine;
        fMicEngine = 0;
        mic->release();
    }
    if (fAudioEngine) {
        fAudioEngine->release();
        fAudioEngine = 0;
    }
    if (fPCIDevice && fProviderOpen)
        fPCIDevice->setBusMasterEnable(false);
    if (fHardwareTouched && fRegisters) {
        const YMFPCIResult result = ymfStopDSP(fHardwareIO);
        if (result != YMFPCI_OK)
            IOLog("YMFPCI: shutdown: %s\n", ymfResultName(result));
    }
    fHardwareTouched = false;
    fRegisters = 0;
    if (fRegisterMap != 0) {
        fRegisterMap->release();
        fRegisterMap = 0;
    }
    if (fMemoryConfigured && fPCIDevice)
        fPCIDevice->setMemoryEnable(fMemoryWasEnabled);
    fMemoryConfigured = false;
    if (fIOConfigured && fPCIDevice)
        fPCIDevice->setIOEnable(fIOWasEnabled);
    fIOConfigured = false;
    if (fProviderOpen && fPCIDevice)
        fPCIDevice->close(this);
    fProviderOpen = false;
    fPCIDevice = 0;
}

void YMFPCIAudioDevice::logAC97Snapshot(const char *stage)
{
    if (!fPCIDevice || !fRegisters || !stage)
        return;

    const UInt16 pciCommand =
        fPCIDevice->configRead16(kIOPCIConfigCommand);
    const YMFWord registers[] = { 0x04, 0x06, 0x18, 0x38 };
    YMFWord values[sizeof(registers) / sizeof(registers[0])];

    /*
     * ymfCodecRead issues AC97 read commands through MMIO, but does not write
     * any AC97 codec register. Stop on the first failed read to keep this
     * diagnostic bounded; failure is logged and never aborts initialization.
     */
    for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        const YMFPCIResult result =
            ymfCodecRead(fHardwareIO, registers[i], values[i]);
        if (result != YMFPCI_OK) {
            IOLog("YMFPCI: AC97 snapshot stage=%s pci-command=%04x read-failed reg=%02x result=%s\n",
                stage, pciCommand, registers[i], ymfResultName(result));
            return;
        }
    }

    IOLog("YMFPCI: AC97 snapshot stage=%s pci-command=%04x 04=%04x 06=%04x 18=%04x 38=%04x\n",
        stage, pciCommand, values[0], values[1], values[2], values[3]);
}

void YMFPCIAudioDevice::stop(IOService *provider)
{
    /* Engine teardown must run while MMIO/provider are still available. */
    IOLog("YMFPCI: device stop entered\n");
    disarmInterruptSource();
    if (fDigitalAudioEngine) {
        IOLog("YMFPCI: device stop digital engine begin\n");
        fDigitalAudioEngine->shutdown();
        IOLog("YMFPCI: device stop digital engine complete\n");
    }
    if (fMicEngine) {
        IOLog("YMFPCI: device stop microphone engine begin\n");
        fMicEngine->shutdown();
        IOLog("YMFPCI: device stop microphone engine complete\n");
    }
    if (fAudioEngine) {
        IOLog("YMFPCI: device stop analog engine begin\n");
        fAudioEngine->shutdown();
        IOLog("YMFPCI: device stop analog engine complete\n");
    }
    releaseInterruptSource();
    IOLog("YMFPCI: device stop superclass begin\n");
    super::stop(provider);
    IOLog("YMFPCI: device stop superclass complete; releasing hardware\n");
    releaseHardware();
    IOLog("YMFPCI: device stop complete\n");
}

void YMFPCIAudioDevice::free()
{
    releaseHardware();
    super::free();
}

IOReturn YMFPCIAudioDevice::performPowerStateChange(IOAudioDevicePowerState oldState,
    IOAudioDevicePowerState newState, UInt32 *microsecondsUntilComplete)
{
    if (microsecondsUntilComplete)
        *microsecondsUntilComplete = 0;
    if (newState == kIOAudioDeviceSleep) {
        if (fDigitalAudioEngine)
            fDigitalAudioEngine->suspendHardware();
        if (fAudioEngine)
            fAudioEngine->suspendHardware();
        disarmInterruptSource();
    } else if (oldState == kIOAudioDeviceSleep) {
        if (armInterruptSource() != kIOReturnSuccess)
            return kIOReturnIOError;
        if (fAudioEngine && !fAudioEngine->resumeHardware())
        {
            disarmInterruptSource();
            return kIOReturnIOError;
        }
        if (fDigitalAudioEngine && !fDigitalAudioEngine->resumeHardware())
        {
            disarmInterruptSource();
            return kIOReturnIOError;
        }
    }
    return kIOReturnSuccess;
}

/* x86_64 is little-endian. Use volatile accesses of the required MMIO width
 * with uncached mapping and compiler ordering. */
YMFWord YMFPCIAudioDevice::read16(void *context, unsigned offset)
{
    YMFPCIAudioDevice *device = static_cast<YMFPCIAudioDevice *>(context);
    if (!device->fRegisters || offset > YMF_MMIO_LENGTH - 2 || (offset & 1))
        return 0xffff;
    synchronizeMMIO();
    return *reinterpret_cast<volatile UInt16 *>(device->fRegisters + offset);
}

YMFDword YMFPCIAudioDevice::read32(void *context, unsigned offset)
{
    YMFPCIAudioDevice *device = static_cast<YMFPCIAudioDevice *>(context);
    if (!device->fRegisters || offset > YMF_MMIO_LENGTH - 4 || (offset & 3))
        return 0xffffffffU;
    synchronizeMMIO();
    return *reinterpret_cast<volatile UInt32 *>(device->fRegisters + offset);
}

void YMFPCIAudioDevice::write16(void *context, unsigned offset, YMFWord value)
{
    YMFPCIAudioDevice *device = static_cast<YMFPCIAudioDevice *>(context);
    if (!device->fRegisters || offset > YMF_MMIO_LENGTH - 2 || (offset & 1)) {
        IOLog("YMFPCI: rejected invalid 16-bit MMIO write\n");
        return;
    }
    *reinterpret_cast<volatile UInt16 *>(device->fRegisters + offset) = value;
    synchronizeMMIO();
}

void YMFPCIAudioDevice::write32(void *context, unsigned offset, YMFDword value)
{
    YMFPCIAudioDevice *device = static_cast<YMFPCIAudioDevice *>(context);
    if (!device->fRegisters || offset > YMF_MMIO_LENGTH - 4 || (offset & 3)) {
        IOLog("YMFPCI: rejected invalid 32-bit MMIO write\n");
        return;
    }
    *reinterpret_cast<volatile UInt32 *>(device->fRegisters + offset) = value;
    synchronizeMMIO();
}

YMFByte YMFPCIAudioDevice::configRead8(void *context, unsigned offset)
{
    return static_cast<YMFPCIAudioDevice *>(context)->fPCIDevice->configRead8(offset);
}

void YMFPCIAudioDevice::configWrite8(void *context, unsigned offset, YMFByte value)
{
    static_cast<YMFPCIAudioDevice *>(context)->fPCIDevice->configWrite8(offset, value);
}

void YMFPCIAudioDevice::configWrite16(void *context, unsigned offset, YMFWord value)
{
    static_cast<YMFPCIAudioDevice *>(context)->fPCIDevice->configWrite16(offset, value);
}

void YMFPCIAudioDevice::delayMicroseconds(void *, unsigned value)
{
    IODelay(value);
}

void YMFPCIAudioDevice::sleepMilliseconds(void *, unsigned value)
{
    IOSleep(value);
}
