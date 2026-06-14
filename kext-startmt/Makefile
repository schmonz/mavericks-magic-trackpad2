KEXT_NAME  = MT2StartMT
BUNDLE_ID  = com.schmonz.MT2StartMT

CXX = clang++
CXXFLAGS = -mmacosx-version-min=10.9 -fno-rtti -fno-exceptions -fapple-kext \
           -std=c++11 -Wno-deprecated-register \
           -I/System/Library/Frameworks/Kernel.framework/Headers \
           -I/System/Library/Frameworks/IOKit.framework/Headers
LDFLAGS  = -nostdlib -Wl,-kext -Wl,-no_uuid -lkmodc++ -lkmod

OBJS = $(KEXT_NAME).o

.PHONY: all clean

all: $(KEXT_NAME).kext

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(KEXT_NAME).kext: $(OBJS) Info.plist
	mkdir -p $(KEXT_NAME).kext/Contents/MacOS
	$(CXX) $(LDFLAGS) -o $(KEXT_NAME).kext/Contents/MacOS/$(KEXT_NAME) $(OBJS)
	cp Info.plist $(KEXT_NAME).kext/Contents/
	chmod -R 755 $(KEXT_NAME).kext

clean:
	rm -rf $(KEXT_NAME).kext $(OBJS)
