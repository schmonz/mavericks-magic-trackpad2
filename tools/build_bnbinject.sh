#!/bin/bash
# build_bnbinject.sh - assemble the codeless kext that injects a BNBTrackpadDevice
# personality retargeted to the Magic Trackpad 2's REAL Bluetooth identity, so Apple's
# real BT multitouch transport (BNBTrackpadDevice) binds the MT2's live L2CAP channel.
#
# This drove Apple's stock stack all the way to the MT1 handshake with a real MT2
# (match -> load -> open channels -> init -> waitForHandshake). Remaining wall: the
# MT2 speaks a different init/handshake protocol than the MT1 BNBTrackpadDevice expects.
#
# Run ON THE TARGET (needs the stock AppleBluetoothMultitouch.kext). Then:
#   sudo kextload /tmp/BNBInject.kext        # inject personality
#   # disconnect+reconnect the MT2 over Bluetooth so BNBTrackpadDevice wins the channels
#   sudo dmesg | grep BNB                    # watch handshake progress
# A reboot clears it (transient /tmp codeless kext); the IOCatalogue remove API returns
# kIOReturnNoMemory, so reboot is the clean removal.
set -e
HERE="$(cd "$(dirname "$0")"/.. && pwd)"
STOCK=/System/Library/Extensions/AppleBluetoothMultitouch.kext/Contents/Info.plist
PB=/usr/libexec/PlistBuddy

# 1. clone the stock BNBTrackpadDriver personality, retarget identity to the MT2's BT values
$PB -x -c "Print :IOKitPersonalities:BNBTrackpadDriver" "$STOCK" > /tmp/pers.plist
$PB -c "Set :VendorID 76"        /tmp/pers.plist   # 0x4C BT-SIG Apple (stock: 1452 USB)
$PB -c "Set :ProductID 613"      /tmp/pers.plist   # 0x0265 MT2     (stock: 782 MT1)
$PB -c "Set :VendorIDSource 1"   /tmp/pers.plist   # 1 BT-SIG       (stock: 2 USB-IF)
$PB -c "Add :IOProbeScore integer 100000" /tmp/pers.plist  # beat generic IOBluetoothHIDDriver

# 2. build the codeless-kext Info.plist from the base shell + the retargeted personality
cp "$HERE/kext-bnbinject/Info.plist" /tmp/BNBInfo.plist
$PB -c "Delete :IOKitPersonalities:MT2BTTrackpad" /tmp/BNBInfo.plist || true
$PB -c "Add :IOKitPersonalities:MT2BTTrackpad dict" /tmp/BNBInfo.plist
$PB -c "Merge /tmp/pers.plist :IOKitPersonalities:MT2BTTrackpad" /tmp/BNBInfo.plist

# 3. assemble the kext bundle
sudo rm -rf /tmp/BNBInject.kext
mkdir -p /tmp/BNBInject.kext/Contents
cp /tmp/BNBInfo.plist /tmp/BNBInject.kext/Contents/Info.plist
sudo chown -R root:wheel /tmp/BNBInject.kext
echo "built /tmp/BNBInject.kext"
sudo kextutil -n /tmp/BNBInject.kext 2>&1 | tail -1
