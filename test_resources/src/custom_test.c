#include "custom_ops.h"
#include "io.h"
#include "power.h"

typedef struct {
    unsigned long long a;
    unsigned long long b;
} BinCase;

static unsigned long long golden_padd8(unsigned long long a, unsigned long long b) {
    unsigned long long result = 0;
    for (int i = 0; i < 8; i++) {
        unsigned long long shift = (unsigned long long)i * 8;
        unsigned long long x = (a >> shift) & 0xff;
        unsigned long long y = (b >> shift) & 0xff;
        result |= ((x + y) & 0xff) << shift;
    }
    return result;
}

static unsigned long long golden_padd8s(unsigned long long a, unsigned long long b) {
    unsigned long long result = 0;
    for (int i = 0; i < 8; i++) {
        unsigned long long shift = (unsigned long long)i * 8;
        unsigned long long x = (a >> shift) & 0xff;
        unsigned long long y = (b >> shift) & 0xff;
        unsigned long long sum = x + y;
        if (sum > 0xff) {
            sum = 0xff;
        }
        result |= sum << shift;
    }
    return result;
}

static long long golden_pdot8(unsigned long long a, unsigned long long b) {
    int acc = 0;
    for (int i = 0; i < 4; i++) {
        int x = (int)(signed char)((a >> (i * 8)) & 0xff);
        int y = (int)(signed char)((b >> (i * 8)) & 0xff);
        acc += x * y;
    }
    return (long long)acc;
}

static int check_padd8(const BinCase* cases, int count) {
    int passed = 0;
    for (int i = 0; i < count; i++) {
        unsigned long long hw = padd8(cases[i].a, cases[i].b);
        unsigned long long sw = golden_padd8(cases[i].a, cases[i].b);
        if (hw == sw) {
            printf("[PASS] padd8  case %d: %llx + %llx = %llx\n", i, cases[i].a,
                   cases[i].b, hw);
            passed++;
        } else {
            printf("[FAIL] padd8  case %d: %llx + %llx -> hw=%llx sw=%llx\n", i,
                   cases[i].a, cases[i].b, hw, sw);
        }
    }
    return passed;
}

static int check_padd8s(const BinCase* cases, int count) {
    int passed = 0;
    for (int i = 0; i < count; i++) {
        unsigned long long hw = padd8s(cases[i].a, cases[i].b);
        unsigned long long sw = golden_padd8s(cases[i].a, cases[i].b);
        if (hw == sw) {
            printf("[PASS] padd8s case %d: %llx + %llx = %llx\n", i, cases[i].a,
                   cases[i].b, hw);
            passed++;
        } else {
            printf("[FAIL] padd8s case %d: %llx + %llx -> hw=%llx sw=%llx\n", i,
                   cases[i].a, cases[i].b, hw, sw);
        }
    }
    return passed;
}

static int check_pdot8(const BinCase* cases, int count) {
    int passed = 0;
    for (int i = 0; i < count; i++) {
        long long hw = pdot8(cases[i].a, cases[i].b);
        long long sw = golden_pdot8(cases[i].a, cases[i].b);
        if (hw == sw) {
            printf("[PASS] pdot8  case %d: %llx . %llx = %lld\n", i, cases[i].a,
                   cases[i].b, hw);
            passed++;
        } else {
            printf("[FAIL] pdot8  case %d: %llx . %llx -> hw=%lld sw=%lld\n", i,
                   cases[i].a, cases[i].b, hw, sw);
        }
    }
    return passed;
}

int main(void) {
    static const BinCase padd8_cases[] = {
        {0x0102030405060708ULL, 0x1112131415161718ULL},
        {0xFFFFFFFFFFFFFFFFULL, 0x0101010101010101ULL},
        {0x8080808080808080ULL, 0x8080808080808080ULL},
        {0x0000000000000000ULL, 0x0000000000000000ULL},
        {0x00FF00FF00FF00FFULL, 0xFF00FF00FF00FF00ULL},
        {0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL},
    };
    static const BinCase padd8s_cases[] = {
        {0xF0F0F0F0F0F0F0F0ULL, 0x2020202020202020ULL},
        {0xFFFFFFFFFFFFFFFFULL, 0x0101010101010101ULL},
        {0x0102030405060708ULL, 0x1112131415161718ULL},
        {0x0000000000000000ULL, 0x0000000000000000ULL},
    };
    static const BinCase pdot8_cases[] = {
        {0x01020304ULL, 0x05060708ULL},
        {0xFFFFFF80ULL, 0xFFFFFF80ULL},
        {0x01020304ULL, 0xFFFFFFFFULL},
        {0x80808080ULL, 0x80808080ULL},
        {0x7F7F7F7FULL, 0x7F7F7F7FULL},
        {0x00000000ULL, 0x00000000ULL},
    };

    TEST_START("custom_test");
    printf("=== Custom instruction backend verification ===\n");

    int total = 0;
    int passed = 0;

    total += sizeof(padd8_cases) / sizeof(padd8_cases[0]);
    passed += check_padd8(padd8_cases, sizeof(padd8_cases) / sizeof(padd8_cases[0]));

    total += sizeof(padd8s_cases) / sizeof(padd8s_cases[0]);
    passed += check_padd8s(padd8s_cases, sizeof(padd8s_cases) / sizeof(padd8s_cases[0]));

    total += sizeof(pdot8_cases) / sizeof(pdot8_cases[0]);
    passed += check_pdot8(pdot8_cases, sizeof(pdot8_cases) / sizeof(pdot8_cases[0]));

    if (passed == total) {
        printf("=== ALL %d TESTS PASSED ===\n", total);
        pass();
    } else {
        printf("=== VERIFICATION FAILED (%d/%d) ===\n", passed, total);
        PowerOff();
    }

    return 0;
}
