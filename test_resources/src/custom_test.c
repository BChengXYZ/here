#include "custom_ops.h"
#include "io.h"
#include "power.h"

int main(void) {
    TEST_START("custom_test");

    unsigned long long a = 0x0102030405060708ULL;
    unsigned long long b = 0x1112131415161718ULL;
    unsigned long long sum = padd8(a, b);
    printf("padd8(%llx, %llx) = %llx\n", a, b, sum);

    unsigned long long x = 0x01020304ULL;
    unsigned long long y = 0x05060708ULL;
    long long dot = pdot8(x, y);
    printf("pdot8(%llx, %llx) = %lld (%llx)\n", x, y, dot,
           (unsigned long long)dot);

    pass();
    return 0;
}
