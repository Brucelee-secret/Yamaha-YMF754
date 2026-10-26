/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * A programmatic NSPreferencePane for Mac OS X 10.6.
 */
#import <Cocoa/Cocoa.h>
#import <IOKit/IOKitLib.h>
#import "../UserSupport/YMF754ControlAccess.h"
#import "YMF754ThreeDWidePreferencePane.h"

@implementation YMF754ThreeDWidePreferencePane

- (id)initWithBundle:(NSBundle *)bundle
{
    self = [super initWithBundle:bundle];
    if (self) {
        wideCheckbox = nil;
        statusField = nil;
        controlEntry = IO_OBJECT_NULL;
        currentValue = NO;
    }
    return self;
}

- (void)dealloc
{
    if (controlEntry != IO_OBJECT_NULL) {
        IOObjectRelease(controlEntry);
        controlEntry = IO_OBJECT_NULL;
    }
    [super dealloc];
}

- (NSView *)loadMainView
{
    NSView *view = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 210)]
        autorelease];
    [view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [self setMainView:view];

    wideCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(28, 150, 500, 26)];
    [wideCheckbox setButtonType:NSSwitchButton];
    [wideCheckbox setTitle:@"3D Wide — аналоговый rear out"];
    [wideCheckbox setTarget:self];
    [wideCheckbox setAction:@selector(wideCheckboxChanged:)];
    [wideCheckbox setEnabled:NO];
    [view addSubview:wideCheckbox];
    [wideCheckbox release];

    NSTextField *note = [[NSTextField alloc] initWithFrame:NSMakeRect(28, 104, 500, 28)];
    [note setStringValue:@"Эффект действует только на аналоговый rear out; S/PDIF не изменяется."];
    [note setEditable:NO];
    [note setSelectable:NO];
    [note setBezeled:NO];
    [note setDrawsBackground:NO];
    [note setFont:[NSFont systemFontOfSize:12]];
    [view addSubview:note];
    [note release];

    statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(28, 58, 500, 38)];
    [statusField setStringValue:@"Поиск переключателя YMF754…"];
    [statusField setEditable:NO];
    [statusField setSelectable:NO];
    [statusField setBezeled:NO];
    [statusField setDrawsBackground:NO];
    [statusField setFont:[NSFont systemFontOfSize:11]];
    [view addSubview:statusField];
    [statusField release];

    NSButton *refresh = [[NSButton alloc] initWithFrame:NSMakeRect(382, 18, 146, 28)];
    [refresh setTitle:@"Повторить поиск"];
    [refresh setTarget:self];
    [refresh setAction:@selector(refreshControl:)];
    [view addSubview:refresh];
    [refresh release];

    [self setInitialKeyView:wideCheckbox];
    [self setFirstKeyView:wideCheckbox];
    [self setLastKeyView:refresh];
    [self refreshControl:nil];
    return view;
}

- (void)didSelect
{
    [super didSelect];
    [self refreshControl:nil];
}

- (void)refreshControl:(id)sender
{
    (void)sender;
    if (controlEntry != IO_OBJECT_NULL) {
        IOObjectRelease(controlEntry);
        controlEntry = IO_OBJECT_NULL;
    }

    [wideCheckbox setEnabled:NO];
    [statusField setStringValue:@"Поиск переключателя YMF754…"];
    controlEntry = YMFFindThreeDWideControl();
    if (controlEntry == IO_OBJECT_NULL) {
        [statusField setStringValue:
            @"Контрол не найден под аудиодвижком. Нужен обновлённый YMFPCI.kext."];
        return;
    }

    SInt32 value = 0;
    IOReturn result = YMFReadThreeDWide(controlEntry, &value);
    if (result != kIOReturnSuccess) {
        [statusField setStringValue:[NSString stringWithFormat:
            @"Не удалось прочитать состояние: 0x%08x", result]];
        IOObjectRelease(controlEntry);
        controlEntry = IO_OBJECT_NULL;
        return;
    }

    currentValue = value != 0;
    [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
    [wideCheckbox setEnabled:YES];
    [statusField setStringValue:[NSString stringWithFormat:
        @"3D Wide %@", currentValue ? @"включён" : @"выключен"]];
}

- (void)wideCheckboxChanged:(id)sender
{
    (void)sender;
    if (controlEntry == IO_OBJECT_NULL) {
        [self refreshControl:nil];
        return;
    }

    const SInt32 requestedValue =
        [wideCheckbox state] == NSOnState ? 1 : 0;
    IOReturn result = YMFSetThreeDWide(controlEntry, requestedValue);
    if (result != kIOReturnSuccess) {
        [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
        [statusField setStringValue:[NSString stringWithFormat:
            @"Драйвер отклонил переключение: 0x%08x", result]];
        return;
    }

    SInt32 readback = 0;
    result = YMFReadThreeDWide(controlEntry, &readback);
    if (result != kIOReturnSuccess || readback != requestedValue) {
        [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
        [statusField setStringValue:
            @"Драйвер не подтвердил новое состояние. Повторите поиск."];
        return;
    }

    currentValue = readback != 0;
    [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
    [statusField setStringValue:[NSString stringWithFormat:
        @"3D Wide %@", currentValue ? @"включён" : @"выключен"]];
}

@end
