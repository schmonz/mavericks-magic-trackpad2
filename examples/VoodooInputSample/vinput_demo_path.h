#ifndef VINPUT_DEMO_PATH_H
#define VINPUT_DEMO_PATH_H
/* Pure demo path: a contact circling within [0..lmax_x] x [0..lmax_y]. No IOKit.
 * C linkage: this .c is compiled as C++ INTO the sample kext but called from a .cpp — without
 * the extern "C" guard kxld fails to resolve the mangled symbols at LOAD. */
#define VINPUT_DEMO_PERIOD 120u   /* frames per full circle */
#ifdef __cplusplus
extern "C" {
#endif
void vinput_demo_point(unsigned phase, unsigned lmax_x, unsigned lmax_y,
                       unsigned *out_x, unsigned *out_y);
#ifdef __cplusplus
}
#endif
#endif
