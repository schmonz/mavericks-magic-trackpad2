KEXT_NAME  = MT2Claim
BUNDLE_ID  = com.schmonz.MT2Claim

CXX = clang++
# On Mavericks, kernel headers live in /System, not in an SDK.
CXXFLAGS = -mmacosx-version-min=10.9 -fno-rtti -fno-exceptions -fapple-kext \
           -std=c++11 -Wno-deprecated-register \
           -I/System/Library/Frameworks/Kernel.framework/Headers \
           -I/System/Library/Frameworks/IOKit.framework/Headers
LDFLAGS  = -nostdlib -Wl,-kext -Wl,-no_uuid -lkmodc++ -lkmod

.PHONY: all clean load unload

all: $(KEXT_NAME).kext

$(KEXT_NAME).o: $(KEXT_NAME).cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(KEXT_NAME).kext: $(KEXT_NAME).o Info.plist
	mkdir -p $(KEXT_NAME).kext/Contents/MacOS
	$(CXX) $(LDFLAGS) -o $(KEXT_NAME).kext/Contents/MacOS/$(KEXT_NAME) $(KEXT_NAME).o
	cp Info.plist $(KEXT_NAME).kext/Contents/
	chmod -R 755 $(KEXT_NAME).kext

# Load from a root-owned copy in /tmp (kextload requires root:wheel ownership).
load: $(KEXT_NAME).kext
	rm -rf /tmp/$(KEXT_NAME).kext
	cp -R $(KEXT_NAME).kext /tmp/
	sudo chown -R root:wheel /tmp/$(KEXT_NAME).kext
	sudo kextload /tmp/$(KEXT_NAME).kext

unload:
	-sudo kextunload -b $(BUNDLE_ID)

clean:
	rm -rf $(KEXT_NAME).kext $(KEXT_NAME).o
