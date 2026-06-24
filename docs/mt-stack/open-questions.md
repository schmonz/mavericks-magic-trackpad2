# Open questions — things we need to understand but don't yet

Genuine known-unknowns about driving the 10.9 multitouch stack: behaviours we've *observed* but
can't yet *explain*, and things we haven't measured. Distinct from `decisions.md` (which records
choices we've already resolved). When one of these gets understood, fold the finding into
`explanation.md`/`reference.md` and remove it here (or move a closed one to `decisions.md` if it
settled a choice).

---

## Edge-clamp: frozen edge bands — STILL OPEN; TWO theories now falsified on-device

> **UPDATE 2026-06-24 — transport theory FALSIFIED.** The `kEdgeNoBtTransport` build (override
> `newTransportString` → non-BT) was tested on-device. `tools/mt_transport` confirms it worked at the
> recognizer level: `transportMethod=1` (not 4 = Bluetooth). **But the L/R dead-zone PERSISTED and
> from-edge swipes STILL worked** — both should have changed if they were gated on `transport==4`. So the
> edge reserve is NOT gated on transport (the `0xf8a4 cmpl $0x4` reading below is either misread or not the
> operative gate). This is the SECOND falsified theory (coordinate-range was first). Per systematic
> debugging: stop theory-guessing; re-investigate from evidence (instrument what happens to the cursor's
> X delta inside the band). Candidate gates to examine instead (from `mt_transport`): `builtIn=0`,
> `familyID=128`, `driverType=4`, `parserType=1000`, `parserOptions=47`; also whether toggling the NC
> edge-swipe pref changes it. The `kEdgeNoBtTransport` flag is reverted to false (no-op for the bug).
> Everything below is the (now-doubted) prior transport RE, kept for the audit trail.

### (superseded) prior reading: `MTSlideGesture::isBlocked`, gated on BT transport

**Symptom:** cursor freezes in a band near the pad edges (you must clutch/lift more); the cursor still
reaches all screen edges (it's relative). Up/down works in the band.

**Not coordinates (H1 + the encode-range fix are dead).** In-band wiggle: decoded `x` *varies*
(`3440→3573`) while the cursor is frozen → the device reports position fine; nothing in decode/encode
clamps. On-device, narrowing the emitted X range (`MT1_MAX_X` ±2500) did **nothing** — confirming it's
not a coordinate-range problem. (Also overturns the 2026-06-19 "device-side saturation" reading.)

**CONFIRMED CAUSE (RE, off-device):** the 10.9 `MTSlideGesture::isBlocked` edge-swipe reserve
(MultitouchHID.plugin) — a known bug where the ~2cm edge strip stays reserved (blocking 1-finger
pointer) even with the NC gesture off. Instruction-verified at the `0xf8a4` conditional:
```
0xf89a  cmpl $0x4, %r8d         ; transport == 4 (Bluetooth)?
0xf89e  jne  0xf902             ;   not BT -> skip the buggy block
0xf8a4  testb $0x20, 0xd9(%r15) ; contact in the reserve zone?
0xf8ac  jne  0xf952             ;   -> BLOCKED  (pointer frozen)
```
So the block is **gated on transport == 4**. `_MTDeviceGetTransportMethod` returns the transport cached
at `MTDevice+0x90`, set by `_mt_CachePropertiesForDevice` from the AMD's **`Transport` property string**;
only `"Bluetooth"` maps to 4 (it's the only transport string in MultitouchSupport). We publish
`Transport="Bluetooth"` always (full-BNB via `createMultitouchHandler`; fDevice/USB via `makeHidProps`),
so we hit the block on every transport.

**FIX (identified, pending on-device verify):** make the recognizer see a **non-BT transport** (anything
≠ `"Bluetooth"` → numeric ≠ 4 → `jne 0xf902` skips the buggy block). fDevice/USB path: one-line change
in `makeHidProps`. Full-BNB path: harder — the AMD's `Transport` is set by `createMultitouchHandler`
(override after creation or via the props dict; same timing concern as geometry). **Risk to verify:**
no gesture/tap/pane regression from a non-BT transport (the only transport==4-gated thing found is this
block, but confirm). **Verify:** dead zone gone + gestures OK; `ioreg` shows `Transport` ≠ Bluetooth.
Relates to [[mt2-cursor-edge-clamp]]. (Test bed: the fDevice/USB path under `kFullBnb=false`.)

---

## Genuine USB presentation (AppleUSBMultitouchDriver) — viable but structurally unlike BT-genuine

RE'd 2026-06-23 (off-device) to inform "should the genuine presentation cover USB too / the full-BNB-BT
decision." Findings:
- **Genuine USB stack exists:** `AppleUSBMultitouchDriver` in `AppleUSBMultitouch.kext` (twin of
  AppleBluetoothMultitouch.kext), pane-matched, lineage `IOUSBHIDDriver : IOHIDDevice`.
- **Opposite object model to BT:** it IS the multitouch device (one object — has `cacheDeviceProperties`,
  `_deviceGetReportWithLookUp`, `_deviceGetReport`, `handleReport` itself), NOT a transport that spawns a
  separate AMD. No `createMultitouchHandler`/trigger. Touch feed seam = **`handleReport` @0x4196** (the
  USB pipe → its handleReport parses). Geometry seam = its own **`_deviceGetReport`** (USB), not a
  transport vtable. `newTransportString` = `IOUSBHIDDriver`'s → **"USB"** → no `isBlocked` edge-clamp.
- **So "genuine for both" is NOT one parameterized strategy** — BT (two-object transport+AMD, vtable
  geometry, L2CAP-interpose feed) and USB (one-object HID driver, _deviceGetReport geometry, handleReport
  feed) are different implementations. It buys the pane on both + no USB edge-clamp, at the cost of a
  SECOND genuine impl (vs today's working synthetic-USB). Genuine-USB looks *simpler* than genuine-BT
  (no manual-trigger/interpose/flap), possibly near-free IF handleReport parses MT2's USB format.
- **Decision input:** decide full-BNB-for-BT on its own merits (pane/genuine vs flap+dirty-tricks cost),
  NOT on a unify-with-USB benefit (there isn't one). See [[mt2-mission-interface-over-driver]].

**RESOLVED on-device 2026-06-24 (genuine-usb-prefpane spike): NO — Apple's USB driver rejects MT2's
packets; genuine-USB is not viable for input.** What the spike proved, in order:
- Inject a device-match personality for pid 613 (one past Apple's `{566..612}` list) → genuine
  `AppleUSBMultitouchDriver` starts on the MT2 interface, `Transport="USB"`, no panic. **Pane lights**
  (userspace `IOHIDLibUserClient`s attach) — BUT shows the **built-in-laptop-trackpad** animation, not
  `BTTrackpad` art (see [[mt2-prefpane-asset-swap]]).
- Apple's `AppleUSBMultitouchHIDEventDriver` matches `{page 65280, usage 1}`; MT2 presents
  `{65280, 12}`. Inject a twin event-driver personality (usage 12) → it binds + wires to `IOHIDSystem`.
  Cursor plumbing complete.
- Device only streams after MT2's USB enable (SET_REPORT Feature id 0x02 `{0x02,0x01}`); Apple's driver
  doesn't send it. Sent it from userspace (`tools/mt2_usb_enable.c`, `IOHIDDeviceSetReport`, returned OK)
  → device streams: `handleReport` fired 1755× on finger movement (dtrace `fbt:com.apple.driver.AppleUSBMultitouch`).
- **But every packet is rejected.** dmesg: `handleReport - not in path binary mode, received 0x2 data
  packet of length 21` + `validateChecksum - 21-byte packet checksum is incorrect`. Two independent
  incompatibilities between MT2 and Apple's 10.9 USB driver (a "path binary mode" the device isn't in,
  and a different checksum scheme over the 21-byte packet) → nothing dispatched → no cursor.

**Verdict:** genuine-USB assembles the *presentation* (pane) and the device streams, but Apple's driver
can't *consume* MT2's raw packets natively (mode + checksum mismatch). This is **not a dead end — it's a
translation target we haven't written**: the same translate-and-feed pattern we already use everywhere
(BT: MT2→MT1→BNB's AMD; synthetic-USB: MT2→MT1→our nub). Genuine-USB would be **MT2 packets → Apple's
older USB packet format ("path binary mode" + valid checksum) → feed `handleReport`** (interpose via an
instance vtable clone, as with the BT geometry override). Leverage we already hold: dmesg gives checksum
test vectors (`expected 0x499` vs `bytes 0x249` …) to crack `AppleUSBMultitouchDriver::validateChecksum`,
and `handleReport`/`_deviceGetReport` give the packet layout.

**SIZING RE done 2026-06-24 (off-device):**
- **`validateChecksum` CRACKED — trivial.** 16-bit additive sum of bytes[0 .. n-3]; the last two bytes
  hold it little-endian (`low=byte[n-2], high=byte[n-1]`). Verified vs the live vector (`expected 0x499`
  = sum of bytes[0..18]; MT2's own trailing `0x0249` is a different scheme). A ~5-line encoder appends it.
- **The checksum is the ONLY hard gate.** "not in path binary mode" / "Mouse mode was detected" are just
  IOLog warnings; `handleReport` computes the checksum and proceeds regardless.
- **Mode-coax route is OUT.** On seeing MT2's `0x2` packets (read as "mouse mode") the driver calls
  `configureDataMode`, which switches mode via Apple-specific feature reports (read/modify/write report id
  3 @8B, then set report `0xac` @1B). MT2 won't honor these → it won't natively emit Apple's format. So we
  can't just flip a mode; we **translate**.
- **Route = translate-and-feed:** emit Apple path-binary packets (header `0x60` + Apple's contact layout)
  + the now-known checksum, and interpose `handleReport` (instance vtable clone). Source contacts: we
  already decode them (`mt2_usb_decode`).
- **One sizing unknown left:** the exact `0x60` packet LAYOUT (contact struct fields/offsets/count). RE
  from `handleReport`'s parse (the `callq *0xb28` contact path) + cross-ref Linux `bcm5974`. That's the
  last read before a tight estimate.

**Effort: medium, bounded** — and likely SMALLER than feared. The kext doesn't parse contacts; after the
checksum it calls `AppleUSBMultitouchUserClient::enqueueData(raw,len)` → the format lives in MultitouchSupport's
parser family (`_MTParse_*BinaryFrameHeader` + `*BinaryPath`). **The decisive find:** `_MTParse_CompactV4BinaryPath`
computes contact count as `(length-4)/9` (signed div-by-9, `imul 0x38E38E39`) ⇒ format = **4-byte header + N × 9-byte
contacts** — and **MT2 is already a CompactV4 device** (its BT path uses `_MTCompactV4HeaderUnpack`, cracked during
tap-to-click; MT2 wire contacts are 9-byte records). So the parser wants the format MT2 already speaks. The
genuine-USB rejection is NOT a contact-layout mismatch — it's (a) framing/header (`0x02` report-id vs the `0x60`
path-binary the kext gates on) + (b) the checksum. So translate-and-feed ≈ **re-frame header + recompute Apple's
checksum**, contact bodies largely pass-through. Last confirmation before building: byte-align a captured MT2 USB
`0x02` packet against the CompactV4 frame the parser expects (header size, whether the enqueued length includes the
checksum / how the `(len-4)/9` lands). Then it's a small interpose at `handleReport`/`enqueueData` + a checksum fn.

**BYTE-ALIGN DONE 2026-06-24 — contacts are PASS-THROUGH (bit-for-bit).** Diffed `_MTCompactV4BinaryContactUnpack`
vs `src/mt2_decode.c`: X = `(b1&0x1F)<<8 | b0`; Y = `b1>>5 | b2<<3 | (b3&0x3)<<11` — IDENTICAL in both (we only
negate Y's sign). touch_major/minor = bytes 4/5, size = byte 6 (`&0x3F`), state in byte 3, id/orientation byte 8
— all the same bytes our decoder uses. So MT2's raw 9-byte USB contact IS the CompactV4 contact; no per-contact
transform. **Final genuine-USB build spec:** interpose `handleReport`/`enqueueData`; leave the 9-byte contact
bodies untouched; (1) reframe the header to the CompactV4 frame the parser expects (4-byte CompactV4 header —
packing already RE'd for tap-to-click: `ts=(b1>>2)|(b2<<6)|(b3<<14)` — plus the `0x60` path-binary framing the
kext gate wants) and (2) append Apple's 16-bit additive-sum checksum (replacing MT2's trailing bytes). SIZE =
SMALL: one buffer-rewrite shim + a ~5-line checksum. Only detail to pin while building: exact MT2-USB-header →
CompactV4-4-byte-header byte mapping + whether the enqueued length includes the checksum. No remaining unknowns
of consequence.

**PACKET LAYOUT PINNED 2026-06-24 (Task 0.2 RE — CORRECTS the "4-byte header" shorthand above).** Disassembled
`AppleUSBMultitouchDriver::handleReport` (file off `0x4196`), `::validateChecksum` (`0x4506`), the `enqueueData`
call site, and MultitouchSupport `_MTCompactV4HeaderUnpack` (`0x5c84`) / `_MTParse_CompactV4BinaryFrameHeader`
(`0x5ee3`) / `_MTParse_CompactV4BinaryPath` (`0x5f69`). The `0x60` is NOT part of the CompactV4 header — it is a
framing prefix that sits BEFORE it. The full packet `handleReport` validates and forwards is:
```
byte[0]            = 0x60                       framing magic — gated at handleReport+0xed (cmpb $0x60); CONFIRMED
byte[1]            = prefix/length byte         skipped by both content scans (start at +2); HYPOTHESIS = length
byte[2..5]         = 4-byte CompactV4 header    [type, flags+timestamp] — see below
byte[6 .. 6+9N-1]  = N x 9-byte contacts        pass-through bit-identical (proven)
byte[len-2..len-1] = 16-bit additive checksum   sum of bytes[0..len-3], little-endian; CONFIRMED (validateChecksum)
```
- **CompactV4 4-byte header** (`_MTCompactV4HeaderUnpack`): `cv4[0]`=frame/report type (copied verbatim, no value
  check); `cv4[1]`=`(ts&0x3F)<<2 | flagB<<1 | flagA` (low-6 ts + 2 flag bits); `cv4[2]`=`(ts>>6)&0xFF`;
  `cv4[3]`=`(ts>>14)&0xFF`. Timestamp packing `ts=(cv4[1]>>2)|(cv4[2]<<6)|(cv4[3]<<14)` (22-bit) — CONFIRMED, matches
  the tap-to-click RE (b1=cv4[1] etc.). Note cv4[1]'s low 2 bits are flags, not ts.
- **Contact count is length-derived, NOT in the header:** `_MTParse_CompactV4BinaryPath` computes `(flen-4)/9` on the
  FRAME (the post-`0x60`/prefix region the parser is handed, i.e. starting at byte[2]); `flen = 4 + 9N`. CONFIRMED.
- **enqueueData range = the WHOLE packet** including byte[0]=`0x60` and the 2 checksum bytes; `len` comes from the
  descriptor's `getLength` (`handleReport+0x69`), checksum-validated with that same len, then `enqueueData(%r14=buf,
  len)` at `handleReport+0x28b`. CONFIRMED. So total packet `len = 8 + 9N` (with a 2-byte `0x60`+prefix region).
- **Still pin on-device (static RE cannot decide — Apple's code never sees MT2's layout):** (1) prefix width 1 vs 2
  bytes (does the CV4 header start at byte[1] or byte[2]? → `len = 7+9N` vs `8+9N`); (2) `byte[1]` semantics;
  (3) the `cv4[0]` type constant a genuine CV4 USB frame uses; (4) the MT2-USB-header → `ts` source (locate a
  timestamp field in MT2's 12-byte header, or synthesize a monotonic full-res ts — the tap-to-click work showed a
  synthesized ts is acceptable to the recognizer). Resolve (1)/(2)/(4) with the Task 0.1 captured fixture.

**INTERPOSE SEAM PINNED 2026-06-24 (Task 0.3 RE).** Via `re/vtable` on the Apple kext:
- **`handleReport` vtable slot byte offset = `0x8b8`** → slot index `0x8b8 / sizeof(void*)` = `0x117` (279). CONFIRMED.
- **Signature CONFIRMED 3-arg:** `IOReturn handleReport(IOMemoryDescriptor *report, IOHIDReportType reportType,
  IOOptionBits options)` (not the 2-arg variant). Override C sig = `(OSObject *thisptr, IOMemoryDescriptor*,
  IOHIDReportType, IOOptionBits)`.
- **Clone span:** full vtable is `0xb70` bytes (`vtable for AppleUSBMultitouchDriver` @ `0xb5a0` → MetaClass vtable @
  `0xc110`); pass `span_bytes = 0x1000` to `vtc_clone_override` (covers it with margin; the helper adds the 2 ABI
  words itself). Reusing the BT path's `0x2000` is also safe.
- **Instance acquisition = (A) MANUAL-START** (mirrors the BT manual-start of BNB; endorsed by
  [[reuse-apple-code-construct-seam]]). `AppleUSBMultitouchDriver::gMetaClass` is an external symbol and its
  `MetaClass::alloc`/`init`/ctor are present → the class is externally constructable at runtime via
  `OSMetaClass::getMetaClassWithName("AppleUSBMultitouchDriver")`. Our `com_schmonz_MT2USBReader` matches the
  `IOUSBInterface` (vid 1452/pid 613/iface 1), instantiates + `init`/`attach`/`start`s the genuine driver on that
  interface, holds the pointer, clones slot `0x117`, and chains the saved original. **CRITICAL: our reader must NOT
  open the interrupt pipe in this route** — Apple's driver opens it itself; double-open would conflict. Route (B)
  (passive `addMatchingNotification` + post-start clone) rejected: races a report arriving before the clone installs.
  Teardown reverses with `vtc_restore` FIRST, then release the genuine instance ([[mt2-unload-while-streaming-uaf]]).

**BUILD DONE + FIRST ON-DEVICE TEST BLOCKED BY CATALOGUE RESIDUE 2026-06-24.** The translate-and-feed
build is complete on branch `genuine-usb-translate-feed` (checksum + reframe host-tested; manual-start +
`handleReport` interpose, flag `kGenuineUsb`). First on-device load: Apple's `AppleUSBMultitouchDriver`
bound the interface directly (matched `idProduct 613`, bundle `com.apple.driver.AppleUSBMultitouch`,
score 99000), our `com_schmonz_MT2USBReader` had **0 instances** and never ran `startGenuine` → no
interpose, no multitouch-enable → device emitted only 8-byte mouse-mode packets, all rejected. But 613
is **NOT in Apple's on-disk personalities** (`re/plist AppleUSBMultitouch` → zero "613"), and the box
had **not rebooted since the 2026-06-24 spike** that injected a live IOCatalogue personality
`idProduct 613 → AppleUSBMultitouchDriver`. **Injected catalogue personalities persist across
kextload/kextunload — only a reboot clears them** — and this residue pre-empted seam A (which needs OUR
reader to win the interface). Our code never ran; it is not implicated. NEXT: reboot to clear, reload
the genuine build, retest (does our reader now win + interpose + checksum-accept?). If Apple still grabs
613 after a clean reboot, seam A is flawed (pivot to intercepting Apple's match). Dev-loop lesson: verify
the *matching environment* (who won the match), not just the loaded binary. Full handoff in the
`mt2-genuine-usb-resume-here` memory.

**Cost/benefit (the real decision):** synthetic-USB already delivers working cursor+gestures. Genuine-USB
*adds* a genuine prefpane on USB and makes our role uniform across transports — on BOTH we'd just
translate MT2's native packets into what Apple's genuine driver expects and feed it (BT→BNB's AMD,
USB→`handleReport`). (Pane art is a non-issue per the user 2026-06-24 — the laptop animation is fine.)
Cost = RE Apple's USB packet format + `validateChecksum` (have dmesg test vectors), write the encoder,
interpose `handleReport` (instance vtable clone). This is the clearest concrete instance of the mission's
reusable engine capability — "translate a device's native format into whatever the target consumer
expects" ([[mt2-mission-interface-over-driver]]). Next cheap step if pursued: RE `validateChecksum` +
the packet layout off-device to size it. Until/unless pursued, **keep synthetic-USB** (works today).
Relates to [[mt2-prefpane-detection-mechanism]], [[mt2-prefpane-asset-swap]].

---

## Cold-boot and sleep/wake flap rate — unmeasured

**Measured clean (2026-06-22):** warm BT reconnect = 0 flaps across all observed cycles (CONNTRACE
oracle, all `STEADY`), with the committed defer-0xF1 fix. **Not measured:** cold boot (where the
historical "no cursor at boot" actually lived) and sleep/wake (classically fragile). Until measured
with the same oracle, we don't know whether the flap is genuinely put to bed or merely absent in the
warm-reconnect case.

**How to measure:** after a reboot (or a sleep/wake), `sudo sysctl debug.mt2_log=1` then
`dmesg | ./re/conn-trace` → STEADY/FAIL for that boot's first connect. A `FAIL` timeline that never
reaches `INTERRUPT_BOUND` ⇒ PSM 19 didn't open ⇒ the targeted fix is `waitForChannelState(OPEN)` on
PSM 17 (with, finally, a real repro to verify against). See `reference.md` → BT connect handshake for
the genuine sequence and `how-to.md` → fix the connect flap. Relates to [[mt2-bt-attach-flap-rootcause]].

---

## Which control-channel transition does the device key on to open PSM 19?

**Known (RE'd):** PSM 19 is device-initiated, and the genuine driver provokes it only by *correctly
accepting the control channel* (`listenAt`-bound + `waitForChannelState(OPEN)`), sending no HID
command first. **Not provable by static RE:** *which exact* control-channel state transition the
device watches for — pure L2CAP OPEN vs. `listenAt`-acceptance vs. an L2CAP-config detail.

**Why it matters:** if reproducing the genuine *order* (the flap fix above) doesn't fully fix
cold-boot/sleep-wake, we won't know which sub-step we're getting wrong without seeing the wire.

**The one justified live capture:** an `hcidump`/L2CAP trace of a genuine-driver connect vs. our
reader's connect, diffed on the control-channel exchange right before the device opens PSM 19. Only
needed if reproducing the order doesn't fix the flap — not up front.
