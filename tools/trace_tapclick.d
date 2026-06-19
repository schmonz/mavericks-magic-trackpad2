#!/usr/sbin/dtrace -s
/*
 * trace_tapclick.d - observe the FULL tap->click decision chain in the native recognizer
 * (MultitouchHID, hosted in hidd) for REAL finger taps. Removes CGEvent-counting noise:
 * instead of guessing from posted clicks, we watch what the recognizer actually decides.
 *
 * Per tap, the interesting chain is:
 *   chk4newTapChord  -> selectTapChord        tap RECOGNIZED (gate passed) or not
 *   handleChordTaps  @+0x1c0 branches on this+0x408 (tap-drag cycle phase):
 *       this+0x408==1 -> MTTapDragManager::handleTapsForDrag   (drag/phantom path)
 *       else          -> MTTapDragManager::clearCycle          (clean single path)
 *   queueButtonClickEvent / queueButtonReleaseEvent            actual click output
 *   handleChordLiftoff                                          liftoff processing (2x => double)
 *
 * Reads this+0x408 (arg0 = `this` for the MTChordCyclingTrackpad methods). Timestamps (ms)
 * let us correlate the per-tap sequence and spot a phantom (a 2nd click ~ms later) or a
 * miss (recognized but no queueButtonClick).
 *
 *   sudo dtrace -q -s tools/trace_tapclick.d -p <hidd-pid>   (tap with real fingers ~20s)
 */
#pragma D option quiet
#pragma D option bufsize=8m

dtrace:::BEGIN { secs=0; printf("== tap->click trace: TAP WITH REAL FINGERS NOW (~20s) ==\n\n"); }

pid$target:MultitouchHID:*Trackpad*chk4newTapChord*:entry
{
    this->t = *(uint32_t *)copyin(arg0 + 0x408, 4);
    printf("[%u] chk4newTapChord        this+0x408=%u\n", (uint32_t)(timestamp/1000000), this->t);
    @chk = count();
}
pid$target:MultitouchHID:*selectTapChord*:entry
{
    printf("[%u]   selectTapChord   (tap RECOGNIZED)\n", (uint32_t)(timestamp/1000000));
    @sel = count();
}
pid$target:MultitouchHID:*Trackpad*handleChordTaps*:entry
{
    this->h = *(uint32_t *)copyin(arg0 + 0x408, 4);
    printf("[%u] handleChordTaps        this+0x408=%u\n", (uint32_t)(timestamp/1000000), this->h);
    @hct = count();
}
pid$target:MultitouchHID:*handleTapsForDrag*:entry
{
    this->st = *(uint32_t *)copyin(arg0 + 0xc, 4);   /* MTTapDragManager state @+0xc */
    printf("[%u]   -> handleTapsForDrag  state(0xc)=%u\n", (uint32_t)(timestamp/1000000), this->st);
    @drag = count();
}
pid$target:MultitouchHID:*clearCycle*:entry
{
    printf("[%u]   -> clearCycle         (clean single path)\n", (uint32_t)(timestamp/1000000));
    @clear = count();
}
pid$target:MultitouchHID:*queueButtonClickEvent*:entry
{
    printf("[%u]   == queueButtonClick   (CLICK queued)\n", (uint32_t)(timestamp/1000000));
    @click = count();
}
pid$target:MultitouchHID:*queueButtonReleaseEvent*:entry
{
    printf("[%u]   == queueButtonRelease (RELEASE queued)\n", (uint32_t)(timestamp/1000000));
    @rel = count();
}
pid$target:MultitouchHID:*handleChordLiftoff*:entry
{
    printf("[%u] handleChordLiftoff\n", (uint32_t)(timestamp/1000000));
    @lift = count();
}
pid$target:MultitouchHID:*dispatchEvents*:entry
{
    printf("[%u]   .. dispatchEvents phase=0x%x\n", (uint32_t)(timestamp/1000000), (int)arg2);
    @disp = count();
}

profile:::tick-1sec { secs++; }
profile:::tick-1sec /secs >= 20/ { exit(0); }

dtrace:::END
{
    printf("\n== summary (counts) ==\n");
    printa("   chk4newTapChord    x%@u\n", @chk);
    printa("   selectTapChord     x%@u\n", @sel);
    printa("   handleChordTaps    x%@u\n", @hct);
    printa("   handleChordLiftoff x%@u\n", @lift);
    printa("   handleTapsForDrag  x%@u\n", @drag);
    printa("   clearCycle         x%@u\n", @clear);
    printa("   queueButtonClick   x%@u\n", @click);
    printa("   queueButtonRelease x%@u\n", @rel);
}
