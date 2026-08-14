/*
 * libFuzzer harness for lib_convert's string parsers.
 *
 * The fuzzer supplies an arbitrary byte string; we NUL-terminate it and push it
 * through every public entry point. ASan and UBSan are on (see fuzz/CMakeLists),
 * so an out-of-bounds read or any undefined behaviour inside the parsers is a
 * crash the fuzzer will hand back as a reproducer.
 *
 * A crash is not the only thing worth catching, though. "Returned a wrong
 * number without complaining" is the failure mode that actually bites a caller,
 * and no sanitizer can see it -- there is nothing memory-unsafe about returning
 * 4294967295. So this harness also checks INVARIANTS: properties that must hold
 * for EVERY input, no matter what the fuzzer invents. Those are the checks that
 * would have caught p101_parse_unsigned_long_long("-1") == UINTMAX_MAX.
 *
 * Invariants asserted below:
 *   1. An unsigned parse that reports success must never have been handed a
 *      leading '-'  (no silent wraparound).
 *   2. A successful parse must land inside the target type's range.
 *   3. A successful signed parse must agree with strtoimax on the same string.
 *   4. p101_convert_address() must pick the SAME address family an independent
 *      inet_pton-based oracle picks -- never a leftover byte, and never a
 *      blanket AF_UNSPEC that would pass a mere "is it a legal family" test.
 *   5. If p101_convert_address() reports AF_INET/AF_INET6, the address it stored must
 *      round-trip back through inet_ntop/inet_pton to the same bytes.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <p101_convert/integer.h>
#include <p101_convert/networking.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Deliberately not assert(): this must fire even if NDEBUG is ever defined,
 * because a silenced invariant check is worse than no check at all. */
#define FUZZ_CHECK(cond, msg, input)                                                                                                                                                                                                                               \
    do                                                                                                                                                                                                                                                             \
    {                                                                                                                                                                                                                                                              \
        if(!(cond))                                                                                                                                                                                                                                                \
        {                                                                                                                                                                                                                                                          \
            fprintf(stderr, "INVARIANT VIOLATED: %s\n  input: \"%s\"\n", (msg), (input));                                                                                                                                                                          \
            abort();                                                                                                                                                                                                                                               \
        }                                                                                                                                                                                                                                                          \
    } while(0)

enum
{
    IPV4_DECIMAL_BASE = 10U,
    IPV4_OCTET_COUNT  = 4U,
    IPV4_DOT_COUNT    = IPV4_OCTET_COUNT - 1U,
    IPV4_MAX_DIGITS   = 3U,
    IPV4_MAX_OCTET    = 255U,
    ASCII_ZERO        = '0',
    ASCII_NINE        = '9',
    ASCII_DOT         = '.'
};

/* The first non-blank character the parsers will see. */
static int leading_sign_is_minus(const char *s)
{
    while(isspace((unsigned char)*s))
    {
        s++;
    }

    return *s == '-';
}

static int strict_ipv4_literal(const char *s)
{
    unsigned int octet;
    unsigned int octets;
    unsigned int digits;

    octet  = 0;
    octets = 0;
    digits = 0;

    if(s == NULL || *s == '\0')
    {
        return 0;
    }

    while(*s != '\0')
    {
        if(*s >= ASCII_ZERO && *s <= ASCII_NINE)
        {
            if(digits == 1U && octet == 0U)
            {
                return 0;
            }
            octet = (octet * IPV4_DECIMAL_BASE) + (unsigned int)(*s - ASCII_ZERO);
            digits++;
            if(digits > IPV4_MAX_DIGITS || octet > IPV4_MAX_OCTET)
            {
                return 0;
            }
        }
        else if(*s == ASCII_DOT)
        {
            if(digits == 0U || octets >= IPV4_DOT_COUNT)
            {
                return 0;
            }
            octets++;
            octet  = 0;
            digits = 0;
        }
        else
        {
            return 0;
        }
        s++;
    }

    return (octets == IPV4_DOT_COUNT && digits > 0U) ? 1 : 0;
}

static int dotted_numeric_text(const char *s)
{
    int saw_dot;

    saw_dot = 0;
    if(s == NULL || *s == '\0')
    {
        return 0;
    }

    while(*s != '\0')
    {
        if(*s == ASCII_DOT)
        {
            saw_dot = 1;
        }
        else if(*s < ASCII_ZERO || *s > ASCII_NINE)
        {
            return 0;
        }
        s++;
    }

    return saw_dot;
}

static void check_signed(const struct p101_env *env, struct p101_error *err, const char *s)
{
    intmax_t reference;
    intmax_t got;
    char    *endptr;
    int      ref_ok;

    /* Independent reference: plain strtoimax with the same "all of it, or
     * nothing" rule the library applies. */
    errno     = 0;
    reference = strtoimax(s, &endptr, 10);
    ref_ok    = (endptr != s && *endptr == '\0' && errno == 0);

    p101_error_reset(err);
    got = p101_parse_long_long(env, err, s, 0);

    if(!p101_error_has_error(err))
    {
        /* Invariant 3: a clean parse must match the reference exactly. */
        FUZZ_CHECK(ref_ok, "p101_parse_long_long accepted a string strtoimax rejects", s);
        FUZZ_CHECK(got == reference, "p101_parse_long_long disagrees with strtoimax", s);
    }

    /* Invariant 2, at a narrow width: a clean parse must be in range. */
    p101_error_reset(err);
    got = p101_parse_int16_t(env, err, s, 0);

    if(!p101_error_has_error(err))
    {
        FUZZ_CHECK(got >= INT16_MIN && got <= INT16_MAX, "p101_parse_int16_t returned an out-of-range value", s);
        FUZZ_CHECK(ref_ok && got == reference, "p101_parse_int16_t disagrees with strtoimax", s);
    }

    /* The sub-families: their whole job is the sign constraint. */
    p101_error_reset(err);
    got = p101_parse_negative_int(env, err, s, -1);

    if(!p101_error_has_error(err))
    {
        FUZZ_CHECK(got < 0, "p101_parse_negative_int accepted a non-negative value", s);
    }

    p101_error_reset(err);
    got = p101_parse_positive_int(env, err, s, 0);

    if(!p101_error_has_error(err))
    {
        FUZZ_CHECK(got > 0, "p101_parse_positive_int accepted a non-positive value", s);
    }
}

static void check_unsigned(const struct p101_env *env, struct p101_error *err, const char *s)
{
    unsigned long long wide;
    unsigned int       narrow;
    uint16_t           port;
    int                minus;

    minus = leading_sign_is_minus(s);

    /* Invariant 1, at the width where nothing else can catch it: if the string
     * starts with '-', strtoumax would wrap it around to a huge positive value,
     * and the range check cannot notice because the wrapped value IS in range. */
    p101_error_reset(err);
    wide = p101_parse_unsigned_long_long(env, err, s, 0);

    if(!p101_error_has_error(err))
    {
        FUZZ_CHECK(!minus, "p101_parse_unsigned_long_long accepted a negative string", s);
        (void)wide;
    }

    p101_error_reset(err);
    narrow = p101_parse_unsigned_int(env, err, s, 0);

    if(!p101_error_has_error(err))
    {
        FUZZ_CHECK(!minus, "p101_parse_unsigned_int accepted a negative string", s);
        FUZZ_CHECK(narrow <= UINT_MAX, "p101_parse_unsigned_int returned an out-of-range value", s);
    }

    p101_error_reset(err);
    port = p101_parse_in_port_t(env, err, s);

    if(!p101_error_has_error(err))
    {
        FUZZ_CHECK(!minus, "p101_parse_in_port_t accepted a negative string", s);
        FUZZ_CHECK(port <= UINT16_MAX, "p101_parse_in_port_t returned an out-of-range port", s);
    }
}

static void check_address(const struct p101_env *env, struct p101_error *err, const char *s)
{
    struct sockaddr_storage addr;
    struct sockaddr_un      sun;
    char                    text[INET6_ADDRSTRLEN];
    struct in_addr          v4;
    struct in6_addr         v6;
    sa_family_t             expected;
    socklen_t               got_length;
    int                     is_v4;
    int                     is_v6;

    /* An INDEPENDENT answer, worked out from the documented rules using the
     * platform's own inet_pton rather than anything in lib_convert. Comparing
     * against this is what makes the check a real oracle: a membership test
     * ("is the family one of these four?") is satisfied by a p101_convert_address
     * that just returns AF_UNSPEC for everything, which is exactly the failure
     * this is here to catch. */
    is_v4 = (inet_pton(AF_INET, s, &v4) == 1);
    is_v6 = (inet_pton(AF_INET6, s, &v6) == 1);

    if(dotted_numeric_text(s) && !strict_ipv4_literal(s))
    {
        expected = AF_UNSPEC;
    }
    else if(is_v4)
    {
        expected = AF_INET;
    }
    else if(is_v6)
    {
        expected = AF_INET6;
    }
    else if(s[0] != '\0' && strchr(s, '/') != NULL && strlen(s) <= sizeof(sun.sun_path) - 1)
    {
        expected = AF_UNIX;
    }
    else
    {
        expected = AF_UNSPEC;
    }

    /* 0xA5 rather than 0: if p101_convert_address() forgets to write a field, the
     * leftover is loud instead of an accidentally-plausible zero. */
    memset(&addr, 0xA5, sizeof(addr));
    p101_error_reset(err);
    got_length = p101_convert_address(env, err, s, &addr);

    /* Invariant 4: the family is what the caller branches on next (socket(),
     * bind()), so it must be the RIGHT one -- not merely a legal-looking one,
     * and never a leftover byte. */
    FUZZ_CHECK(addr.ss_family == expected, "p101_convert_address chose the wrong address family", s);
    FUZZ_CHECK((expected == AF_UNSPEC) == p101_error_has_error(err), "p101_convert_address error state disagrees with its result", s);

    /* Invariant 5: whatever it claims to have parsed must round-trip. */
    if(addr.ss_family == AF_INET)
    {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)&addr;

        FUZZ_CHECK(inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) != NULL, "p101_convert_address stored an unprintable IPv4 address", s);
        FUZZ_CHECK(memcmp(&v4, &sin->sin_addr, sizeof(v4)) == 0, "p101_convert_address stored the wrong IPv4 bytes", s);
        FUZZ_CHECK(got_length == sizeof(*sin), "p101_convert_address returned the wrong IPv4 length", s);
    }
    else if(addr.ss_family == AF_INET6)
    {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)(const void *)&addr;

        FUZZ_CHECK(inet_ntop(AF_INET6, &sin6->sin6_addr, text, sizeof(text)) != NULL, "p101_convert_address stored an unprintable IPv6 address", s);
        FUZZ_CHECK(memcmp(&v6, &sin6->sin6_addr, sizeof(v6)) == 0, "p101_convert_address stored the wrong IPv6 bytes", s);
        FUZZ_CHECK(got_length == sizeof(*sin6), "p101_convert_address returned the wrong IPv6 length", s);
    }
    else if(addr.ss_family == AF_UNIX)
    {
        const struct sockaddr_un *stored = (const struct sockaddr_un *)(const void *)&addr;

        /* The path must be NUL-terminated inside the field -- otherwise every
         * later strlen()/connect() on it reads past the end. */
        FUZZ_CHECK(memchr(stored->sun_path, '\0', sizeof(stored->sun_path)) != NULL, "p101_convert_address left sun_path unterminated", s);
        FUZZ_CHECK(strcmp(stored->sun_path, s) == 0, "p101_convert_address stored the wrong sun_path", s);
        FUZZ_CHECK(got_length == offsetof(struct sockaddr_un, sun_path) + strlen(s) + 1U, "p101_convert_address returned the wrong Unix address length", s);
    }
    else
    {
        FUZZ_CHECK(got_length == 0U, "p101_convert_address returned a length for an invalid address", s);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char              *buf;
    struct p101_error *err;
    struct p101_env   *env;

    /* The parsers take a C string, so the input has to be NUL-terminated. A
     * heap buffer sized exactly to the input (rather than a fixed stack array)
     * is deliberate: ASan puts a redzone right after it, so a one-past-the-end
     * read inside the parsers is caught instead of landing in slack space. */
    buf = (char *)malloc(size + 1);

    if(buf == NULL)
    {
        return 0;
    }

    memcpy(buf, data, size);
    buf[size] = '\0';

    err = p101_error_create(false);
    env = p101_env_create(err, NULL);

    check_signed(env, err, buf);
    check_unsigned(env, err, buf);
    check_address(env, err, buf);

    p101_env_destroy(env);
    p101_error_destroy(err);
    free(buf);

    return 0;
}
