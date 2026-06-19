#!/usr/sbin/dtrace -s
/*
 * trace_tap.d - round-5 tap-blocker capture: find WHERE the tap path dies.
 *
 * Round-5 runtime finding (already established): our 1-finger tap matches a chord with
 * motionCode 0 -> the MTChordTable+0x18 mask is BYPASSED (motion 0 skips the gate). So
 * the mask is NOT the blocker (H1 dead). This script funnels the MTChordCycling tap path
 * to find the LAST function reached during a tap = the real gate.
 *
 * Funnel (per frame): chk4chordCycling -> chk4newTapChord / handleChordTaps
 *                     -> selectTapChord -> tapHasValidTimingAndStrength
 *
 * Self-terminates after ~9s (clean END flush; no kill race). Inject taps during that window.
 *   sudo dtrace -q -s tools/trace_tap.d -p <hidd-pid>
 */
#pragma D option quiet
#pragma D option bufsize=8m
#pragma D option aggsize=4m

dtrace:::BEGIN
{
    secs = 0;
    printf("== trace_tap funnel: inject taps now (auto-stops in ~9s) ==\n\n");
}

/* ---- the tap funnel: count entries per function ---- */
pid$target:MultitouchHID:*chk4chordCycling*:entry,
pid$target:MultitouchHID:*chk4newTapChord*:entry,
pid$target:MultitouchHID:*handleChordTaps*:entry,
pid$target:MultitouchHID:*selectTapChord*:entry,
pid$target:MultitouchHID:*selectSlideChord*:entry,
pid$target:MultitouchHID:*handleChordLiftoff*:entry,
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:entry
{
    @funnel[probefunc] = count();
}

/* ---- findMatchingChord: confirm matched chord motionCode (expect 0) ---- */
pid$target:MultitouchHID:*findMatchingChord*:entry
{ self->fmc = 1; self->combo = (uint32_t)arg1; }

pid$target:MultitouchHID:*findMatchingChord*:return
/self->fmc && arg1 != 0/
{
    this->m = *(uint32_t *)copyin((uintptr_t)arg1 + 0xe8, 4);
    @fmc_match[self->combo, this->m] = count();
    self->fmc = 0;
}
pid$target:MultitouchHID:*findMatchingChord*:return
/self->fmc && arg1 == 0/
{ @fmc_nomatch[self->combo] = count(); self->fmc = 0; }

/* ---- tapHasValidTimingAndStrength: live entry + return (the H2 gate itself) ---- */
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:entry
{ printf("[%u] >>> tapHasValidTimingAndStrength FIRED\n", (uint32_t)(timestamp/1000000)); }

pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:return
{ printf("[%u] <<< tapHasValidTimingAndStrength -> %d\n", (uint32_t)(timestamp/1000000), (int)arg1);
  @tapret[(int)arg1] = count(); }

/* ---- selectTapChord: live entry + return (does it pick a tap chord?) ---- */
pid$target:MultitouchHID:*selectTapChord*:return
{ printf("[%u]   selectTapChord -> 0x%llx\n", (uint32_t)(timestamp/1000000), (uint64_t)arg1); }

profile:::tick-1sec
{ secs++; }

profile:::tick-1sec
/secs >= 9/
{ exit(0); }

dtrace:::END
{
    printf("\n== TAP FUNNEL (function -> entry count) ==\n");
    printa("   %-44s x%@u\n", @funnel);
    printf("\n== findMatchingChord matches (combo, motion) ==\n");
    printa("   combo=0x%-4x motion=0x%-8x x%@u\n", @fmc_match);
    printf("\n== findMatchingChord NO-MATCH (combo) ==\n");
    printa("   combo=0x%-4x x%@u\n", @fmc_nomatch);
    printf("\n== tapHasValidTimingAndStrength returns (0=reject,1=accept) ==\n");
    printa("   ret=%d x%@u\n", @tapret);
}
