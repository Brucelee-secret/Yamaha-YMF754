/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * System Preferences pane for the YMF754 analog 3D Wide control.
 */
#import <PreferencePanes/PreferencePanes.h>
#import <IOKit/IOKitLib.h>

@interface YMF754ThreeDWidePreferencePane : NSPreferencePane
{
@private
    NSButton *wideCheckbox;
    NSTextField *statusField;
    io_registry_entry_t controlEntry;
    BOOL currentValue;
}

- (NSView *)loadMainView;
- (void)refreshControl:(id)sender;
- (void)wideCheckboxChanged:(id)sender;

@end
