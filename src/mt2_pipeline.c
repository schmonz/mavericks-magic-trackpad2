#include "mt2_pipeline.h"
int mt2_settle_passed(uint32_t now_ms, uint32_t settle_until_ms) {
    return now_ms >= settle_until_ms ? 1 : 0;
}
