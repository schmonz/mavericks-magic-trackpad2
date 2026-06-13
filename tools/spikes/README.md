# Spikes

Throwaway, single-purpose probes written while figuring the driver out. Kept for
reference; not part of the build. Compile ad-hoc, e.g.:

    clang -O2 -o /tmp/usbdiag tools/spikes/usbdiag.c -framework IOKit -framework CoreFoundation

- `spike_bind.c` — proved Apple's gesture driver binds a kextless virtual HID device.
- `diag.c` — enumerate MT2 HID interfaces; test HID-level seize + enable.
- `usbdiag.c` — claim the USB interface and read raw multitouch frames.
- `vhid_query_probe.c` — log the get/set feature reports the driver issues at start.
- `send_mouse.c` — prove the virtual mouse can drive the system cursor.
- `pipe_debug.c` — verbose end-to-end pipeline (frames -> decode -> encode -> device).
