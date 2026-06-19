#!/usr/sbin/dtrace -s
/*
 * trace_gateB.d - find the first failing sub-gate inside
 * MTChordCycling::tapHasValidTimingAndStrength (it now FIRES but returns false).
 *
 * Every fail-branch jumps to 0x22932 (return r14b=0). A reach-counter at the fall-through
 * AFTER each gate shows how far we get: the first label whose count drops to 0 is the gate
 * that rejects our tap. Offsets from the @0x227d0 disasm.
 *
 *   sudo dtrace -q -s tools/trace_gateB.d -p <hidd-pid>   (drive taps via synth_tap)
 */
#pragma D option quiet
#pragma D option bufsize=8m

dtrace:::BEGIN { secs=0; printf("== gate B reach trace: inject taps now (~9s) ==\n\n"); }

pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:entry
{
    @reach["00 entry"] = count();
    /* gate 1 (strength): need stats+0xc0 (float) > this+0x170 (float) */
    this->strength = *(uint32_t *)copyin(arg1 + 0xc0, 4);
    this->thresh   = *(uint32_t *)copyin(arg0 + 0x170, 4);
    /* gate 2 (duration): stats+0x8 - stats+0x50 vs gTimingPrefs+0x30 */
    this->t8  = *(uint64_t *)copyin(arg1 + 0x08, 8);
    this->t50 = *(uint64_t *)copyin(arg1 + 0x50, 8);
    printf("[%u] strength=0x%08x thresh=0x%08x  t8=0x%016llx t50=0x%016llx\n",
        (uint32_t)(timestamp/1000000), this->strength, this->thresh, this->t8, this->t50);
}
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:31  { @reach["01 past strength @+0x31"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:5a  { @reach["02 past duration @+0x5a"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:75  { @reach["03 past gate3   @+0x75"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:8b  { @reach["04 past gate4   @+0x8b"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:a1  { @reach["05 past gate5   @+0xa1"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:b8  { @reach["06 past gate6   @+0xb8"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:d2  { @reach["07 past gate7   @+0xd2"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:e8  { @reach["08 past gate8   @+0xe8"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:fe  { @reach["09 past gate9   @+0xfe"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:116 { @reach["10 past gate10  @+0x116"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:131 { @reach["11 provisional PASS @+0x131"] = count(); }
pid$target:MultitouchHID:*tapHasValidTimingAndStrength*:147 { @reach["12 final-true path @+0x147"] = count(); }

profile:::tick-1sec { secs++; }
profile:::tick-1sec /secs >= 9/ { exit(0); }

dtrace:::END
{
    printf("\n== reach (first label that drops to 0 = the failing gate) ==\n");
    printa("   %-30s x%@u\n", @reach);
}
