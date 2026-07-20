#ifndef MAVERICKS_CONNECT_SM_H
#define MAVERICKS_CONNECT_SM_H

typedef enum {
    CSM_IDLE, CSM_CONTROL_UP, CSM_BNB_FORMED, CSM_INTERRUPT_BOUND,
    CSM_INTERPOSED, CSM_MT_MODE, CSM_HANDLER_UP, CSM_STEADY
} csm_state_t;

typedef enum {
    CSM_EV_CONTROL_ATTACH, CSM_EV_CONTROL_OPEN,
    CSM_EV_INTERRUPT_PUBLISHED, CSM_EV_BNB_LISTENING, CSM_EV_INTERPOSE_OK,
    CSM_EV_MT_MODE_CONFIRMED, CSM_EV_HANDLER_SPAWNED, CSM_EV_FRAME,
    CSM_EV_DISCONNECT
} csm_event_t;

typedef enum {
    CSM_ACT_NONE, CSM_ACT_WAIT_CONTROL_OPEN, CSM_ACT_FORM_BNB,
    CSM_ACT_INSTALL_INTERPOSE, CSM_ACT_ENSURE_MT_MODE,
    CSM_ACT_INJECT_HANDLER_TRIGGER, CSM_ACT_MARK_STEADY, CSM_ACT_TEARDOWN
} csm_action_t;

typedef struct { csm_state_t next; csm_action_t action; } csm_result_t;

/* Pure: given current state + an observed event, return the next state and the
 * single action to perform on entering it. Unknown/duplicate events are no-ops
 * (stay in state, CSM_ACT_NONE) — idempotency (rule R2). DISCONNECT from any
 * state returns (CSM_IDLE, CSM_ACT_TEARDOWN). No IOKit, no I/O. */
csm_result_t csm_step(csm_state_t state, csm_event_t event);

typedef enum {
    CSM_TD_RESTORE_DELEGATE,   /* undo interpose: channel+0x110/+0x118 back to BNB's original */
    CSM_TD_TERMINATE_BNB,      /* terminate() + detach the manual BNB so none lingers */
    CSM_TD_RELEASE_REFS        /* release + null our refs (bnb ptr, saved delegate, channel) */
} csm_teardown_step_t;

/* The fixed teardown order the kernel must follow on CSM_ACT_TEARDOWN. */
void csm_teardown_steps(const csm_teardown_step_t **steps, unsigned *count);

#endif
