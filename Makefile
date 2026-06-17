CC      = clang
CFLAGS  = -Wall -Wextra -Wno-missing-field-initializers -O2 -std=c99
FRAMEWORKS = -framework IOKit -framework CoreFoundation
SRC = src
VERSION = 1.0.0
PKG_ID  = com.schmonz.mt2d

.PHONY: all test clean tools kext-gesture pkg

all: tools

tools: vhid_probe mt2_reenumerate

vhid_probe: tools/vhid_probe.c $(SRC)/vhid_mt1.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

mt2_reenumerate: tools/mt2_reenumerate.c
	$(CC) $(CFLAGS) -o $@ $< $(FRAMEWORKS)

# Build the gesture kext (delegates to its own makefile).
kext-gesture:
	$(MAKE) -C kext-gesture

# Assemble an installer. The unsigned kext goes under /usr/local/lib/mt2d (NOT
# /Library/Extensions, which enforces signing); the launchd wrapper kextloads it
# from there. Root-run binaries + wrapper -> /usr/local/sbin, LaunchDaemon -> /Library.
pkg: mt2_reenumerate kext-gesture
	rm -rf build/pkgroot build/scripts
	mkdir -p build/pkgroot/usr/local/lib/mt2d
	mkdir -p build/pkgroot/usr/local/sbin
	mkdir -p build/pkgroot/Library/LaunchDaemons
	cp -R kext-gesture/MT2Gesture.kext build/pkgroot/usr/local/lib/mt2d/
	cp mt2_reenumerate dist/mt2d-run build/pkgroot/usr/local/sbin/
	chmod +x build/pkgroot/usr/local/sbin/mt2d-run
	cp dist/com.schmonz.mt2d.plist build/pkgroot/Library/LaunchDaemons/
	cp -R dist/scripts build/scripts
	chmod +x build/scripts/preinstall build/scripts/postinstall
	pkgbuild --root build/pkgroot --scripts build/scripts \
	  --identifier $(PKG_ID) --version $(VERSION) --install-location / \
	  build/mt2d-$(VERSION).pkg
	@echo "Built build/mt2d-$(VERSION).pkg"

# Unit tests are pure C, no frameworks needed.
TESTS = test_model test_decode test_bt_decode test_encode test_pipeline test_session
test: $(TESTS)
	@fail=0; for t in $(TESTS); do echo "== $$t =="; ./$$t || fail=1; done; \
	 echo "== test_mt2d_run.sh =="; sh tests/test_mt2d_run.sh || fail=1; \
	 [ $$fail -eq 0 ] && echo "ALL TESTS PASS"

test_model: tests/test_model.c
	$(CC) $(CFLAGS) -o $@ $<
test_decode: tests/test_decode.c $(SRC)/mt2_usb_decode.c
	$(CC) $(CFLAGS) -o $@ $^
test_bt_decode: tests/test_bt_decode.c $(SRC)/mt2_bt_decode.c
	$(CC) $(CFLAGS) -o $@ $^
test_encode: tests/test_encode.c $(SRC)/mt1_encode.c
	$(CC) $(CFLAGS) -o $@ $^
test_pipeline: tests/test_pipeline.c $(SRC)/mt2_pipeline.c
	$(CC) $(CFLAGS) -o $@ $^
test_session: tests/test_session.c $(SRC)/mt2_session.c $(SRC)/mt2_pipeline.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f vhid_probe mt2_reenumerate test_gesture $(TESTS) *.o
	rm -rf build
