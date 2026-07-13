# VoodooInputSample — a reference VoodooInput satellite for macOS 10.9

This is a minimal, **hardware-less** [VoodooInput](https://github.com/acidanthera/VoodooInput)
*satellite* driver for Mavericks (10.9). It exists to demonstrate — end to end, on real hardware —
that **a driver authored for VoodooInput can be compiled for 10.9 and Just Work** against this
project's VoodooInput compatibility layer. Loading it and toggling one sysctl moves the cursor,
driven through the entire stack with no special glue.

It is a *reference example*, not a real device driver: instead of reading hardware it fabricates a
contact that circles the pad on a timer. The **load-bearing part — how a VoodooInput client publishes
itself and delivers events — is faithful** and is the template to copy.

## The two patterns a VoodooInput satellite implements

**1. Publish yourself so the VoodooInput multiplexer binds you** (`VoodooInputSample.cpp`,
`start()`): set the `VoodooInputSupported` property to true and advertise your logical coordinate
range via `Logical Max X` / `Logical Max Y` (`VOODOO_INPUT_LOGICAL_MAX_X_KEY` / `_Y_KEY`), then
`registerService()`. IOKit matches the multiplexer (here, this project's `com_schmonz_VoodooInput`)
as a client on top of you.

```cpp
setProperty("VoodooInputSupported", kOSBooleanTrue);
setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, (unsigned long long)VSAMPLE_LMAX, 32);
setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, (unsigned long long)VSAMPLE_LMAX, 32);
registerService();
```

**2. Find the VoodooInput instance and send it events** (`VoodooInputSample.cpp`, `tick()`): the
multiplexer attaches asynchronously and sets the `VOODOO_INPUT_IDENTIFIER` ("VoodooInput Instance")
property on itself. Find it among your clients, build a `VoodooInputEvent`, and deliver it with
`messageClient(kIOMessageVoodooInputMessage, muxInstance, &event, sizeof event)`:

```cpp
VoodooInputEvent ev; memset(&ev, 0, sizeof ev);
ev.contact_count = 1;
ev.transducers[0].isTransducerActive = true;
ev.transducers[0].secondaryId = 1;
ev.transducers[0].currentCoordinates.x = x;   // in [0 .. Logical Max X]
ev.transducers[0].currentCoordinates.y = y;
ev.transducers[0].currentCoordinates.pressure = 40;   // > 0 for an active contact
messageClient(kIOMessageVoodooInputMessage, muxInstance, &ev, sizeof ev);
```

The wire types (`VoodooInputEvent`, `kIOMessageVoodooInputMessage`, the property-key strings) come
from acidanthera's VoodooInput headers, vendored verbatim under `third_party/VoodooInput/` — so this
example compiles against **the real VoodooInput ABI**, not a lookalike.

## What happens under the hood

`sample tick → VoodooInputEvent → messageClient(mux) → mux translates to the MT2 contact format →
shared conditioning session → the synthetic AppleMultitouchDevice the mux stood up → hidd's
multitouch recognizer → IOHIDPointing → the cursor moves`. None of that is in this example; it is the
merged VoodooInput inbound interface (sub-project 1) + synthetic terminal (sub-project 2).

## Building and running (on a 10.9 box)

```sh
cmake --preset native
cmake --build --preset native --target kext          # MT2Gesture.kext (the mux + synthetic terminal)
cmake --build --preset native --target voodoo_sample # VoodooInputSample.kext (this example)

# load the mux kext first, then this satellite (each is a separate bundle):
sudo cp -R build-native/MT2Gesture.kext /tmp/ && sudo chown -R root:wheel /tmp/MT2Gesture.kext
sudo kextload /tmp/MT2Gesture.kext
sudo cp -R build-native/VoodooInputSample.kext /tmp/ && sudo chown -R root:wheel /tmp/VoodooInputSample.kext
sudo kextload /tmp/VoodooInputSample.kext

sudo sysctl -w debug.vinput_demo=1   # cursor circles on its own — the end-to-end proof
sudo sysctl -w debug.vinput_demo=0   # stops (sends a clean lift)

sudo kextunload -b com.schmonz.VoodooInputSample
sudo kextunload -b com.schmonz.MT2Gesture
```

`ioreg -w0 -r -c com_schmonz_VoodooInputSample` shows this satellite with `VoodooInputSupported`, the
`com_schmonz_VoodooInput` multiplexer attached as its client, and (once bound) a fabricated
`AppleMultitouchDevice` with a descendant `IOHIDPointing`.

## Files
- `VoodooInputSample.{h,cpp}` — the satellite `IOService` (publish + emit).
- `vinput_demo_path.{h,c}` — the pure circling-coordinate math (host-tested by
  `tests/test_vinput_demo_path.c`; no libm — a fixed-point cosine table).
- `Info.plist.in` — an `IOResources`-matched kext (loads unconditionally; no hardware).
