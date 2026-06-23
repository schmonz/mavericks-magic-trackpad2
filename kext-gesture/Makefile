KEXT_NAME  = MT2Gesture
BUNDLE_ID  = com.schmonz.MT2Gesture

CXX = clang++
# -MMD -MP: the compiler emits a .d file per object listing every header that
# source actually #includes, and we -include them below. Header edits then trigger
# the right rebuilds AUTOMATICALLY -- no hand-maintained prerequisite lists to drift
# out of sync (the old lists silently missed headers, producing stale binaries).
# -MP adds phantom targets so deleting a header doesn't wedge the build.
CXXFLAGS = -mmacosx-version-min=10.9 -fno-rtti -fno-exceptions -fapple-kext \
           -std=c++11 -Wno-deprecated-register -MMD -MP -I../src \
           -I/System/Library/Frameworks/Kernel.framework/Headers \
           -I/System/Library/Frameworks/IOKit.framework/Headers
LDFLAGS  = -nostdlib -Wl,-kext -Wl,-no_uuid -lkmodc++ -lkmod

OBJS = $(KEXT_NAME).o MT2HIDShell.o MT2GestureUserClient.o \
       MT2BTReader.o mt2_bt_decode.o mt1_encode.o mt2_to_mt1.o \
       MT2USBReader.o mt2_usb_decode.o mt2_decode.o \
       mt2_pipeline.o mt2_lifecycle.o mt2_session.o mt2_geometry.o mt2_log.o

.PHONY: all clean load unload

all: $(KEXT_NAME).kext

# Local C++ sources. Header prerequisites come from the generated .d files.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# MT2USBReader needs KERNEL=1: the Kernel.framework USB headers gate the kernel-only
# IOUSBDevRequestDesc control-request API behind #if KERNEL. Scoped to this object so
# the BT/gesture path compiles unchanged. (Explicit recipe overrides the pattern above.)
MT2USBReader.o: MT2USBReader.cpp
	$(CXX) $(CXXFLAGS) -DKERNEL=1 -c -o $@ $<

# Shared pure-C sources from ../src (the decode/encode/session core the USB path also
# uses), compiled as C++ under the kext toolchain; plain C99, no libc dependency.
%.o: ../src/%.c
	$(CXX) $(CXXFLAGS) -x c++ -c -o $@ $<

$(KEXT_NAME).kext: $(OBJS) Info.plist
	mkdir -p $(KEXT_NAME).kext/Contents/MacOS
	$(CXX) $(LDFLAGS) -o $(KEXT_NAME).kext/Contents/MacOS/$(KEXT_NAME) $(OBJS)
	cp Info.plist $(KEXT_NAME).kext/Contents/
	chmod -R 755 $(KEXT_NAME).kext

# kextload refuses any kext not owned by root:wheel, so we stage a copy in /tmp and
# chown it. Every step that touches that staged copy needs root: the prior load left
# it root-owned, so even the cleanup rm must be sudo. Keeping all the privileged steps
# under sudo makes this work as a plain `make load` AND as `sudo make load` (nested
# sudo is harmless) — pick either, both are idempotent.
load: $(KEXT_NAME).kext
	sudo rm -rf /tmp/$(KEXT_NAME).kext
	sudo cp -R $(KEXT_NAME).kext /tmp/
	sudo chown -R root:wheel /tmp/$(KEXT_NAME).kext
	sudo kextload /tmp/$(KEXT_NAME).kext

unload:
	-sudo kextunload -b $(BUNDLE_ID)

clean:
	rm -rf $(KEXT_NAME).kext $(OBJS) $(OBJS:.o=.d)

# Pull in the auto-generated header dependencies (silent on first build / after clean).
-include $(OBJS:.o=.d)
