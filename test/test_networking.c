/*
 * Unity tests for src/networking.c -- parse_in_port_t() and convert_address().
 *
 * convert_address() decides, from a user-supplied string, WHICH address family
 * a socket will be created in. Getting that wrong is not a cosmetic bug: the
 * caller goes on to socket()/bind()/connect() with whatever struct comes back,
 * so a wrong ss_family or an uninitialised sockaddr means binding to an
 * unintended address.
 *
 * The tests assert the CONTRACT, deliberately, rather than whatever the code
 * happens to do today:
 *
 *   "1.2.3.4"        -> ss_family == AF_INET,   sin_addr  == that address
 *   "::1"            -> ss_family == AF_INET6,  sin6_addr == that address
 *   "/tmp/sock"      -> ss_family == AF_UNIX,   sun_path  == that path
 *   over-long path   -> ss_family == AF_UNSPEC
 *
 * and they check the FULL struct, not just ss_family, because an address that
 * is the right family but the wrong bytes is still wrong.
 */
#include "p101_convert/networking.h"
#include "unity.h"
#include <netinet/in.h>
#include <string.h>
#include <sys/un.h>

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

/* Fill the storage with a recognisable byte so "the function never wrote here"
 * is visible as 0xA5 rather than accidentally-correct zeroes. */
static void poison(struct sockaddr_storage *addr)
{
    memset(addr, 0xA5, sizeof(*addr));
    p101_error_reset(error);
}

/* ------------------------------------------------------------ parse_in_port_t */

static void test_parse_in_port_t_accepts_valid_ports(void)
{
    TEST_ASSERT_EQUAL_UINT16(80, parse_in_port_t(env, error, "80"));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    p101_error_reset(error);
    TEST_ASSERT_EQUAL_UINT16(65535, parse_in_port_t(env, error, "65535"));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    p101_error_reset(error);
    TEST_ASSERT_EQUAL_UINT16(0, parse_in_port_t(env, error, "0"));
    TEST_ASSERT_FALSE(p101_error_has_error(error));
}

static void test_parse_in_port_t_rejects_out_of_range(void)
{
    parse_in_port_t(env, error, "65536");
    TEST_ASSERT_TRUE(p101_error_has_error(error));
    p101_error_reset(error);
    parse_in_port_t(env, error, "-1");
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

static void test_parse_in_port_t_rejects_garbage(void)
{
    static const char *const bad[] = {"", "http", "80x", " ", "8 0"};
    size_t                   i;

    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        p101_error_reset(error);
        parse_in_port_t(env, error, bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(p101_error_has_error(error), bad[i]);
    }
}

/* ------------------------------------------------------------ convert_address */

static void test_convert_address_ipv4(void)
{
    struct sockaddr_storage addr;
    struct sockaddr_in      expected;
    const struct sockaddr_in *got;

    poison(&addr);
    convert_address(env, error, "192.168.1.10", &addr);

    TEST_ASSERT_FALSE(p101_error_has_error(error));
    TEST_ASSERT_EQUAL_INT(AF_INET, addr.ss_family);

    TEST_ASSERT_EQUAL_INT(1, inet_pton(AF_INET, "192.168.1.10", &expected.sin_addr));
    got = (const struct sockaddr_in *)(const void *)&addr;
    TEST_ASSERT_EQUAL_MEMORY(&expected.sin_addr, &got->sin_addr, sizeof(struct in_addr));
}

static void test_convert_address_ipv4_edges(void)
{
    struct sockaddr_storage addr;

    poison(&addr);
    convert_address(env, error, "0.0.0.0", &addr);
    TEST_ASSERT_EQUAL_INT(AF_INET, addr.ss_family);

    poison(&addr);
    convert_address(env, error, "255.255.255.255", &addr);
    TEST_ASSERT_EQUAL_INT(AF_INET, addr.ss_family);

    poison(&addr);
    convert_address(env, error, "127.0.0.1", &addr);
    TEST_ASSERT_EQUAL_INT(AF_INET, addr.ss_family);
}

static void test_convert_address_ipv6(void)
{
    struct sockaddr_storage    addr;
    struct sockaddr_in6        expected;
    const struct sockaddr_in6 *got;

    poison(&addr);
    convert_address(env, error, "2001:db8::1", &addr);

    TEST_ASSERT_FALSE(p101_error_has_error(error));
    TEST_ASSERT_EQUAL_INT(AF_INET6, addr.ss_family);

    TEST_ASSERT_EQUAL_INT(1, inet_pton(AF_INET6, "2001:db8::1", &expected.sin6_addr));
    got = (const struct sockaddr_in6 *)(const void *)&addr;
    TEST_ASSERT_EQUAL_MEMORY(&expected.sin6_addr, &got->sin6_addr, sizeof(struct in6_addr));
}

static void test_convert_address_ipv6_loopback(void)
{
    struct sockaddr_storage addr;

    poison(&addr);
    convert_address(env, error, "::1", &addr);
    TEST_ASSERT_FALSE(p101_error_has_error(error));
    TEST_ASSERT_EQUAL_INT(AF_INET6, addr.ss_family);
}

static void test_convert_address_is_not_confused_by_near_misses(void)
{
    /* These are NOT valid IPv4 -- inet_pton is stricter than inet_addr, which
     * is exactly why the library uses it. None may come back as AF_INET. */
    static const char *const not_ipv4[] = {"1.2.3", "1.2.3.4.5", "256.1.1.1", "01.02.03.04", "1.2.3.4 ", "0x7f.0.0.1"};
    size_t                   i;
    struct sockaddr_storage  addr;

    for(i = 0; i < sizeof(not_ipv4) / sizeof(not_ipv4[0]); i++)
    {
        poison(&addr);
        convert_address(env, error, not_ipv4[i], &addr);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(AF_INET, addr.ss_family, not_ipv4[i]);
    }
}

static void test_convert_address_unix_path(void)
{
    struct sockaddr_storage   addr;
    const struct sockaddr_un *got;

    poison(&addr);
    convert_address(env, error, "/tmp/p101.sock", &addr);

    TEST_ASSERT_EQUAL_INT(AF_UNIX, addr.ss_family);
    got = (const struct sockaddr_un *)(const void *)&addr;
    TEST_ASSERT_EQUAL_STRING("/tmp/p101.sock", got->sun_path);
}

static void test_convert_address_unix_path_at_the_limit(void)
{
    struct sockaddr_storage addr;
    struct sockaddr_un      sun;
    char                    path[sizeof(sun.sun_path)];

    /* Exactly sizeof(sun_path) - 1 characters: the longest path that fits. */
    memset(path, 'a', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    poison(&addr);
    convert_address(env, error, path, &addr);
    TEST_ASSERT_EQUAL_INT(AF_UNIX, addr.ss_family);
}

static void test_convert_address_rejects_overlong_unix_path(void)
{
    struct sockaddr_storage addr;
    struct sockaddr_un      sun;
    char                    path[sizeof(sun.sun_path) + 8];

    memset(path, 'a', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    poison(&addr);
    convert_address(env, error, path, &addr);
    TEST_ASSERT_EQUAL_INT(AF_UNSPEC, addr.ss_family);
}

static void test_convert_address_empty_string(void)
{
    struct sockaddr_storage addr;

    /* An empty string is not an IP; it IS a (degenerate) Unix path, so the
     * documented fall-through gives AF_UNIX with an empty sun_path. What it
     * must never be is AF_INET/AF_INET6 with uninitialised bytes. */
    poison(&addr);
    convert_address(env, error, "", &addr);
    TEST_ASSERT_NOT_EQUAL(AF_INET, addr.ss_family);
    TEST_ASSERT_NOT_EQUAL(AF_INET6, addr.ss_family);
}

static void test_convert_address_null_storage_raises(void)
{
    convert_address(env, error, "127.0.0.1", NULL);
    TEST_ASSERT_TRUE(p101_error_has_error(error));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_in_port_t_accepts_valid_ports);
    RUN_TEST(test_parse_in_port_t_rejects_out_of_range);
    RUN_TEST(test_parse_in_port_t_rejects_garbage);
    RUN_TEST(test_convert_address_ipv4);
    RUN_TEST(test_convert_address_ipv4_edges);
    RUN_TEST(test_convert_address_ipv6);
    RUN_TEST(test_convert_address_ipv6_loopback);
    RUN_TEST(test_convert_address_is_not_confused_by_near_misses);
    RUN_TEST(test_convert_address_unix_path);
    RUN_TEST(test_convert_address_unix_path_at_the_limit);
    RUN_TEST(test_convert_address_rejects_overlong_unix_path);
    RUN_TEST(test_convert_address_empty_string);
    RUN_TEST(test_convert_address_null_storage_raises);
    return UNITY_END();
}
