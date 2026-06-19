#!/usr/sbin/dtrace -s
/* trace_btnstack.d - capture the CALLER of each MTAppendMouseButtonEvent (down/up), to find
 * what emits the phantom (2nd) click. arg1: 1=down 0=up. ustack shows the recognizer caller. */
#pragma D option quiet
#pragma D option bufsize=16m
dtrace:::BEGIN { secs=0; printf("== button-append caller trace ==\n"); }
pid$target:MultitouchHID:MTAppendMouseButtonEvent:entry {
    printf("[%u] btn=%d\n", (uint32_t)(timestamp/1000000), (int)arg1);
    ustack(5);
    printf("---\n");
}
profile:::tick-1sec { secs++; }
profile:::tick-1sec /secs >= 13/ { exit(0); }
