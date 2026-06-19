#!/usr/sbin/dtrace -s
/* trace_button.d - find what actually POSTS the tap-click button event (the CGEvent source),
 * since queueButtonClickEvent count != CGEvent count. Traces the button-emit path:
 *   MTAppendMouseButtonEvent           - appends the mouse-button HID event (likely the poster)
 *   MTTrackpadHIDManager::handleButtonState / forwardButtonState - device button-state -> event
 * Args: arg1/arg2 are the button-state words (mask). Timestamps (ms) to spot the ~6ms double.
 *   sudo dtrace -q -s tools/trace_button.d -p <hidd>
 */
#pragma D option quiet
#pragma D option bufsize=8m
dtrace:::BEGIN { secs=0; printf("== button-post trace ==\n"); }

pid$target:MultitouchHID:MTAppendMouseButtonEvent:entry {
    printf("[%u] MTAppendMouseButtonEvent arg0=0x%x arg1=0x%x arg2=0x%x\n",
        (uint32_t)(timestamp/1000000), (int)arg0, (int)arg1, (int)arg2);
    @app = count();
}
pid$target:MultitouchHID:*forwardButtonState*:entry {
    printf("[%u]   forwardButtonState a1=0x%x a2=0x%x\n",
        (uint32_t)(timestamp/1000000), (int)arg1, (int)arg2);
    @fwd = count();
}
pid$target:MultitouchHID:*handleButtonState*:entry {
    printf("[%u]   handleButtonState a1=0x%x a2=0x%x\n",
        (uint32_t)(timestamp/1000000), (int)arg1, (int)arg2);
    @hbs = count();
}
profile:::tick-1sec { secs++; }
profile:::tick-1sec /secs >= 14/ { exit(0); }
dtrace:::END {
    printf("\n== counts ==\n");
    printa("  MTAppendMouseButtonEvent %@u\n", @app);
    printa("  forwardButtonState       %@u\n", @fwd);
    printa("  handleButtonState        %@u\n", @hbs);
}
