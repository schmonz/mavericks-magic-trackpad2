#!/usr/sbin/dtrace -s
/*
 * trace_tapchord.d - pinpoint WHICH bail branch in
 * MTChordCyclingTrackpad::chk4newTapChord rejects our tap.
 *
 * Funnel capture showed the tap path dies in chk4newTapChord (runs, but never reaches
 * selectTapChord). chk4newTapChord bails to 0x212dd (skipping the tap-chord select at
 * +0x96) on any of:
 *   gate1 +0x17  candidate gestureset (arg3) == NULL
 *   gate2 +0x2b  stat+0x50 (double) <= stat+0x80 (double)
 *   gate3 +0x3b  virtual predicate *0x30(this) returns false
 *   gate4 +0x4f.. finger-count/field checks
 * Reach-probes at offsets tell us how far we get; entry-probe dumps the gate inputs.
 *
 *   sudo dtrace -q -s tools/trace_tapchord.d -p <hidd-pid>
 */
#pragma D option quiet
#pragma D option bufsize=8m

dtrace:::BEGIN { secs=0; printf("== chk4newTapChord gate trace: inject taps now (~9s) ==\n\n"); }

pid$target:MultitouchHID:*Trackpad*chk4newTapChord*:entry
{
    this->obj = arg0; this->stat = arg1; this->gs = arg3;
    this->d50 = *(uint64_t *)copyin(this->stat + 0x50, 8);
    this->d80 = *(uint64_t *)copyin(this->stat + 0x80, 8);
    this->t408 = *(uint32_t *)copyin(this->obj + 0x408, 4);
    this->t410 = *(uint32_t *)copyin(this->obj + 0x410, 4);
    this->sae = *(uint8_t  *)copyin(this->stat + 0xae, 1);
    this->sb7 = *(uint8_t  *)copyin(this->stat + 0xb7, 1);
    printf("[%u] ENTRY gs(arg3)=0x%llx  stat+0x50=0x%016llx stat+0x80=0x%016llx  this+0x408=%u this+0x410=%u stat+0xae=%u stat+0xb7=%u\n",
        (uint32_t)(timestamp/1000000), (uint64_t)this->gs,
        this->d50, this->d80, this->t408, this->t410, this->sae, this->sb7);
    @entry = count();
}

/* gestureset fields (only if non-null) */
pid$target:MultitouchHID:*Trackpad*chk4newTapChord*:entry
/arg3 != 0/
{
    this->gd8 = *(uint32_t *)copyin(arg3 + 0xd8, 4);
    this->ge0 = *(uint32_t *)copyin(arg3 + 0xe0, 4);
    this->gdc = *(uint32_t *)copyin(arg3 + 0xdc, 4);
    printf("        gestureset+0xd8(fingers?)=%u +0xe0=%u +0xdc=%u\n", this->gd8, this->ge0, this->gdc);
}

/* selectTapChord reached? (the non-bail outcome) */
pid$target:MultitouchHID:*selectTapChord*:entry { @sel = count(); }

profile:::tick-1sec { secs++; }
profile:::tick-1sec /secs >= 9/ { exit(0); }

dtrace:::END
{
    printf("\n== summary ==\n");
    printa("   chk4newTapChord ENTRY x%@u\n", @entry);
    printa("   selectTapChord  ENTRY x%@u\n", @sel);
    printf("   gate2 bails iff stat+0x50 <= stat+0x80 (compare the doubles above)\n");
}
