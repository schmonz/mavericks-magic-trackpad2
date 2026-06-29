// mt2_pane_watch — per-user LaunchAgent (Branch A of the standalone-osax delivery):
// inject MT2PaneRefresh into System Preferences when it launches, by sending the
// MT2x/load Apple event to its pid (the osax in /Library/ScriptingAdditions then
// loads and its constructor runs). Own process (NOT injected) -> normal ObjC/ARC.
#import <Foundation/Foundation.h>

static NSString * const kSysPrefsBundleID = @"com.apple.systempreferences";

BOOL mt2_should_inject(NSString *bundleID) {
    return bundleID != nil && [bundleID isEqualToString:kSysPrefsBundleID];
}
