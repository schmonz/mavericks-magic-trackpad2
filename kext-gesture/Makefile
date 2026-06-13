KEXT_NAME  = MT2Gesture
BUNDLE_ID  = com.schmonz.MT2Gesture

CXX = clang++
CXXFLAGS = -mmacosx-version-min=10.9 -fno-rtti -fno-exceptions -fapple-kext \
           -std=c++11 -Wno-deprecated-register \
           -I/System/Library/Frameworks/Kernel.framework/Headers \
           -I/System/Library/Frameworks/IOKit.framework/Headers
LDFLAGS  = -nostdlib -Wl,-kext -Wl,-no_uuid -lkmodc++ -lkmod

OBJS = $(KEXT_NAME).o MT2GestureUserClient.o

.PHONY: all clean load unload

all: $(KEXT_NAME).kext

%.o: %.cpp MT2Gesture.h amd_shim.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(KEXT_NAME).kext: $(OBJS) Info.plist
	mkdir -p $(KEXT_NAME).kext/Contents/MacOS
	$(CXX) $(LDFLAGS) -o $(KEXT_NAME).kext/Contents/MacOS/$(KEXT_NAME) $(OBJS)
	cp Info.plist $(KEXT_NAME).kext/Contents/
	chmod -R 755 $(KEXT_NAME).kext

load: $(KEXT_NAME).kext
	rm -rf /tmp/$(KEXT_NAME).kext
	cp -R $(KEXT_NAME).kext /tmp/
	sudo chown -R root:wheel /tmp/$(KEXT_NAME).kext
	sudo kextload /tmp/$(KEXT_NAME).kext

unload:
	-sudo kextunload -b $(BUNDLE_ID)

clean:
	rm -rf $(KEXT_NAME).kext $(OBJS)
