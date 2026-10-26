# YMF754 3D Wide control app

This small Cocoa app provides the user interface that Mac OS X 10.6's Sound
preference pane does not provide for custom `IOAudioToggleControl` entries.
It finds the registered YMFPCIAudioEngine and searches its attached child
controls by ID, then changes `IOAudioControlValue` through IOKit's
registry-property API. `IOAudioControl` objects are attached below the engine
and may not be returned by a global class match. The app does not access PCI or
AC'97 registers directly.

## Build on Mac OS X 10.6.8

1. Load the matching `YMFPCI.kext`.
2. Open `YMF754Control.xcodeproj` in Xcode 4.2.
3. Select the `YMF754Control` scheme and build for x86_64.
4. Run `YMF754Control.app`. If the driver was loaded after the app opened, click
   **Повторить поиск**.

The driver publishes the custom toggle from the analog engine when the AC'97
codec is supported. The checkbox starts on by default, and its state is read back
from IORegistry after each change; write errors are shown rather than silently
accepted. The driver keeps the 3D setting analog only, so the app does not alter
the S/PDIF path.

This project was source-checked against the 10.6 SDK with Clang. It still needs
an Xcode 4.2 build and a hardware test on the target Mac.
