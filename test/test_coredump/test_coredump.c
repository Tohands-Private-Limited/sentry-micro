/**
 * Host tests for the architecture name tables in `sentry_coredump.c`.
 *
 * These strings are issue titles. Change one and every existing issue for that crash splits
 * in two: the old title keeps its history, the new one starts from zero, and nothing in the
 * build says it happened. Both ports (ESP-IDF and Zephyr) read them from here, so pinning
 * them here pins both.
 */

#include <unity.h>

#include "sentry_coredump.h"

void setUp(void) { }
void tearDown(void) { }

static void test_names_the_common_xtensa_faults_as_the_panic_handler_prints_them(void)
{
    TEST_ASSERT_EQUAL_STRING("IllegalInstruction", sentry_xtensa_exccause_name(0));
    TEST_ASSERT_EQUAL_STRING("IntegerDivideByZero", sentry_xtensa_exccause_name(6));
    TEST_ASSERT_EQUAL_STRING("InstFetchProhibited", sentry_xtensa_exccause_name(20));
    TEST_ASSERT_EQUAL_STRING("LoadProhibited", sentry_xtensa_exccause_name(28));
    TEST_ASSERT_EQUAL_STRING("StoreProhibited", sentry_xtensa_exccause_name(29));
}

static void test_an_unnamed_xtensa_cause_is_empty_not_null(void)
{
    /* 7 and 10 are gaps in the table; 63 is past it, where Zephyr's own software causes
     * live. Empty rather than NULL so the result can go straight into a format string. */
    TEST_ASSERT_EQUAL_STRING("", sentry_xtensa_exccause_name(7));
    TEST_ASSERT_EQUAL_STRING("", sentry_xtensa_exccause_name(10));
    TEST_ASSERT_EQUAL_STRING("", sentry_xtensa_exccause_name(63));
    TEST_ASSERT_EQUAL_STRING("", sentry_xtensa_exccause_name(0xffffffffu));
}

static void test_only_load_store_causes_carry_a_data_address(void)
{
    const uint32_t with_address[] = { 3, 9, 24, 25, 26, 28, 29 };
    for (unsigned i = 0; i < sizeof(with_address) / sizeof(with_address[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            sentry_xtensa_exccause_has_data_addr(with_address[i]), "load/store cause");
    }

    /* Everything else leaves EXCVADDR stale; an address on these would be invented. */
    for (uint32_t cause = 0; cause < 64; cause++) {
        bool expected = false;
        for (unsigned i = 0; i < sizeof(with_address) / sizeof(with_address[0]); i++) {
            if (with_address[i] == cause) {
                expected = true;
            }
        }
        TEST_ASSERT_EQUAL(expected, sentry_xtensa_exccause_has_data_addr(cause));
    }
}

static void test_names_the_common_riscv_faults_as_the_panic_handler_prints_them(void)
{
    TEST_ASSERT_EQUAL_STRING("IllegalInstruction", sentry_riscv_mcause_name(2));
    TEST_ASSERT_EQUAL_STRING("LoadAccessFault", sentry_riscv_mcause_name(5));
    TEST_ASSERT_EQUAL_STRING("StoreAccessFault", sentry_riscv_mcause_name(7));
    /* Three privilege levels, one title: which mode made the call does not change the bug. */
    TEST_ASSERT_EQUAL_STRING("EnvironmentCall", sentry_riscv_mcause_name(8));
    TEST_ASSERT_EQUAL_STRING("EnvironmentCall", sentry_riscv_mcause_name(11));
}

static void test_an_unnamed_riscv_cause_is_empty_not_null(void)
{
    TEST_ASSERT_EQUAL_STRING("", sentry_riscv_mcause_name(10));
    TEST_ASSERT_EQUAL_STRING("", sentry_riscv_mcause_name(14));
    /* An interrupt sets the high bit; it is not an exception and has no name here. */
    TEST_ASSERT_EQUAL_STRING("", sentry_riscv_mcause_name(0x80000007u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_names_the_common_xtensa_faults_as_the_panic_handler_prints_them);
    RUN_TEST(test_an_unnamed_xtensa_cause_is_empty_not_null);
    RUN_TEST(test_only_load_store_causes_carry_a_data_address);
    RUN_TEST(test_names_the_common_riscv_faults_as_the_panic_handler_prints_them);
    RUN_TEST(test_an_unnamed_riscv_cause_is_empty_not_null);
    return UNITY_END();
}
