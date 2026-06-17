KEXT_NAME  = MT2Gesture
BUNDLE_ID  = com.schmonz.MT2Gesture

CXX = clang++
CXXFLAGS = -mmacosx-version-min=10.9 -fno-rtti -fno-exceptions -fapple-kext \
           -std=c++11 -Wno-deprecated-register -I../src \
           -I/System/Library/Frameworks/Kernel.framework/Headers \
           -I/System/Library/Frameworks/IOKit.framework/Headers
LDFLAGS  = -nostdlib -Wl,-kext -Wl,-no_uuid -lkmodc++ -lkmod

OBJS = $(KEXT_NAME).o MT2GestureUserClient.o MT2HIDShell.o \
       MT2BTReader.o mt2_bt_decode.o mt1_encode.o \
       MT2USBReader.o mt2_usb_decode.o \
       mt2_pipeline.o mt2_session.o

.PHONY: all clean load unload

all: $(KEXT_NAME).kext

%.o: %.cpp MT2Gesture.h amd_shim.h MT2HIDShell.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# The BT reader + the pure-C decoder/encoder shared with the USB path (compiled as C++
# under the kext toolchain; plain C99, no libc dependency).
MT2BTReader.o: MT2BTReader.cpp MT2BTReader.h bt_l2cap_shim.h MT2Gesture.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<
mt2_bt_decode.o: ../src/mt2_bt_decode.c ../src/mt2_bt_decode.h ../src/touch_model.h
	$(CXX) $(CXXFLAGS) -x c++ -c -o $@ $<
mt1_encode.o: ../src/mt1_encode.c ../src/mt1_encode.h ../src/touch_model.h
	$(CXX) $(CXXFLAGS) -x c++ -c -o $@ $<
# The in-kernel USB reader + the pure-C USB decoder shared with the (now removed)
# userspace path (compiled as C++ under the kext toolchain).
# -DKERNEL=1: the Kernel.framework USB headers gate the kernel-only
# IOUSBDevRequestDesc (memory-descriptor control request) behind #if KERNEL.
# Scoped to this object so the working BT/gesture path compiles unchanged.
MT2USBReader.o: MT2USBReader.cpp MT2USBReader.h MT2Gesture.h
	$(CXX) $(CXXFLAGS) -DKERNEL=1 -c -o $@ $<
mt2_usb_decode.o: ../src/mt2_usb_decode.c ../src/mt2_usb_decode.h ../src/touch_model.h
	$(CXX) $(CXXFLAGS) -x c++ -c -o $@ $<
mt2_pipeline.o: ../src/mt2_pipeline.c ../src/mt2_pipeline.h ../src/touch_model.h
	$(CXX) $(CXXFLAGS) -x c++ -c -o $@ $<
mt2_session.o: ../src/mt2_session.c ../src/mt2_session.h ../src/mt2_pipeline.h ../src/touch_model.h
	$(CXX) $(CXXFLAGS) -x c++ -c -o $@ $<

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
