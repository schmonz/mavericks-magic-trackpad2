#!/usr/sbin/dtrace -s
/*
 * trace_pathts.d - round-6: dump MTPathStageTimestamps + see if the hand-stats stage
 * clock advances with our injected frames.
 *
 * MTHandStatistics::updateStatsWithFingerPath(int stage, MTContactIdentity finger,
 *   MTPoint, MTPathStageTimestamps const& pathTS, ...). ABI: arg0=this, arg1=int(esi),
 *   arg2=finger(rdx), arg3=pathTS(rcx). pathTS doubles at +0x0,+0x10,+0x28,+0x30;
 *   gate in chk4newTapChord needs pathTS+0x10 > pathTS+0x28.
 *
 *   sudo dtrace -q -s tools/trace_pathts.d -p <hidd-pid>   (drive taps via synth_tap)
 */
#pragma D option quiet
#pragma D option bufsize=8m

dtrace:::BEGIN { secs=0; printf("== pathTS dump: inject taps now (~9s) ==\n\n"); }

pid$target:MultitouchHID:*updateStatsWithFingerPath*:entry
/arg3 != 0/
{
    this->p0  = *(uint64_t *)copyin(arg3 + 0x00, 8);
    this->p10 = *(uint64_t *)copyin(arg3 + 0x10, 8);
    this->p28 = *(uint64_t *)copyin(arg3 + 0x28, 8);
    this->p30 = *(uint64_t *)copyin(arg3 + 0x30, 8);
    printf("[%u] stage=%d finger=%d  pathTS +0x0=0x%016llx +0x10=0x%016llx +0x28=0x%016llx +0x30=0x%016llx\n",
        (uint32_t)(timestamp/1000000), (int)arg1, (int)arg2,
        this->p0, this->p10, this->p28, this->p30);
    @calls = count();
}

pid$target:MultitouchHID:*updateStatsWithFingerPath*:entry
/arg3 == 0/
{ @nullp = count(); }

profile:::tick-1sec { secs++; }
profile:::tick-1sec /secs >= 9/ { exit(0); }

dtrace:::END
{
    printf("\n== updateStatsWithFingerPath calls (pathTS non-null) ==\n");
    printa("   x%@u\n", @calls);
    printf("== calls with null pathTS ==\n");
    printa("   x%@u\n", @nullp);
}
