#!/usr/bin/env python
# Source-list/plist regression checks; Python 2.6+/3, no external packages.
from __future__ import print_function
import glob
import os
import plistlib
import re

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(root, "YMF754-macOS.xcodeproj", "project.pbxproj")) as f:
    project = f.read()
project = re.compile(r"/\*.*?\*/", re.S).sub("", project)
files = dict(re.findall(
    r"([A-F0-9]{24})\s*=\s*\{isa = PBXFileReference;[^}]*?path = ([^;]+);",
    project))
build = dict(re.findall(
    r"([A-F0-9]{24})\s*=\s*\{isa = PBXBuildFile; fileRef = ([A-F0-9]{24})\s*;",
    project))
phase = re.search(r"isa = PBXSourcesBuildPhase;.*?files = \((.*?)\);", project, re.S)
ids = re.findall(r"[A-F0-9]{24}", phase.group(1))
sources = [files[build[item]] for item in ids]
expected = [os.path.basename(p) for p in glob.glob(os.path.join(root, "Driver", "*.cpp"))]
assert len(sources) == len(set(sources)), "Duplicate Xcode source entry"
assert sorted(sources) == sorted(expected), (sources, expected)
assert "PBXShellScriptBuildPhase" not in project, "firmware must not depend on a hidden generated include"
assert "YMFPCIFirmware.cpp" in sources, "embedded firmware must compile as a visible source file"
for filename in files.values():
    if filename.endswith((".cpp", ".h", ".plist")):
        assert os.path.isfile(os.path.join(root, "Driver", filename)), filename
path = os.path.join(root, "Driver", "Info.plist")
if hasattr(plistlib, "load"):
    with open(path, "rb") as f:
        info = plistlib.load(f)
else:
    info = plistlib.readPlist(path)
assert info["IOKitPersonalities"]["Yamaha YMF754 DS-1E"]["IOPCIPrimaryMatch"] == "0x00121073"
assert "com.apple.kpi.mach" in info["OSBundleLibraries"]
assert info["CFBundleShortVersionString"] == "0.2.0"
app_root = os.path.join(root, "ControlApp")
with open(os.path.join(app_root, "YMF754Control.xcodeproj", "project.pbxproj")) as f:
    app_project = f.read()
with open(os.path.join(app_root, "main.m")) as f:
    app_source = f.read()
app_project_clean = re.compile(r"/\*.*?\*/", re.S).sub("", app_project)
app_files = dict(re.findall(
    r"([A-F0-9]{24})\s*=\s*\{isa = PBXFileReference;[^}]*?path = ([^;]+);",
    app_project_clean))
app_files = dict((key, value.strip('"')) for key, value in app_files.items())
app_build = dict(re.findall(
    r"([A-F0-9]{24})\s*=\s*\{isa = PBXBuildFile; fileRef = ([A-F0-9]{24})\s*;",
    app_project_clean))
app_phase = re.search(
    r"isa = PBXSourcesBuildPhase;.*?files = \((.*?)\);",
    app_project_clean, re.S)
app_source_ids = re.findall(r"[A-F0-9]{24}", app_phase.group(1))
app_sources = [app_files[app_build[item]] for item in app_source_ids]
if hasattr(plistlib, "load"):
    with open(os.path.join(app_root, "Info.plist"), "rb") as f:
        app_info = plistlib.load(f)
else:
    app_info = plistlib.readPlist(os.path.join(app_root, "Info.plist"))
assert "productType = \"com.apple.product-type.application\"" in app_project
assert "SDKROOT = macosx10.6;" in app_project
assert "INFOPLIST_FILE = Info.plist;" in app_project
assert app_sources == ["main.m", "../UserSupport/YMF754ControlAccess.m"]
assert "Cocoa.framework" in app_project and "IOKit.framework" in app_project
for filename in app_files.values():
    if filename in ("main.m", "Info.plist") or filename.startswith("../UserSupport/"):
        assert os.path.isfile(os.path.join(app_root, filename)), filename
assert app_info["LSMinimumSystemVersion"] == "10.6"
support_source_path = os.path.join(root, "UserSupport", "YMF754ControlAccess.m")
with open(support_source_path) as f:
    support_source = f.read()
assert "IOServiceMatching(kYMF754AudioEngineClass)" in support_source
assert "IORegistryEntryGetChildIterator" in support_source
assert "kYMF754ThreeDWideControlID = 0x33445744U" in support_source
assert "IORegistryEntrySetCFProperty" in support_source
assert "kIOAudioControlValueKey" in support_source
assert "register state only, audible effect unverified" in open(
    os.path.join(root, "Driver", "YMFPCIAudioEngine.cpp")).read()
assert "YMFFindThreeDWideControl()" in app_source
assert "IOServiceMatching(kIOAudioControlClassName)" not in app_source
assert "Повторить поиск" in app_source
pane_root = os.path.join(root, "ControlPane")
with open(os.path.join(pane_root, "YMF754ThreeDWide.xcodeproj", "project.pbxproj")) as f:
    pane_project = f.read()
with open(os.path.join(pane_root, "YMF754ThreeDWidePreferencePane.m")) as f:
    pane_source = f.read()
pane_project_clean = re.compile(r"/\*.*?\*/", re.S).sub("", pane_project)
pane_files = dict(re.findall(
    r"([A-F0-9]{24})\s*=\s*\{isa = PBXFileReference;[^}]*?path = ([^;]+);",
    pane_project_clean))
pane_files = dict((key, value.strip('"')) for key, value in pane_files.items())
pane_build = dict(re.findall(
    r"([A-F0-9]{24})\s*=\s*\{isa = PBXBuildFile; fileRef = ([A-F0-9]{24})\s*;",
    pane_project_clean))
pane_phase = re.search(
    r"isa = PBXSourcesBuildPhase;.*?files = \((.*?)\);",
    pane_project_clean, re.S)
pane_source_ids = re.findall(r"[A-F0-9]{24}", pane_phase.group(1))
pane_sources = [pane_files[pane_build[item]] for item in pane_source_ids]
assert pane_sources == [
    "YMF754ThreeDWidePreferencePane.m",
    "../UserSupport/YMF754ControlAccess.m",
]
assert "productType = \"com.apple.product-type.bundle\"" in pane_project
assert "WRAPPER_EXTENSION = prefPane;" in pane_project
assert len(re.findall(r"^\s*ARCHS = \"i386 x86_64\";$", pane_project, re.M)) == 4
assert pane_project.count("ONLY_ACTIVE_ARCH = NO;") == 2
assert pane_project.count('VALID_ARCHS = "i386 x86_64";') == 2
assert "SDKROOT = macosx10.6;" in pane_project
assert "- (NSView *)loadMainView" in pane_source
assert "YMFFindThreeDWideControl()" in pane_source
for filename in pane_files.values():
    if filename.endswith((".m", ".h", ".plist")):
        assert os.path.isfile(os.path.join(pane_root, filename)), filename
if hasattr(plistlib, "load"):
    with open(os.path.join(pane_root, "Info.plist"), "rb") as f:
        pane_info = plistlib.load(f)
else:
    pane_info = plistlib.readPlist(os.path.join(pane_root, "Info.plist"))
assert pane_info["NSPrincipalClass"] == "YMF754ThreeDWidePreferencePane"
assert pane_info["LSMinimumSystemVersion"] == "10.6"
device_path = os.path.join(root, "Driver", "YMFPCIAudioDevice.cpp")
with open(device_path) as f:
    device = f.read()
assert "PCI pre-setup command=%04x BAR0-5=" in device
assert "PCI legacy/power snapshot 40=%04x 42=%04x 48=%02x" in device
assert "PCI after safe setup command=%04x I/O BAR1/2=%s" in device
assert "const bool legacyIOBarsValid" in device
assert "ioBase1 <= 0xfffcU && ioBase2 <= 0xfffcU" in device
assert "setIOEnable(true)" in device
assert "fIOConfigured && fPCIDevice" in device
assert "setIOEnable(fIOWasEnabled)" in device
assert "pciDevice->setBusMasterEnable(true);" in device
assert "(pciCommandAfterSetup & 6U) != 6U" in device
assert device.index("pciDevice->setMemoryEnable(true);") < \
    device.index("pciDevice->setIOEnable(true);") < \
    device.index("pciDevice->setBusMasterEnable(true);") < \
    device.index("ymfUploadFirmware(fHardwareIO,")
assert "PCI after firmware reset CTRL=%02x PWR1=%04x PWR2=%04x" in device
device_header_path = os.path.join(root, "Driver", "YMFPCIAudioDevice.h")
with open(device_header_path) as f:
    device_header = f.read()
snapshot_start = device.index("void YMFPCIAudioDevice::logAC97Snapshot")
snapshot_end = device.index("void YMFPCIAudioDevice::stop", snapshot_start)
snapshot = device[snapshot_start:snapshot_end]
assert "const YMFWord registers[] = { 0x04, 0x06, 0x18, 0x38 };" in snapshot
assert "ymfCodecRead(fHardwareIO, registers[i], values[i])" in snapshot
assert "ymfCodecWrite" not in snapshot, "AC97 snapshot must not write codec registers"
assert 'logAC97Snapshot("pre-link-reset")' in device
assert 'logAC97Snapshot("post-link-reset")' in device
assert "armInterruptSource()" in device
assert device.index("activateAudioEngine(fMicEngine)") < \
    device.index("armInterruptSource()"), \
    "arm the device IRQ only after all audio engines are registered"
assert device.index("activateAudioEngine(fDigitalAudioEngine)") < \
    device.index("armInterruptSource()"), \
    "register all endpoints before arming the shared device IRQ"
engine_path = os.path.join(root, "Driver", "YMFPCIAudioEngine.cpp")
with open(engine_path) as f:
    engine = f.read()
engine_header_path = os.path.join(root, "Driver", "YMFPCIAudioEngine.h")
with open(engine_header_path) as f:
    engine_header = f.read()
assert 'logAC97Snapshot("post-codec-config")' in engine
assert "kIOAudioStreamDirectionInput" in engine
assert '"YMF754 microphone"' in engine
assert "class YMFPCIMicEngine : public IOAudioEngine" in engine_header
assert "YMFPCIMicEngine *fMicEngine;" in engine_header
assert "YMFPCIAudioEngine *fOwner;" in engine_header
assert "IOReturn armInterruptSource();" in device_header
assert "void disarmInterruptSource();" in device_header
assert "IOFilterInterruptEventSource *fInterruptSource;" in device_header
assert "IOFilterInterruptEventSource *fInterrupt;" not in engine_header
assert "fInterruptAdded" not in engine_header
assert "IOFilterInterruptEventSource::filterInterruptEventSource" not in engine
assert "fInputStream = input;" in engine
assert "fOutputStream" not in engine
assert "convertInputSamples" in engine_header
assert "YMF_REG_ADCSLOTSR" in engine and "YMF_REG_ADCFORMAT" in engine
assert "ymfPCMInitCaptureBank" in engine
assert "activeClientCount()" not in engine, \
    "capture lifecycle is owned by the separate microphone engine"
assert "1U << YMF_PCM_MIC_CAPTURE_VOICE" in engine
assert "startCaptureEngine()" in engine and "stopCaptureEngine()" in engine
assert "ymfCodecWrite(io, 0x1a, 0x0000)" in engine, \
    "AC97 microphone must remain selected as the recording source"
assert "microphone capture window frames=%u nonzero_samples=" in engine
assert "YMF_REG_PRIADCLOOPVOL, 0);" in engine
assert "YMF_REG_PRIADCLOOPVOL, 0x3fff3fff" not in engine, \
    "do not route ADC monitor audio directly to the speakers"
assert "YMF_REG_PRIADCOUTVOL, 0);" in engine, \
    "AC97 ADC-to-playback monitor must be explicitly muted"
assert "ADC monitor at capture start playback=%08x loop=%08x" in engine
assert "updateAnalogDACGate" not in engine
assert "fPlaybackActive" in engine and "fCaptureActive" in engine
assert "IOLockLock(fLifecycleLock)" in engine
capture_routing = engine[engine.index("void YMFPCIAudioEngine::updateCaptureRouting()"):
                         engine.index("UInt32 YMFPCIAudioEngine::currentFrameLocked()")]
assert "YMF_REG_MAPOFREC" in capture_routing
assert "const bool requested = fCaptureActive;" in capture_routing
assert "recordMap & ~mask" not in capture_routing, \
    "the firmware latches MAPOFREC at MODE start; don't toggle it while running"
assert "io.write32(io.context, YMF_REG_MAPOFREC" not in capture_routing, \
    "capture routing must be published before MODE, not changed live"
assert "ymfPCMInitCaptureBank" not in capture_routing, \
    "capture descriptors must not be rewritten while the DSP is running"
assert "bzero(fCaptureSamples.bytes" not in capture_routing, \
    "do not clear a live capture DMA ring when enabling its route"
prep_pos = engine.index("void YMFPCIAudioEngine::prepareCapturePath()")
prep = engine[prep_pos:engine.index("bool YMFPCIAudioEngine::configureHardware()", prep_pos)]
assert prep.index("YMF_REG_ADCSLOTSR") < prep.index("YMF_REG_ADCFORMAT"), \
    "write the ADC slot rate before its format"
assert prep.index("ymfPCMInitCaptureBank") < prep.index("YMF_REG_MAPOFREC"), \
    "prepare capture descriptors before publishing the record map"
assert "io.write32(io.context, YMF_REG_MAPOFREC," in prep and \
    "1U << YMF_PCM_MIC_CAPTURE_VOICE" in prep, \
    "publish the capture route before MODE starts"
assert "fCaptureRunning = recordMap != 0xffffffffU" in prep
hw = engine[engine.index("bool YMFPCIAudioEngine::configureHardware()"):
            engine.index("IOReturn YMFPCIAudioEngine::performAudioEngineStart()")]
assert "if (!fDigitalOutput)\n        prepareCapturePath();" in hw
assert hw.index("prepareCapturePath();") < hw.index("YMF_REG_CONFIG, 1)")
start_cap = engine[engine.index("IOReturn YMFPCIAudioEngine::startDSP()"):
                   engine.index("IOReturn YMFPCIAudioEngine::performAudioEngineStop()")]
assert "!fDevice->fInterruptSource || !fDevice->fInterruptSourceAdded" in start_cap
assert "fInterrupt->enable();" not in start_cap
assert "disarmInterruptSource()" not in start_cap
assert start_cap.index("initializeBanks();") < start_cap.index("prepareCapturePath();")
assert start_cap.index("prepareCapturePath();") < start_cap.index("stoppedMode | 3U"), \
    "publish capture descriptors before starting DSP MODE"
assert "ymfAC97MicVolumeRegister(" in engine, \
    "initialize the microphone register through its sensitivity mapping"
assert "YMF_REG_NATIVEADCINVOL, 0x3fff3fff" in engine, \
    "native ADC capture input gain must be initialized"
assert "kIOAudioControlUsageInput" in engine
assert "micVolumeValueChanged" in engine_header and "microphone sensitivity %d/31" in engine
assert "IOAudioToggleControl::createMuteControl" in engine
assert "muteValueChanged" in engine_header
assert "output mute=%d; AC97 master=%04x" in engine
assert "updateCaptureRouting();" not in engine[
    engine.index("IOReturn YMFPCIAudioEngine::clipOutputSamples"):
    engine.index("IOReturn YMFPCIAudioEngine::convertInputSamples")]
assert "updateCaptureRouting();" not in engine[
    engine.index("IOReturn YMFPCIAudioEngine::convertInputSamples"):
    engine.index("void YMFPCIAudioEngine::suspendHardware")]
assert "IOAudioSelectorControl::createOutputSelector" not in engine
assert '"Yamaha YMF754 Analog Output"' in engine
assert '"Yamaha YMF754 Digital Output (S/PDIF)"' in engine
assert "fDigitalAudioEngine" in device
assert "if (fDevice->fDigitalAudioEngine &&\n        fDevice->fDigitalAudioEngine->fRunning)" in engine
assert device.count("new YMFPCIAudioEngine") == 2
assert "fMicEngine = new YMFPCIMicEngine;" in device
assert "activateAudioEngine(fMicEngine)" in device
assert device.index("activateAudioEngine(fAudioEngine)") < \
    device.index("activateAudioEngine(fMicEngine)")
assert device.index("activateAudioEngine(fMicEngine)") < \
    device.index("activateAudioEngine(fDigitalAudioEngine)")
release_hardware = device[device.index("void YMFPCIAudioDevice::releaseHardware()"):
                          device.index("void YMFPCIAudioDevice::stop")]
assert release_hardware.index("disarmInterruptSource();") < \
    release_hardware.index("fDigitalAudioEngine->shutdown();")
assert release_hardware.index("fDigitalAudioEngine->shutdown();") < \
    release_hardware.index("fMicEngine->shutdown();") < \
    release_hardware.index("fAudioEngine->shutdown();")
assert release_hardware.index("fAudioEngine->shutdown();") < \
    release_hardware.index("releaseInterruptSource();") < \
    release_hardware.index("mic->release();"), \
    "remove the device-owned IRQ before releasing its engines"
assert "separate S/PDIF PCM engine registered" in device
assert "other->stopAudioEngine()" in engine
assert "fDevice->fMicEngine->stopAudioEngine()" in engine
assert "stopped previous %s output before %s start" in engine
assert "YMF_REG_SPDIFOUTCTRL, fDigitalOutput ? 2 : 0" in engine
assert "YMF_REG_SPDIFOUTSTATUS, YMF_SPDIF_DEFAULT_STATUS" in engine
assert "YMF_REG_SECCONFIG" in engine and "(1U << 30)" in engine
assert "fDevice->fAudioEngine->fInterrupt->enable()" not in engine
assert "device-owned shared IRQ source armed" in device
init_hardware = engine[engine.index("bool YMFPCIAudioEngine::initHardware"):
                       engine.index("bool YMFPCIAudioEngine::configureHardware()")]
assert "if (fThreeDWideSupported) {" in init_hardware
assert "IOAudioToggleControl::create(" in init_hardware
assert "wide->setValueChangeHandler(threeDWideValueChanged, this)" in init_hardware
assert "addDefaultAudioControl(wide)" in init_hardware
assert "YMF_AUDIO_CONTROL_ID_3D_WIDE = 0x33445744U" in engine
three_d_handler = engine[
    engine.index("IOReturn YMFPCIAudioEngine::threeDWideValueChanged"):
    engine.index("IOReturn YMFPCIAudioEngine::micVolumeValueChanged")]
assert "engine->fDigitalOutput" in three_d_handler
assert "engine->fThreeDWideSupported" in three_d_handler
assert "ymfAC97ThreeDWideRegisters" in three_d_handler
assert "engine->fThreeDWide = enabled" in three_d_handler
assert "3D Wide %s; AC97 3D=%04x GP=%04x" in three_d_handler
assert "if (!fDigitalOutput && !configureHardware())" in init_hardware, \
    "register SPDIF without resetting the already-working analog route"
assert "fLayout = analogEngine->fLayout;" in init_hardware and \
    "else if (!discoverLayout(fLayout))" in init_hardware, \
    "SPDIF registration must reuse the analog firmware layout without resetting DSP CONFIG"
mic_hardware = engine[engine.index("bool YMFPCIMicEngine::initHardware"):
                      engine.index("UInt32 YMFPCIMicEngine::getCurrentSampleFrame()")]
assert "IOAudioStream *stream = new IOAudioStream;" in init_hardware
assert "kIOAudioStreamDirectionInput" not in init_hardware
assert "kIOAudioStreamDirectionInput" in mic_hardware
assert "fCaptureSamples.bytes" in mic_hardware
assert "fThreeDWideSupported" in engine_header
assert "YMF_AC97_3D_CONTROL_REGISTER" in engine
assert "fThreeDWideSupported = vendor1 == 0x8384 && vendor2 == 0x7608" in engine
assert "fThreeDWide = true;" in engine
assert "ymfAC97ThreeDWideRegisters" in engine
assert "YMF_AC97_GENERAL_PURPOSE_REGISTER" in engine
assert "YMF_AC97_3D_ENABLE_MASK" in engine
assert "YMF_AC97_3D_CONTROL_WIDE_VALUE" in open(
    os.path.join(root, "Driver", "YMFPCIPCM.h")).read()
assert "AC97 reset/capability register=%04x (raw)" in engine
assert "register state only, audible effect unverified" in engine
assert engine.index("if (fThreeDWideSupported) {") < \
    engine.index("const unsigned muted[]"), \
    "apply 3D Wide before later AC97 route/volume writes"
capture_convert = engine[engine.index("IOReturn YMFPCIAudioEngine::convertInputSamples"):
                         engine.index("void YMFPCIAudioEngine::suspendHardware()")]
assert "static_cast<const SInt16 *>(fCaptureSamples.bytes)" in capture_convert
assert "if (++sourceFrame == YMF_PCM_FRAMES)" in capture_convert
assert "UInt32 *destination = static_cast<UInt32 *>(mix);" in capture_convert
assert "bzero(mix, frames * 8);" in capture_convert
assert "mix) + firstFrame * 8" not in capture_convert, \
    "input mix buffer is callback-sized, not indexed by the DMA ring position"
assert "bank-start=%u/%u" in capture_convert
assert "YMF_REG_MAPOFREC" in capture_convert and "YMF_REG_ADCFORMAT" in capture_convert
start_pos = engine.index("IOReturn YMFPCIAudioEngine::startDSP()")
stop_pos = engine.index("IOReturn YMFPCIAudioEngine::performAudioEngineStop()", start_pos)
start = engine[start_pos:stop_pos]
assert "fDevice->fConfiguredAudioEngine != this && !configureHardware()" in start
assert "other->stopAudioEngine()" not in start
assert "initializeBanks();" in start, "reset the playback banks while DMA is stopped"
assert "controlHeader[1] = fControl.address + fLayout.playbackOffset" in start
assert "controlHeader[2] = fControl.address + fLayout.playbackOffset +" in start
assert "fPlaybackActive ? 0x3fff3fffU : 0" in start
assert "voice-pointers=%08x/%08x" in start
assert "playback-bank v=%u.%u fmt=%08x" in start
assert "playback-bank state v=%u.%u status=%08x" in start
assert "for (unsigned bankIndex = 0; bankIndex < 2; ++bankIndex)" in start
assert start.index("playback-bank v=%u.%u fmt=") < start.index("setBusMasterEnable(true)")
assert "headerReserved" not in start
assert start.index("initializeBanks();") < start.index("controlHeader[1] =")
assert "YMF_REG_STATUS, 0xffffffffU" in start, "clear cold-start status before DMA"
assert "YMF_REG_INTFLAG, 0xffffU" in start, "clear stale device IRQ flags before DMA"
assert "stoppedMode | 3U" in start, "preserve MODE bits as ALSA does"
assert start.index("setBusMasterEnable(true)") < start.index("stoppedMode | 3U")
assert start.index("stoppedMode | 3U") < start.index("controlHeader[1] ="), \
    "start DSP MODE before publishing active voice pointers, as ALSA does"
assert start.index("controlHeader[1] =") < start.index("controlHeader[2] =")
assert "fInterrupt->enable()" not in start, \
    "the shared device IRQ is armed independently of engine starts"
assert "disarmInterruptSource()" not in start, \
    "analog and digital routes must use the same device-owned IRQ source"
assert "delayed DMA state status=" in engine, "keep post-start hardware state diagnostics"
assert "playback joined active capture DSP" in engine
assert "microphone engine started independently of playback" in engine
assert "playback stopped; microphone DSP remains active" in engine
assert "fDevice->fMicEngine->updateTimestamp()" in engine
assert "UInt32 YMFPCIAudioEngine::currentCaptureFrame()" in engine
assert "bytePosition / 4U" in engine
assert "fOwner->convertInputSamples" in engine
assert "void YMFPCIMicEngine::shutdownLocked()" in engine
irq_filter = engine[engine.index("bool YMFPCIAudioEngine::interruptFilter"):
                    engine.index("void YMFPCIAudioEngine::interruptAction")]
assert "YMF_REG_INTFLAG" in irq_filter and "YMF_REG_STATUS" in irq_filter
assert "io.write" not in irq_filter and "takeTimeStamp" not in irq_filter, \
    "keep the primary interrupt filter short; defer MMIO writes and timestamps"
irq_action = engine[engine.index("void YMFPCIAudioEngine::interruptAction"):
                    engine.index("IOReturn YMFPCIAudioEngine::performFormatChange")]
assert "YMF_REG_INTFLAG" in irq_action and "YMF_REG_STATUS" in irq_action
assert "io.write16(io.context, YMF_REG_INTFLAG" in irq_action
assert "io.write32(io.context, YMF_REG_STATUS" in irq_action
assert "non-PCM interrupt acknowledged" in irq_action
assert "workLoop->addEventSource(fInterruptSource)" in device
assert device.count("IOFilterInterruptEventSource::filterInterruptEventSource") == 1
assert "interruptAction, interruptFilter, fPCIDevice, 0" in device
arm_irq = device[device.index("IOReturn YMFPCIAudioDevice::armInterruptSource()"):
                 device.index("void YMFPCIAudioDevice::disarmInterruptSource()")]
assert "YMF_REG_INTFLAG, 0xffffU" in arm_irq
assert "fInterruptSource->enable()" in arm_irq
assert "YMFPCIAudioEngine::interruptAction(engine, source, count)" in device
assert "engine = device->fConfiguredAudioEngine;" in device
assert "engine = device->fAudioEngine;" in device
stop = engine[engine.index("IOReturn YMFPCIAudioEngine::stopDSP()"):
              engine.index("void YMFPCIAudioEngine::updateCaptureRouting()")]
assert "controlHeader[1] = 0;" in stop and "controlHeader[2] = 0;" in stop
assert "mode & ~3U" in stop, "stop only the ALSA playback mode bits"
assert "fInterrupt->disable()" not in stop
assert "fDevice->fAudioEngine->fInterrupt" not in stop
configure_pos = engine.index("bool YMFPCIAudioEngine::configureHardware()")
configure_end = engine.index("IOReturn YMFPCIAudioEngine::performAudioEngineStart()", configure_pos)
configure = engine[configure_pos:configure_end]
assert "ymfStopDSP(" in configure, "restore the target card's known-good DSP reset sequence"
assert "YMF_REG_STATUS, 0xffffffffU" in configure, "clear latched status during DSP setup"
assert "YMF_REG_CONFIG, 1" in configure, "re-enable DSP after banks and bases are prepared"
assert configure.index("initializeBanks();") < configure.index("YMF_REG_PLAYCTRLBASE")
assert configure.index("YMF_REG_PLAYCTRLBASE") < configure.index("YMF_REG_CONFIG, 1")
assert "YMF_REG_INTFLAG, 0xffffU" in configure
print("PASS: Xcode sources/PCI match; analog, microphone, separate SPDIF endpoint, device-owned shared IRQ")
