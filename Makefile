CC      = clang
CFLAGS  = -Wall -Wextra -Wno-missing-field-initializers -O2 -std=c99
FRAMEWORKS = -framework IOKit -framework CoreFoundation
SRC = src
# built command-line tools live here (gitignored), not the repo root
SBIN = sbin
VERSION = 1.0.0
PKG_ID  = com.schmonz.mt2d

.PHONY: all test clean tools kext-gesture pkg reload prefpane-refresh prefpane-refresh-install prefpane-refresh-uninstall prefpane-refresh-simbl prefpane-refresh-simbl-install prefpane-refresh-simbl-uninstall

all: tools

tools: $(SBIN)/vhid_probe $(SBIN)/mt2_reenumerate $(SBIN)/mt2_set_btname $(SBIN)/mt2_bt_bounce

$(SBIN):
	@mkdir -p $@

$(SBIN)/vhid_probe: tools/vhid_probe.c $(SRC)/vhid_mt1.c | $(SBIN)
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

$(SBIN)/mt2_reenumerate: tools/mt2_reenumerate.c | $(SBIN)
	$(CC) $(CFLAGS) -o $@ $< $(FRAMEWORKS)

# ObjC (IOBluetooth): give the paired MT2 a proper Bluetooth-prefpane name via setDisplayName:.
# 10.9 mis-fetches the device name (stores garbage 0x02 0x01); displayName is the user-override key
# blued keeps. Picture stays generic (a 10.9 limitation; see docs/mt-stack).
$(SBIN)/mt2_set_btname: tools/mt2_set_btname.m | $(SBIN)
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< -framework Foundation -framework IOBluetooth

# ObjC (IOBluetooth): force a clean BT re-establish of the paired MT2 (closeConnection->openConnection),
# the Bluetooth twin of mt2_reenumerate. Used by `make reload` so a hot reload doesn't strand the device
# on a stale link (the device never re-opens PSM 19 -> BNB manual-start fails until a manual tap).
$(SBIN)/mt2_bt_bounce: tools/mt2_bt_bounce.m | $(SBIN)
	$(CC) $(CFLAGS) -fobjc-arc -o $@ $< -framework Foundation -framework IOBluetooth

$(SBIN)/mt2_usb_enable: tools/mt2_usb_enable.c | $(SBIN)
	$(CC) $(CFLAGS) -o $@ $< $(FRAMEWORKS)

# Trackpad prefpane live USB-observer: a pure-C GC-neutral .osax injected into
# System Preferences (gives the open pane the AppleUSBMultitouchDriver observer it
# lacks) + mt2_pane_arm (sends the 'MT2x'/'load' Apple event to trigger it).
# See tools/mt2_prefpane_refresh/ and docs/mt-stack/open-questions.md.
OSAX_DIR    = tools/mt2_prefpane_refresh
OSAX_BUNDLE = build/MT2PaneRefresh.osax
OSAX_DYLIB  = build/MT2PaneRefresh.dylib
prefpane-refresh: $(OSAX_BUNDLE) $(OSAX_DYLIB) $(SBIN)/mt2_pane_arm

# Same payload as a DYLD_INSERT_LIBRARIES dylib — a loader we control at launch
# (the .osax/AppleEvent path does not reliably load into Cocoa System Preferences).
$(OSAX_DYLIB): $(OSAX_DIR)/mt2_prefpane_refresh.c
	@mkdir -p build
	$(CC) -dynamiclib -mmacosx-version-min=10.9 -Wall -framework IOKit -framework CoreFoundation -lobjc \
	  -o $(OSAX_DYLIB) $(OSAX_DIR)/mt2_prefpane_refresh.c
	@echo "Built $(OSAX_DYLIB)"

$(OSAX_BUNDLE): $(OSAX_DIR)/mt2_prefpane_refresh.c $(OSAX_DIR)/Info.plist
	@mkdir -p $(OSAX_BUNDLE)/Contents/MacOS
	$(CC) -bundle -mmacosx-version-min=10.9 -Wall -framework IOKit -framework CoreFoundation -lobjc \
	  -o $(OSAX_BUNDLE)/Contents/MacOS/MT2PaneRefresh $(OSAX_DIR)/mt2_prefpane_refresh.c
	cp $(OSAX_DIR)/Info.plist $(OSAX_BUNDLE)/Contents/Info.plist
	@echo "Built $(OSAX_BUNDLE)"

$(SBIN)/mt2_pane_arm: $(OSAX_DIR)/mt2_pane_arm.c | $(SBIN)
	$(CC) $(CFLAGS) -o $@ $< -framework ApplicationServices -framework CoreFoundation

# Install/uninstall the osax (system-wide scripting addition). Fully reversible:
# uninstall removes the one bundle, modifying no Apple file. Privilege handled here.
prefpane-refresh-install: $(OSAX_BUNDLE)
	sudo rm -rf /Library/ScriptingAdditions/MT2PaneRefresh.osax
	sudo cp -R $(OSAX_BUNDLE) /Library/ScriptingAdditions/
	@echo "Installed /Library/ScriptingAdditions/MT2PaneRefresh.osax"

prefpane-refresh-uninstall:
	sudo rm -rf /Library/ScriptingAdditions/MT2PaneRefresh.osax
	@echo "Removed /Library/ScriptingAdditions/MT2PaneRefresh.osax"

# Package the SAME pure-C payload as a SIMBL plugin bundle. SIMBLAgent (already
# running on this box) injects it into System Preferences at launch — the reliable
# loader, reusing observably-working machinery (the osax/AppleEvent path does not
# load into Cocoa System Preferences). The payload's constructor does the work on
# [bundle load], so no Apple event is needed.
SIMBL_PLUGINS = /Library/Application Support/SIMBL/Plugins
OSAX_SIMBL    = build/MT2PaneRefresh.bundle
prefpane-refresh-simbl: $(OSAX_SIMBL)

$(OSAX_SIMBL): $(OSAX_DIR)/mt2_prefpane_refresh.c $(OSAX_DIR)/SIMBL-Info.plist
	@mkdir -p $(OSAX_SIMBL)/Contents/MacOS
	$(CC) -bundle -mmacosx-version-min=10.9 -Wall -framework IOKit -framework CoreFoundation -lobjc \
	  -o $(OSAX_SIMBL)/Contents/MacOS/MT2PaneRefresh $(OSAX_DIR)/mt2_prefpane_refresh.c
	cp $(OSAX_DIR)/SIMBL-Info.plist $(OSAX_SIMBL)/Contents/Info.plist
	@echo "Built $(OSAX_SIMBL)"

prefpane-refresh-simbl-install: $(OSAX_SIMBL)
	sudo rm -rf "$(SIMBL_PLUGINS)/MT2PaneRefresh.bundle"
	sudo cp -R $(OSAX_SIMBL) "$(SIMBL_PLUGINS)/"
	@echo "Installed into $(SIMBL_PLUGINS) — relaunch System Preferences to inject"

prefpane-refresh-simbl-uninstall:
	sudo rm -rf "$(SIMBL_PLUGINS)/MT2PaneRefresh.bundle"
	@echo "Removed MT2PaneRefresh.bundle from $(SIMBL_PLUGINS)"

# Build the gesture kext (delegates to its own makefile).
kext-gesture:
	$(MAKE) -C kext-gesture

# reload: swap in a freshly built kext AND force the device to re-establish cleanly, so we never hit the
# async-teardown collision. `make -C kext-gesture unload && load` alone leaves the BT link up, so the
# device never re-opens PSM 19 and the manual-started BNBTrackpadDevice can't bind (dead pad until a
# manual tap). Here: unload -> wait for our nub + BNB to drain (bounded) -> load -> bounce whichever
# transport is present (BT via mt2_bt_bounce, USB via mt2_reenumerate). The MT2 drives one transport at
# a time, so bouncing both is safe — the absent one is a no-op. Tools are prereqs so they're built first.
reload: $(SBIN)/mt2_bt_bounce $(SBIN)/mt2_reenumerate
	$(MAKE) -C kext-gesture unload
	@echo "reload: waiting for our nub + BNB to drain (async teardown)..."
	@for i in $$(seq 1 50); do \
	  ioreg -lw0 | grep -q '"com_schmonz_MT2Gesture"=1\|"BNBTrackpadDevice"=1' || break; \
	  sleep 0.1; \
	done
	$(MAKE) -C kext-gesture load
	@echo "reload: bouncing present transport(s) for a clean re-establish..."
	-$(SBIN)/mt2_bt_bounce
	-$(SBIN)/mt2_reenumerate

# Assemble an installer. The unsigned kext goes under /usr/local/lib/mt2d (NOT
# /Library/Extensions, which enforces signing); the launchd wrapper kextloads it
# from there. Root-run binaries + wrapper -> /usr/local/sbin, LaunchDaemon -> /Library.
pkg: $(SBIN)/mt2_reenumerate $(SBIN)/mt2_set_btname kext-gesture
	rm -rf build/pkgroot build/scripts
	mkdir -p build/pkgroot/usr/local/lib/mt2d
	mkdir -p build/pkgroot/usr/local/sbin
	mkdir -p build/pkgroot/Library/LaunchDaemons
	cp -R kext-gesture/MT2Gesture.kext build/pkgroot/usr/local/lib/mt2d/
	cp $(SBIN)/mt2_reenumerate $(SBIN)/mt2_set_btname dist/mt2d-run build/pkgroot/usr/local/sbin/
	chmod +x build/pkgroot/usr/local/sbin/mt2d-run
	cp dist/com.schmonz.mt2d.plist build/pkgroot/Library/LaunchDaemons/
	cp -R dist/scripts build/scripts
	chmod +x build/scripts/preinstall build/scripts/postinstall
	pkgbuild --root build/pkgroot --scripts build/scripts \
	  --identifier $(PKG_ID) --version $(VERSION) --install-location / \
	  build/mt2d-$(VERSION).pkg
	@echo "Built build/mt2d-$(VERSION).pkg"

# Unit tests are pure C, no frameworks needed. Built into build/tests/ (gitignored) to keep root clean.
TESTDIR = build/tests
TESTS = $(addprefix $(TESTDIR)/,test_model test_decode test_bt_decode test_encode test_pipeline test_lifecycle test_session test_connect_sm test_conn_trace test_geometry test_vtable_clone test_usb_reframe test_genuine_host test_coordinator)
test: $(TESTS)
	@fail=0; for t in $(TESTS); do echo "== $$(basename $$t) =="; "$$t" || fail=1; done; \
	 echo "== test_mt2d_run.sh =="; sh tests/test_mt2d_run.sh || fail=1; \
	 echo "== test_conn_trace_parser.sh =="; sh tests/test_conn_trace_parser.sh || fail=1; \
	 if command -v bats >/dev/null 2>&1; then \
	   echo "== test_re.bats =="; bats tests/test_re.bats || fail=1; \
	 else echo "== test_re.bats == SKIPPED (bats not installed)"; fi; \
	 [ $$fail -eq 0 ] && echo "ALL TESTS PASS"

$(TESTDIR):
	@mkdir -p $@

$(TESTDIR)/test_model: tests/test_model.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $<
$(TESTDIR)/test_decode: tests/test_decode.c $(SRC)/mt2_usb_decode.c $(SRC)/mt2_decode.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_bt_decode: tests/test_bt_decode.c $(SRC)/mt2_bt_decode.c $(SRC)/mt2_decode.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_encode: tests/test_encode.c $(SRC)/mt1_encode.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_pipeline: tests/test_pipeline.c $(SRC)/mt2_pipeline.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_lifecycle: tests/test_lifecycle.c $(SRC)/mt2_lifecycle.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_session: tests/test_session.c $(SRC)/mt2_session.c $(SRC)/mt2_pipeline.c $(SRC)/mt2_lifecycle.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_connect_sm: tests/test_connect_sm.c $(SRC)/mt2_connect_sm.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_conn_trace: tests/test_conn_trace.c $(SRC)/conn_trace.c $(SRC)/mt2_connect_sm.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_geometry: tests/test_geometry.c $(SRC)/mt2_geometry.c | $(TESTDIR)
	$(CC) $(CFLAGS) -I$(SRC) -o $@ $^
$(TESTDIR)/test_vtable_clone: tests/test_vtable_clone.c | $(TESTDIR)
	$(CC) $(CFLAGS) -Ikext-gesture -o $@ $^
$(TESTDIR)/test_usb_reframe: tests/test_usb_reframe.c $(SRC)/mt2_usb_reframe.c $(SRC)/mt2_usb_decode.c $(SRC)/mt2_decode.c $(SRC)/mt2_pipeline.c $(SRC)/mt2_lifecycle.c $(SRC)/mt1_encode.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_genuine_host: tests/test_genuine_host.c $(SRC)/genuine_host.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^
$(TESTDIR)/test_coordinator: tests/test_coordinator.c $(SRC)/mt2_coordinator.c | $(TESTDIR)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf $(SBIN) build
	# legacy root-built tool binaries (pre-sbin/ layout) — clean any stragglers
	rm -f vhid_probe mt2_reenumerate mt2_set_btname mt2_bt_bounce mt2_usb_enable mt_listen test_gesture *.o
