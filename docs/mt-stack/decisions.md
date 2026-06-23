# Decisions — big-ticket roads not taken

Why we *didn't* go certain ways, so we don't re-tread them. Two kinds:

- **Not a functioning choice** — a fact makes it impossible. Won't revisit.
- **Not a sufficiently desirable choice** — it works (or could), but lost on a constraint. The entry
  names the constraint, so it's clear what would reopen it.

For *open* unknowns (vs these resolved ones) see `open-questions.md`.

---

### Own / bare `IOHIDDevice` for the prefpane — *not functioning*
To light the prefpane we tried publishing our own `IOHIDDevice` of the matched class. Publishing an
**un-started** `IOHIDDevice` null-derefs in `_publishDeviceNotificationHandler` → kernel panic. You
can't `registerService()` a bare nub of that class. (Led to: manually start a *genuine*
`BNBTrackpadDevice` instead, which is a real started IOHIDDevice.)

### REPLACE — drop genuine BNB, own the device outright — *not desirable*
A clean reimplementation that owns the BT channels + input was viable, but the user pinned **stock
Apple prefpane = mandatory**, and the pane matches `BNBTrackpadDevice`. Dropping genuine BNB loses the
pane. **Reopening criterion:** if "stock pane mandatory" were relaxed (e.g. an own-pane workstream
shipped), REPLACE becomes viable again.

### VID/PID match path (route-2 matching) — *not functioning*
We hoped to get matched onto the device by advertising the MT2's IDs. The matcher reads the **real
DID from the controller-side store**, not from our personality, so we can't win matching that way.
Manual-start is the only route.

### Seam C — wire injection (intercept HIDP GET_REPORT on PSM 17) — *not functioning / not desirable*
Premise was that geometry crosses the L2CAP wire and we could fabricate the response. It doesn't —
the geometry query terminates at in-kernel transport stubs, never hitting the wire. Even if it could
work, the in-kernel vtable override is simpler.

### Single-slot geometry override (`0xcc8` only) — *not functioning as designed*
Overriding only `getMultitouchReport` (`0xcc8`) never fired: `_deviceGetReportWithLookUp` probes the
**LENGTH via `getMultitouchReportInfo` (`0xcd8`) FIRST**, which hit the stub and short-circuited
before the data fetch. Proven on-device (override live, `MATCH`, but never called). **Fix that
shipped:** override *both* `0xcd8` and `0xcc8`. Don't re-try single-slot.

### Late / passive geometry publish — *not functioning*
Installing a geometry handler *after* the AMD started, or late `setProperty` of the geometry keys,
had no effect: userspace `MultitouchSupport` caches geometry at **first attach**, and the AMD is born
(empty) before it's reachable at `transport+0x1b0`. The publish must be correct *before* `AMD::start`
— hence the vtable override installed before `bnb->start()`.

### `IOMatchCategory` to coexist with `IOBluetoothHIDDriver` — *not functioning*
Trying to coexist (instead of exclude) so PSM 19 would open reliably: it opened, but the HID driver
then grabbed the interrupt-channel `listenAt` delegate (single-owner) → our listener rejected → no
frames, and the HID driver still didn't drive a cursor. Worst of both; reverted.

### Hybrid `+0x1b0` poke — *not desirable (stability)*
The hybrid architecture routed prefpane settings by poking BNB's handler slot (`+0x1b0`) to our
fDevice. It worked (controls applied) but its teardown drove genuine HID teardown against a
half-restored object → the `ultimate-hat.panic`. Motivated the move to full-BNB, which deletes the
poke (BNB's node *is* the input *is* the pane target). **Note:** the *hybrid architecture itself*
remains a live alternative for a livability comparison; only this *mechanism* is graveyarded.
