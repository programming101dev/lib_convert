/*
 * Unity tests for src/integer.c -- the p101_parse_* family.
 *
 * These functions turn UNTRUSTED text (argv, config files, network input) into
 * fixed-width integers, so their edge cases are security-relevant, not academic.
 * Every test below therefore checks BOTH halves of the contract:
 *
 *   1. the returned value, and
 *   2. whether an error was raised.
 *
 * A parser that returns a plausible-looking number without raising an error on
 * bad input is exactly how "-1" becomes a 4-billion-byte allocation. Checking
 * only the return value would let that through.
 *
 * Note the module's convention, which the tests pin down deliberately:
 *   - unparsable input  -> raises AND returns the caller's default_value
 *   - out-of-range input-> raises AND returns the caller's default_value
 */
#include "p101_convert/errors.h"
#include <p101_convert/integer.h>
#include "unity.h"
#include <limits.h>
#include <stdint.h>

static struct p101_error *error;
static struct p101_env   *env;

void setUp(void)
{
    error = p101_error_create(false);
    env   = p101_env_create(error, NULL);
}

void tearDown(void)
{
    p101_env_destroy(env);
    p101_error_destroy(error);
}

/* Each case in a table-driven test needs a clean error slate. */
static void reset(void)
{
    p101_error_reset(error);
}

/* ---------------------------------------------------------------- happy path */

static void test_parse_int_accepts_plain_decimal(void)
{
    TEST_ASSERT_EQUAL_INT(42, p101_parse_int(env, error, "42", -1));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

static void test_parse_int_accepts_explicit_sign(void)
{
    TEST_ASSERT_EQUAL_INT(42, p101_parse_int(env, error, "+42", -1));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_INT(-42, p101_parse_int(env, error, "-42", -1));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

static void test_parse_int_accepts_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, p101_parse_int(env, error, "0", -1));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

static void test_parse_int_skips_leading_whitespace(void)
{
    /* strtoimax()'s documented behaviour, inherited on purpose: leading blanks
     * are fine, TRAILING ones are not (see test_parse_int_rejects_trailing_*). */
    TEST_ASSERT_EQUAL_INT(7, p101_parse_int(env, error, "   7", -1));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

static void test_parse_int_hits_the_exact_bounds(void)
{
    TEST_ASSERT_EQUAL_INT(INT_MAX, p101_parse_int(env, error, "2147483647", 0));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_INT(INT_MIN, p101_parse_int(env, error, "-2147483648", 0));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

/* -------------------------------------------------------------- malformed in */

static void test_parse_int_rejects_empty_string(void)
{
    TEST_ASSERT_EQUAL_INT(-1, p101_parse_int(env, error, "", -1));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_int_rejects_only_whitespace(void)
{
    TEST_ASSERT_EQUAL_INT(-1, p101_parse_int(env, error, "   ", -1));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_int_rejects_non_numeric(void)
{
    static const char *const bad[] = {"abc", "-", "+", "--1", "0x", ".5", "e5"};
    size_t                   i;

    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        reset();
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, p101_parse_int(env, error, bad[i], -1), bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(p101_error_has_error(error), bad[i]);
    }
}

static void test_parse_int_rejects_trailing_garbage(void)
{
    /* The whole point of the endptr check: "12abc" must NOT quietly become 12. */
    static const char *const bad[] = {"12abc", "12 ", "1.5", "0x1F", "42\n", "1,000"};
    size_t                   i;

    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        reset();
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, p101_parse_int(env, error, bad[i], -1), bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(p101_error_has_error(error), bad[i]);
    }
}

/* ------------------------------------------------------------ out of range   */

static void test_parse_int_rejects_above_max(void)
{
    TEST_ASSERT_EQUAL_INT(0, p101_parse_int(env, error, "2147483648", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    TEST_ASSERT_TRUE(p101_error_is_error(error, P101_ERROR_USER, P101_CONVERT_ERROR_RANGE));
}

static void test_parse_int_rejects_below_min(void)
{
    TEST_ASSERT_EQUAL_INT(0, p101_parse_int(env, error, "-2147483649", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    TEST_ASSERT_TRUE(p101_error_is_error(error, P101_ERROR_USER, P101_CONVERT_ERROR_RANGE));
}

static void test_parse_int_rejects_intmax_overflow(void)
{
    /* Too wide even for strtoimax: ERANGE comes back from the p101_c wrapper,
     * and the function bails out with the caller's default, not a clamp. */
    TEST_ASSERT_EQUAL_INT(-1, p101_parse_int(env, error, "99999999999999999999999999", -1));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_int8_and_int16_ranges(void)
{
    TEST_ASSERT_EQUAL_INT8(INT8_MAX, p101_parse_int8_t(env, error, "127", 0));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_INT8(0, p101_parse_int8_t(env, error, "128", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_INT16(0, p101_parse_int16_t(env, error, "-32769", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

/* ------------------------------------------------------- signed sub-families */

static void test_parse_negative_int_requires_a_negative(void)
{
    TEST_ASSERT_EQUAL_INT(-5, p101_parse_negative_int(env, error, "-5", 0));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    reset();
    /* Zero is NOT negative: max_value is -1, so 0 trips the upper bound. */
    TEST_ASSERT_EQUAL_INT(0, p101_parse_negative_int(env, error, "0", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_INT(0, p101_parse_negative_int(env, error, "5", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_positive_int_requires_positive(void)
{
    TEST_ASSERT_EQUAL_INT(5, p101_parse_positive_int(env, error, "5", -1));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_INT(-1, p101_parse_positive_int(env, error, "0", -1));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_INT(-1, p101_parse_positive_int(env, error, "-5", -1));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

/* ----------------------------------------------------------------- unsigned  */

static void test_parse_unsigned_accepts_range(void)
{
    TEST_ASSERT_EQUAL_UINT16(65535, p101_parse_uint16_t(env, error, "65535", 0));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_UINT16(0, p101_parse_uint16_t(env, error, "65536", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_unsigned_rejects_negative_input(void)
{
    /* THE one that matters. strtoumax() happily wraps "-1" round to UINTMAX_MAX,
     * so without an explicit sign check a negative string becomes a huge
     * positive number -- the classic path to an absurd size_t/length. Every
     * width in the family must refuse it, INCLUDING the widest one, where the
     * range check cannot save us because the wrapped value is already in range. */
    TEST_ASSERT_EQUAL_UINT8(0, p101_parse_unsigned_char(env, error, "-1", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_UINT16(0, p101_parse_uint16_t(env, error, "-1", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_UINT(0, p101_parse_unsigned_int(env, error, "-1", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)p101_parse_unsigned_long_long(env, error, "-1", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)p101_parse_unsigned_long(env, error, "-9999999999999999999999", 0));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_unsigned_rejects_negative_zero_too(void)
{
    /* "-0" parses to 0 without wrapping, so it would slip past a value-based
     * check; it is still a negative literal and must be refused. */
    TEST_ASSERT_EQUAL_UINT(9, p101_parse_unsigned_int(env, error, "-0", 9));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_unsigned_rejects_garbage(void)
{
    TEST_ASSERT_EQUAL_UINT(7, p101_parse_unsigned_int(env, error, "", 7));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_UINT(7, p101_parse_unsigned_int(env, error, "12abc", 7));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_null_input_raises_and_returns_default(void)
{
    TEST_ASSERT_EQUAL_INT(7, p101_parse_int(env, error, NULL, 7));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    reset();
    TEST_ASSERT_EQUAL_UINT(9, p101_parse_unsigned_int(env, error, NULL, 9U));
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_all_public_integer_widths(void)
{
    TEST_ASSERT_EQUAL_INT(1, p101_parse_char(env, error, "1", 0));
    reset();
    TEST_ASSERT_EQUAL_INT(2, p101_parse_short(env, error, "2", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(3, p101_parse_long(env, error, "3", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(4, p101_parse_long_long(env, error, "4", 0));
    reset();
    TEST_ASSERT_EQUAL_UINT(5, p101_parse_unsigned_short(env, error, "5", 0));
    reset();
    TEST_ASSERT_EQUAL_UINT64(6, (uint64_t)p101_parse_unsigned_long(env, error, "6", 0));
    reset();
    TEST_ASSERT_EQUAL_UINT64(7, (uint64_t)p101_parse_unsigned_long_long(env, error, "7", 0));
    reset();
    TEST_ASSERT_EQUAL_INT32(8, p101_parse_int32_t(env, error, "8", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(9, p101_parse_int64_t(env, error, "9", 0));
    reset();
    TEST_ASSERT_EQUAL_UINT8(10, p101_parse_uint8_t(env, error, "10", 0));
    reset();
    TEST_ASSERT_EQUAL_UINT32(11, p101_parse_uint32_t(env, error, "11", 0));
    reset();
    TEST_ASSERT_EQUAL_UINT64(12, p101_parse_uint64_t(env, error, "12", 0));
    reset();

    TEST_ASSERT_EQUAL_INT(-1, p101_parse_negative_char(env, error, "-1", 0));
    reset();
    TEST_ASSERT_EQUAL_INT(-2, p101_parse_negative_short(env, error, "-2", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(-3, p101_parse_negative_long(env, error, "-3", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(-4, p101_parse_negative_long_long(env, error, "-4", 0));
    reset();
    TEST_ASSERT_EQUAL_INT8(-5, p101_parse_negative_int8_t(env, error, "-5", 0));
    reset();
    TEST_ASSERT_EQUAL_INT16(-6, p101_parse_negative_int16_t(env, error, "-6", 0));
    reset();
    TEST_ASSERT_EQUAL_INT32(-7, p101_parse_negative_int32_t(env, error, "-7", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(-8, p101_parse_negative_int64_t(env, error, "-8", 0));
    reset();

    TEST_ASSERT_EQUAL_INT(1, p101_parse_positive_char(env, error, "1", 0));
    reset();
    TEST_ASSERT_EQUAL_INT(2, p101_parse_positive_short(env, error, "2", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(3, p101_parse_positive_long(env, error, "3", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(4, p101_parse_positive_long_long(env, error, "4", 0));
    reset();
    TEST_ASSERT_EQUAL_INT8(5, p101_parse_positive_int8_t(env, error, "5", 0));
    reset();
    TEST_ASSERT_EQUAL_INT16(6, p101_parse_positive_int16_t(env, error, "6", 0));
    reset();
    TEST_ASSERT_EQUAL_INT32(7, p101_parse_positive_int32_t(env, error, "7", 0));
    reset();
    TEST_ASSERT_EQUAL_INT64(8, p101_parse_positive_int64_t(env, error, "8", 0));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

static void test_negative_char_is_independent_of_plain_char_signedness(void)
{
    signed char parsed;

    parsed = p101_parse_negative_char(env, error, "-1", 0);
    TEST_ASSERT_EQUAL_INT(-1, parsed);
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_int_accepts_plain_decimal);
    RUN_TEST(test_parse_int_accepts_explicit_sign);
    RUN_TEST(test_parse_int_accepts_zero);
    RUN_TEST(test_parse_int_skips_leading_whitespace);
    RUN_TEST(test_parse_int_hits_the_exact_bounds);
    RUN_TEST(test_parse_int_rejects_empty_string);
    RUN_TEST(test_parse_int_rejects_only_whitespace);
    RUN_TEST(test_parse_int_rejects_non_numeric);
    RUN_TEST(test_parse_int_rejects_trailing_garbage);
    RUN_TEST(test_parse_int_rejects_above_max);
    RUN_TEST(test_parse_int_rejects_below_min);
    RUN_TEST(test_parse_int_rejects_intmax_overflow);
    RUN_TEST(test_parse_int8_and_int16_ranges);
    RUN_TEST(test_parse_negative_int_requires_a_negative);
    RUN_TEST(test_parse_positive_int_requires_positive);
    RUN_TEST(test_parse_unsigned_accepts_range);
    RUN_TEST(test_parse_unsigned_rejects_negative_input);
    RUN_TEST(test_parse_unsigned_rejects_negative_zero_too);
    RUN_TEST(test_parse_unsigned_rejects_garbage);
    RUN_TEST(test_null_input_raises_and_returns_default);
    RUN_TEST(test_all_public_integer_widths);
    RUN_TEST(test_negative_char_is_independent_of_plain_char_signedness);
    return UNITY_END();
}
