#!/bin/sh
# Force the installer to place our bundles regardless of the on-disk CFBundleVersion.
#
# ROOT CAUSE (2026-07-09, proven from /var/log/install.log): the legacy kext hardcoded
# CFBundleVersion 1.0.0. PackageKit's default (BundleIsVersionChecked=true) compares the pkg's
# bundle version against what's on disk and SKIPS the component if the installed one is >= the
# pkg's. With the on-disk kext at "1.0.0" and the pkg's at "0.4.4", it read 1.0.0 > 0.4.4 and
# logged: PackageKit: Skipping component "com.schmonz.MavericksVoodooInputHost" ... because the version 1.0.0
# is already installed. Net effect: the KEXT (the driver itself) never updated via the installer.
#
# Setting BundleIsVersionChecked=false on every component makes the installer always overwrite with
# the version in the pkg — the correct behavior for a driver package (install what's in the pkg,
# never defer to a stale on-disk version). pkgbuild --analyze emits an array of component dicts; we
# flip the flag on each. PlistBuddy (not python) so it runs on the 10.9 dev box AND macOS-26 CI.
set -e
PLIST="$1"
[ -f "$PLIST" ] || { echo "pkg_no_version_check: no such plist: $PLIST" >&2; exit 1; }

i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$PLIST" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsVersionChecked false" "$PLIST" 2>/dev/null \
        || /usr/libexec/PlistBuddy -c "Add :$i:BundleIsVersionChecked bool false" "$PLIST"
    i=$((i + 1))
done
[ "$i" -gt 0 ] || { echo "pkg_no_version_check: no bundle components found in $PLIST" >&2; exit 1; }
echo "pkg: disabled installer version-check on $i bundle component(s)"
