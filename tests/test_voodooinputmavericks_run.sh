#!/bin/sh
# Tests the voodooinputmavericks-run boot wrapper's sentinel state machine in dry-run mode.
# Dry-run (MT2D_DRYRUN=1) decides MODE and updates the state file but performs
# no kextload/exec, so the brick-guard logic is testable without a real panic.
set -u
WRAPPER="$(dirname "$0")/../dist/voodooinputmavericks-run"
TMP="$(mktemp -d -t voodooinputmavericks_run_test)"
trap 'rm -rf "$TMP"' EXIT
fail=0

check() {
    desc="$1"; want_mode="$2"; want_state="$3"; init_state="$4"
    sf="$TMP/state"
    if [ "$init_state" = "MISSING" ]; then rm -f "$sf"; else echo "$init_state" > "$sf"; fi
    out="$(MT2D_STATE_FILE="$sf" MT2D_DRYRUN=1 MT2D_STAT_CONSOLE="echo testuser" "$WRAPPER" 2>/dev/null)"
    got_mode="$(echo "$out" | sed -n 's/^MODE=//p')"
    got_state="$(echo "$out" | sed -n 's/^STATE=//p')"
    if [ "$got_mode" = "$want_mode" ] && [ "$got_state" = "$want_state" ]; then
        echo "PASS: $desc"
    else
        echo "FAIL: $desc (mode=$got_mode want $want_mode; state=$got_state want $want_state)"
        fail=1
    fi
}

check "missing state -> full/trying"  full  trying  MISSING
check "ok          -> full/trying"    full  trying  ok
check "trying      -> skip/trying"    skip  trying  trying

# Session guard: at the login window /dev/console is owned by root -> skip the load (leave the MT2 to
# Apple's generic HID); once a real user owns the console -> proceed with the full load. Console owner
# is mocked via MT2D_STAT_CONSOLE (an echo command), mirroring the other mocks.
sguard() {
    desc="$1"; want_mode="$2"; console="$3"
    out="$(MT2D_STATE_FILE="$TMP/sg" MT2D_DRYRUN=1 MT2D_STAT_CONSOLE="echo $console" "$WRAPPER" 2>/dev/null)"
    got="$(echo "$out" | sed -n 's/^MODE=//p')"
    if [ "$got" = "$want_mode" ]; then echo "PASS: $desc"; else echo "FAIL: $desc (mode=$got want $want_mode)"; fail=1; fi
}
sguard "console=root (login window) -> skip-nosession" skip-nosession root
sguard "console=user (logged in)    -> full"           full            testuser

# --reset writes ok regardless of prior state
sf="$TMP/state"; echo trying > "$sf"
MT2D_STATE_FILE="$sf" "$WRAPPER" --reset >/dev/null 2>&1
if [ "$(cat "$sf")" = "ok" ]; then echo "PASS: --reset -> ok"; else echo "FAIL: --reset (got $(cat "$sf"))"; fail=1; fi

hv() {
    desc="$1"; want="$2"; shift 2
    got="$("$WRAPPER" --health-verdict "$@" 2>/dev/null)"
    if [ "$got" = "$want" ]; then echo "PASS: $desc"; else echo "FAIL: $desc (got $got want $want)"; fail=1; fi
}
hv "BT=2 nub=1 -> healthy"     healthy    2 0 1
hv "BT=1 nub=1 -> incomplete"  incomplete 1 0 1
hv "BT=0 nub=1 -> incomplete"  incomplete 0 0 1
hv "USB=1 nub=1 -> healthy"    healthy    0 1 1
hv "nub=0 BT=2 -> incomplete"  incomplete 2 0 0

# Recovery loop: fake ioclasscount returns INCOMPLETE (BT=1) on the first query then HEALTHY (BT=2),
# proving recover_full retries (bounce BT close->open) and converges. bounce + kext ops + sleep stubbed.
rec="$TMP/rec"; mkdir -p "$rec"
cat > "$rec/ioclasscount" <<EOF
#!/bin/sh
case "\$1" in
  MT2BTReader)
    n="\$(cat "$rec/btcalls" 2>/dev/null || echo 0)"; n=\$((n+1)); echo "\$n" > "$rec/btcalls"
    if [ "\$n" -le 1 ]; then echo "x 1"; else echo "x 2"; fi ;;
  MT2USBReader) echo "x 0" ;;
  MavericksVoodooInputHost)   echo "x 1" ;;
esac
EOF
chmod +x "$rec/ioclasscount"
printf '#!/bin/sh\nexit 0\n' > "$rec/kextload";   chmod +x "$rec/kextload"
printf '#!/bin/sh\nexit 0\n' > "$rec/kextunload"; chmod +x "$rec/kextunload"
rec_out="$(MT2D_STATE_FILE="$rec/state" MT2D_IOCLASSCOUNT="$rec/ioclasscount" \
    MT2D_KEXTLOAD="$rec/kextload" MT2D_KEXTUNLOAD="$rec/kextunload" MT2D_BOUNCE=: \
    MT2D_HEALTHY_DELAY=0 MT2D_RECOVER_TRIES=3 \
    "$WRAPPER" --recover 2>&1)"
if echo "$rec_out" | grep -q "recovery succeeded on attempt 2"; then
    echo "PASS: recover_full retries (BT=1->2) then converges to healthy"
else
    echo "FAIL: recover_full (out: $rec_out)"; fail=1
fi

# Regression (two-transport / desktop): a normal boot with NO trackpad connected (BT=0 USB=0, nub
# loaded = kext loaded without panic) must end the sentinel at 'ok' -- NOT 'trying', which would make
# the NEXT boot skip-load -- and must NOT thrash recover_full (there is no device to recover). The
# brick-guard is for load panics (which reboot before this point), not for "no device present".
nod="$TMP/nod"; mkdir -p "$nod/sbin"
cat > "$nod/ioclasscount" <<EOF
#!/bin/sh
case "\$1" in
  MT2BTReader)  echo "x 0" ;;
  MT2USBReader) echo "x 0" ;;
  MavericksVoodooInputHost)   echo "x 1" ;;
esac
EOF
chmod +x "$nod/ioclasscount"
printf '#!/bin/sh\nexit 0\n' > "$nod/kextload";   chmod +x "$nod/kextload"
printf '#!/bin/sh\nexit 0\n' > "$nod/kextunload"; chmod +x "$nod/kextunload"
printf '#!/bin/sh\nexit 1\n' > "$nod/sbin/mt2_reenumerate"; chmod +x "$nod/sbin/mt2_reenumerate"
echo ok > "$nod/state"
nod_out="$(MT2D_STATE_FILE="$nod/state" MT2D_IOCLASSCOUNT="$nod/ioclasscount" \
    MT2D_KEXTLOAD="$nod/kextload" MT2D_KEXTUNLOAD="$nod/kextunload" MT2D_SBIN="$nod/sbin" \
    MT2D_RECONNECT_WAIT=0 MT2D_HEALTHY_DELAY=0 MT2D_FORCE_LOAD=1 MT2D_BOUNCE="touch $nod/bounced" "$WRAPPER" 2>&1)"
nod_state="$(cat "$nod/state")"
if [ "$nod_state" = "ok" ] && ! echo "$nod_out" | grep -q "recovery attempt"; then
    echo "PASS: no-device boot -> ok, no recovery thrash"
else
    echo "FAIL: no-device boot (state=$nod_state, recovery in out? — out: $nod_out)"; fail=1
fi
# The complement of the USB-skip: a NON-USB boot (no USB MT2) MUST still run the BT bounce — skipping it
# there would break the BT half-open takeover. Guards against over-broad bounce suppression.
if [ -f "$nod/bounced" ]; then
    echo "PASS: non-USB boot -> DOES run the BT bounce (half-open takeover preserved)"
else
    echo "FAIL: non-USB boot skipped the BT bounce (out: $nod_out)"; fail=1
fi

# USB gesture-open is delegated to mt2_linkstated (the daemon kicks hidd on the reclaim's live
# USB-appear). The WRAPPER must therefore NEVER kick hidd — on any path. Two kickers made two hidd
# kills ~1s apart -> the second interrupts the first respawn -> a ~9s all-HID freeze (measured
# on-device 2026-07-21, "cursor stuck"). We stub MT2D_HIDD_KICK to a marker; the marker must NOT
# appear. (a) won-on-load boot: USB reader already bound.
usbk="$TMP/usbk"; mkdir -p "$usbk/sbin"
cat > "$usbk/ioclasscount" <<EOF
#!/bin/sh
case "\$1" in
  MT2BTReader)  echo "x 0" ;;
  MT2USBReader) echo "x 1" ;;
  MavericksVoodooInputHost)   echo "x 1" ;;
esac
EOF
chmod +x "$usbk/ioclasscount"
printf '#!/bin/sh\nexit 0\n' > "$usbk/kextload";   chmod +x "$usbk/kextload"
printf '#!/bin/sh\nexit 0\n' > "$usbk/kextunload"; chmod +x "$usbk/kextunload"
echo ok > "$usbk/state"
usbk_out="$(MT2D_STATE_FILE="$usbk/state" MT2D_IOCLASSCOUNT="$usbk/ioclasscount" \
    MT2D_KEXTLOAD="$usbk/kextload" MT2D_KEXTUNLOAD="$usbk/kextunload" MT2D_SBIN="$usbk/sbin" \
    MT2D_RECONNECT_WAIT=0 MT2D_HEALTHY_DELAY=0 MT2D_FORCE_LOAD=1 MT2D_BOUNCE=: \
    MT2D_HIDD_KICK="touch $usbk/kicked" "$WRAPPER" 2>&1)"
if [ ! -f "$usbk/kicked" ] && ! echo "$usbk_out" | grep -q "kicked hidd"; then
    echo "PASS: won-on-load boot -> wrapper does NOT kick hidd (daemon owns gesture-open)"
else
    echo "FAIL: won-on-load boot wrapper kicked hidd (kicked? $([ -f "$usbk/kicked" ] && echo yes || echo no); out: $usbk_out)"; fail=1
fi

# (b) RECLAIM path: our reader lost interface 1, mt2_reenumerate reclaims it. The wrapper must still
# RECLAIM (logs "re-enumerated to reclaim it") but must NOT kick hidd (the daemon will, on the live
# re-appear). Stub: first MT2USBReader read 0 (drives the reenumerate branch), reenumerate succeeds.
recl="$TMP/recl"; mkdir -p "$recl/sbin"
cat > "$recl/ioclasscount" <<EOF
#!/bin/sh
case "\$1" in
  MT2BTReader)  echo "x 0" ;;
  MT2USBReader)
    n=\$(cat "$recl/usb_reads" 2>/dev/null || echo 0); n=\$((n + 1)); echo "\$n" > "$recl/usb_reads"
    if [ "\$n" -ge 2 ]; then echo "x 1"; else echo "x 0"; fi ;;
  MavericksVoodooInputHost) echo "x 1" ;;
esac
EOF
chmod +x "$recl/ioclasscount"
printf '#!/bin/sh\nexit 0\n' > "$recl/sbin/mt2_reenumerate"; chmod +x "$recl/sbin/mt2_reenumerate"
printf '#!/bin/sh\nexit 0\n' > "$recl/kextload";   chmod +x "$recl/kextload"
printf '#!/bin/sh\nexit 0\n' > "$recl/kextunload"; chmod +x "$recl/kextunload"
echo ok > "$recl/state"
recl_out="$(MT2D_STATE_FILE="$recl/state" MT2D_IOCLASSCOUNT="$recl/ioclasscount" \
    MT2D_KEXTLOAD="$recl/kextload" MT2D_KEXTUNLOAD="$recl/kextunload" MT2D_SBIN="$recl/sbin" \
    MT2D_RECONNECT_WAIT=0 MT2D_HEALTHY_DELAY=0 MT2D_FORCE_LOAD=1 MT2D_BOUNCE="touch $recl/bounced" \
    MT2D_HIDD_KICK="touch $recl/kicked" "$WRAPPER" 2>&1)"
if printf '%s\n' "$recl_out" | grep -q "re-enumerated to reclaim it" \
   && [ ! -f "$recl/kicked" ] && ! printf '%s\n' "$recl_out" | grep -q "kicked hidd"; then
    echo "PASS: reclaim boot -> reclaims interface 1 but wrapper does NOT kick hidd (daemon owns it)"
else
    echo "FAIL: reclaim boot (reclaimed? $(printf '%s\n' "$recl_out" | grep -q 're-enumerated' && echo yes || echo no); wrapper-kicked? $([ -f "$recl/kicked" ] && echo yes || echo no); out: $recl_out)"; fail=1
fi
# ...and a USB boot must SKIP the BT bounce (the ~10.5s single-transport timeout, measured 2026-07-21).
if [ ! -f "$recl/bounced" ] && printf '%s\n' "$recl_out" | grep -q "skipping BT bounce"; then
    echo "PASS: USB boot -> skips the BT bounce (no single-transport timeout)"
else
    echo "FAIL: USB boot BT bounce (bounced? $([ -f "$recl/bounced" ] && echo yes || echo no); out: $recl_out)"; fail=1
fi

# And the no-device boot above must NOT have kicked hidd either. Reuse its output.
if echo "$nod_out" | grep -q "kicked hidd"; then
    echo "FAIL: no-device boot kicked hidd (should not)"; fail=1
else
    echo "PASS: no-device boot does NOT kick hidd"
fi

if [ $fail -eq 0 ]; then echo "ALL voodooinputmavericks-run TESTS PASS"; else echo "voodooinputmavericks-run TESTS FAILED"; exit 1; fi
exit 0
