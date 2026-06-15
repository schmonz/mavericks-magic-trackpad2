CC      = clang
CFLAGS  = -Wall -Wextra -Wno-missing-field-initializers -O2 -std=c99
FRAMEWORKS = -framework IOKit -framework CoreFoundation
SRC = src
VERSION = 1.0.0
PKG_ID  = com.schmonz.mt2d

.PHONY: all test clean daemon tools kext kext-gesture pkg

all: daemon tools

daemon: mt2d
tools: dump_frames vhid_probe mt2_reenumerate mt2_gesture_feed

# Shipping daemon: synthesize cursor/scroll/click from touches (works today).
mt2d: $(SRC)/mt2d.c $(SRC)/mt2_reader.c $(SRC)/mt2_decode.c $(SRC)/gesture.c $(SRC)/vhid_mouse.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

# Research daemon: feed MT1 reports to a fake-MT1 IOHIDUserDevice (gesture-engine
# path; binds AppleMultitouchHIDEventDriver but no MultitouchDevice yet).
mt2d_mt1: $(SRC)/mt2d_mt1.c $(SRC)/mt2_reader.c $(SRC)/mt2_decode.c $(SRC)/mt1_encode.c $(SRC)/vhid_mt1.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

dump_frames: tools/dump_frames.c $(SRC)/mt2_reader.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

vhid_probe: tools/vhid_probe.c $(SRC)/vhid_mt1.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

mt2_reenumerate: tools/mt2_reenumerate.c
	$(CC) $(CFLAGS) -o $@ $< $(FRAMEWORKS)

# Milestone 4 feeder: MT2 frames -> MT1 reports -> MT2Gesture kext user client.
mt2_gesture_feed: tools/mt2_gesture_feed.c $(SRC)/mt2_reader.c $(SRC)/mt2_decode.c $(SRC)/mt1_encode.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

# Build the interface-claim kext (delegates to its own makefile).
kext:
	$(MAKE) -C kext

# Build the gesture kext (delegates to its own makefile).
kext-gesture:
	$(MAKE) -C kext-gesture

# Assemble an installer. The unsigned kext goes under /usr/local/lib/mt2d (NOT
# /Library/Extensions, which enforces signing); the launchd wrapper kextloads it
# from there. Root-run binaries + wrapper -> /usr/local/sbin, LaunchDaemon -> /Library.
pkg: mt2d mt2_reenumerate kext
	rm -rf build/pkgroot build/scripts
	mkdir -p build/pkgroot/usr/local/lib/mt2d
	mkdir -p build/pkgroot/usr/local/sbin
	mkdir -p build/pkgroot/Library/LaunchDaemons
	cp -R kext/MT2Claim.kext build/pkgroot/usr/local/lib/mt2d/
	cp mt2d mt2_reenumerate dist/mt2d-run build/pkgroot/usr/local/sbin/
	chmod +x build/pkgroot/usr/local/sbin/mt2d-run
	cp dist/com.schmonz.mt2d.plist build/pkgroot/Library/LaunchDaemons/
	cp -R dist/scripts build/scripts
	chmod +x build/scripts/preinstall build/scripts/postinstall
	pkgbuild --root build/pkgroot --scripts build/scripts \
	  --identifier $(PKG_ID) --version $(VERSION) --install-location / \
	  build/mt2d-$(VERSION).pkg
	@echo "Built build/mt2d-$(VERSION).pkg"

# Unit tests are pure C, no frameworks needed.
TESTS = test_model test_decode test_encode test_gesture
test: $(TESTS)
	@fail=0; for t in $(TESTS); do echo "== $$t =="; ./$$t || fail=1; done; \
	 [ $$fail -eq 0 ] && echo "ALL TESTS PASS"

test_model: tests/test_model.c
	$(CC) $(CFLAGS) -o $@ $<
test_decode: tests/test_decode.c $(SRC)/mt2_decode.c
	$(CC) $(CFLAGS) -o $@ $^
test_encode: tests/test_encode.c $(SRC)/mt1_encode.c
	$(CC) $(CFLAGS) -o $@ $^
test_gesture: tests/test_gesture.c $(SRC)/gesture.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f mt2d mt2d_mt1 dump_frames vhid_probe mt2_reenumerate mt2_gesture_feed $(TESTS) *.o
	rm -rf build
	$(MAKE) -C kext clean 2>/dev/null || true
