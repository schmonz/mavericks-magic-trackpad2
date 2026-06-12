#ifndef TEST_H
#define TEST_H
#include <stdio.h>
#include <stdlib.h>
static int test_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_failures++; \
    } \
} while (0)
#define CHECK_EQ(a,b) do { \
    long _a=(long)(a), _b=(long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d: %s (%ld) != %s (%ld)\n", \
            __FILE__, __LINE__, #a, _a, #b, _b); \
        test_failures++; \
    } \
} while (0)
#define TEST_MAIN() int main(void){ run_tests(); \
    if (test_failures){ fprintf(stderr,"%d failure(s)\n",test_failures); return 1;} \
    printf("OK\n"); return 0; }
#endif
