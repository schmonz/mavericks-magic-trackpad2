#include "vinput_demo_path.h"

/* 1024*cos(i * 3deg), i = 0..30 (0..90deg). Correct quarter wave: 1024 down to 0. */
static const short kCosQ[31] = {
  1024,1023,1018,1011,1002, 989, 974, 956, 936, 912, 887, 859, 828, 796, 761, 724,
   685, 644, 602, 558, 512, 465, 416, 367, 316, 265, 213, 160, 107,  54,   0 };

static int cos120(unsigned p) {           /* 1024*cos(2pi*p/120), p in 0..119 */
    p %= 120u;
    if (p <= 30)  return  kCosQ[p];
    if (p <= 60)  return -kCosQ[60 - p];
    if (p <= 90)  return -kCosQ[p - 60];
    return                kCosQ[120 - p];
}

void vinput_demo_point(unsigned phase, unsigned lmax_x, unsigned lmax_y,
                       unsigned *out_x, unsigned *out_y) {
    long cx = (long)lmax_x / 2, cy = (long)lmax_y / 2;
    long rx = (long)lmax_x * 4 / 10, ry = (long)lmax_y * 4 / 10;   /* radius ~0.4*lmax */
    long x = cx + (rx * cos120(phase)) / 1024;
    long y = cy + (ry * cos120((phase + 30u) % 120u)) / 1024;      /* +90deg = sine */
    if (x < 0) x = 0;
    if (x > (long)lmax_x) x = (long)lmax_x;
    if (y < 0) y = 0;
    if (y > (long)lmax_y) y = (long)lmax_y;
    *out_x = (unsigned)x;
    *out_y = (unsigned)y;
}
