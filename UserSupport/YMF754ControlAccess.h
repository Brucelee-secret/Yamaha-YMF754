/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * User-space access to the YMF754 3D Wide IOAudioControl.
 */
#ifndef YMF754_CONTROL_ACCESS_H
#define YMF754_CONTROL_ACCESS_H

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

io_registry_entry_t YMFFindThreeDWideControl(void);
IOReturn YMFReadThreeDWide(io_registry_entry_t controlEntry, SInt32 *value);
IOReturn YMFSetThreeDWide(io_registry_entry_t controlEntry, SInt32 value);

#endif
