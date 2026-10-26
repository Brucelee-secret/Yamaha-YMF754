#import <Cocoa/Cocoa.h>
#import <IOKit/IOKitLib.h>
#import "../UserSupport/YMF754ControlAccess.h"

@interface YMF754ControlAppDelegate : NSObject <NSApplicationDelegate>
{
    NSWindow *window;
    NSButton *wideCheckbox;
    NSTextField *statusField;
    io_registry_entry_t controlEntry;
    BOOL currentValue;
}
- (void)refreshControl:(id)sender;
- (void)wideCheckboxChanged:(id)sender;
@end

@implementation YMF754ControlAppDelegate

- (id)init
{
    self = [super init];
    if (self) {
        window = nil;
        wideCheckbox = nil;
        statusField = nil;
        controlEntry = IO_OBJECT_NULL;
        currentValue = NO;
    }
    return self;
}

- (void)dealloc
{
    if (controlEntry != IO_OBJECT_NULL)
        IOObjectRelease(controlEntry);
    [window release];
    [super dealloc];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    NSRect frame = NSMakeRect(0, 0, 390, 170);
    window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSTitledWindowMask | NSClosableWindowMask)
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"YMF754 — управление 3D Wide"];
    [window setReleasedWhenClosed:NO];
    [window center];

    NSView *content = [window contentView];
    wideCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(22, 119, 340, 24)];
    [wideCheckbox setButtonType:NSSwitchButton];
    [wideCheckbox setTitle:@"3D Wide — аналоговый rear out"];
    [wideCheckbox setTarget:self];
    [wideCheckbox setAction:@selector(wideCheckboxChanged:)];
    [wideCheckbox setEnabled:NO];
    [content addSubview:wideCheckbox];
    [wideCheckbox release];

    NSTextField *note = [[NSTextField alloc] initWithFrame:NSMakeRect(22, 80, 346, 30)];
    [note setStringValue:@"Переключатель не меняет цифровой S/PDIF-выход."];
    [note setEditable:NO];
    [note setSelectable:NO];
    [note setBezeled:NO];
    [note setDrawsBackground:NO];
    [note setFont:[NSFont systemFontOfSize:12]];
    [content addSubview:note];
    [note release];

    statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(22, 42, 346, 32)];
    [statusField setStringValue:@"Поиск аудиоконтроля…"];
    [statusField setEditable:NO];
    [statusField setSelectable:NO];
    [statusField setBezeled:NO];
    [statusField setDrawsBackground:NO];
    [statusField setFont:[NSFont systemFontOfSize:11]];
    [content addSubview:statusField];
    [statusField release];

    NSButton *refresh = [[NSButton alloc] initWithFrame:NSMakeRect(226, 10, 142, 24)];
    [refresh setTitle:@"Повторить поиск"];
    [refresh setTarget:self];
    [refresh setAction:@selector(refreshControl:)];
    [content addSubview:refresh];
    [refresh release];

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self refreshControl:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification
{
    (void)notification;
    if (controlEntry != IO_OBJECT_NULL) {
        IOObjectRelease(controlEntry);
        controlEntry = IO_OBJECT_NULL;
    }
}

- (BOOL)readCurrentValue:(BOOL *)value
{
    if (controlEntry == IO_OBJECT_NULL || !value)
        return NO;

    SInt32 rawValue = 0;
    if (YMFReadThreeDWide(controlEntry, &rawValue) != kIOReturnSuccess)
        return NO;
    *value = rawValue != 0;
    return YES;
}

- (void)refreshControl:(id)sender
{
    (void)sender;
    if (controlEntry != IO_OBJECT_NULL) {
        IOObjectRelease(controlEntry);
        controlEntry = IO_OBJECT_NULL;
    }
    [wideCheckbox setEnabled:NO];
    [statusField setStringValue:@"Поиск аудиоконтроля…"];

    controlEntry = YMFFindThreeDWideControl();

    if (controlEntry == IO_OBJECT_NULL) {
        [statusField setStringValue:
            @"Переключатель не найден под аудиодвижком YMF754. Проверьте, что загружен обновлённый YMFPCI.kext."];
        return;
    }

    if (![self readCurrentValue:&currentValue]) {
        [statusField setStringValue:
            @"Контрол найден, но состояние прочитать не удалось."];
        IOObjectRelease(controlEntry);
        controlEntry = IO_OBJECT_NULL;
        return;
    }

    [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
    [wideCheckbox setEnabled:YES];
    [statusField setStringValue:[NSString stringWithFormat:
        @"Состояние: %@", currentValue ? @"включено" : @"выключено"]];
}

- (void)wideCheckboxChanged:(id)sender
{
    (void)sender;
    if (controlEntry == IO_OBJECT_NULL) {
        [self refreshControl:nil];
        return;
    }

    SInt32 requestedValue = [wideCheckbox state] == NSOnState ? 1 : 0;
    const IOReturn result = YMFSetThreeDWide(controlEntry, requestedValue);

    if (result != kIOReturnSuccess) {
        [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
        [statusField setStringValue:[NSString stringWithFormat:
            @"Драйвер отклонил переключение: 0x%08x", result]];
        return;
    }

    BOOL readback = NO;
    if (![self readCurrentValue:&readback] || readback != (requestedValue != 0)) {
        [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
        [statusField setStringValue:
            @"Драйвер не подтвердил новое состояние. Нажмите «Повторить поиск»."];
        return;
    }

    currentValue = readback;
    [wideCheckbox setState:currentValue ? NSOnState : NSOffState];
    [statusField setStringValue:[NSString stringWithFormat:
        @"Состояние: %@", currentValue ? @"включено" : @"выключено"]];
}

@end

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSApplication *application = [NSApplication sharedApplication];
    YMF754ControlAppDelegate *delegate = [[YMF754ControlAppDelegate alloc] init];
    [application setDelegate:delegate];
    [application run];
    [application setDelegate:nil];
    [delegate release];
    [pool drain];
    return 0;
}
