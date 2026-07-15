#include "mt2_synth_teardown.h"

void mt2_synth_teardown_run(const mt2_synth_teardown_ops_t *ops) {
    if (!ops) return;
    if (ops->clear_ready) ops->clear_ready(ops->ctx);
    if (ops->term_shell)  ops->term_shell(ops->ctx);
    if (ops->term_amd)    ops->term_amd(ops->ctx);
    if (ops->release_amd) ops->release_amd(ops->ctx);
    if (ops->release_wl)  ops->release_wl(ops->ctx);
}
