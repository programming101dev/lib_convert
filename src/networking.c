/*
 * Copyright 2024-2024 D'Arcy Smith.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "p101_convert/networking.h"
#include "p101_convert/errors.h"
#include "p101_convert/integer.h"
#include <errno.h>
#include <netinet/in.h>
#include <p101_c/p101_string.h>
#include <p101_env/wrapper.h>
#include <p101_network/network.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

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

static bool is_strict_ipv4_literal(const struct p101_env *env, const char *address);
static bool is_dotted_numeric_text(const struct p101_env *env, const char *address);
static bool is_unix_path(const struct p101_env *env, const char *address);

static bool is_strict_ipv4_literal(const struct p101_env *env, const char *address)
{
    unsigned int octet;
    unsigned int octets;
    unsigned int digits;
    bool         valid;

    P101_TRACE(env);
    octet  = 0;
    octets = 0;
    digits = 0;
    valid  = false;

    if(address == NULL || *address == '\0')
    {
        goto done;
    }

    while(*address != '\0')
    {
        if(*address >= ASCII_ZERO && *address <= ASCII_NINE)
        {
            if(digits == 1U && octet == 0U)
            {
                goto done;
            }
            octet = (octet * IPV4_DECIMAL_BASE) + (unsigned int)(*address - ASCII_ZERO);
            digits++;
            if(digits > IPV4_MAX_DIGITS || octet > IPV4_MAX_OCTET)
            {
                goto done;
            }
        }
        else if(*address == ASCII_DOT)
        {
            if(digits == 0U || octets >= IPV4_DOT_COUNT)
            {
                goto done;
            }
            octets++;
            octet  = 0;
            digits = 0;
        }
        else
        {
            goto done;
        }

        address++;
    }

    if(octets == IPV4_DOT_COUNT && digits > 0U)
    {
        valid = true;
    }

done:
    P101_TRACE_EXIT(env);
    return valid;
}

static bool is_dotted_numeric_text(const struct p101_env *env, const char *address)
{
    bool saw_dot;

    P101_TRACE(env);
    saw_dot = false;
    if(address == NULL || *address == '\0')
    {
        goto done;
    }

    while(*address != '\0')
    {
        if(*address == ASCII_DOT)
        {
            saw_dot = true;
        }
        else if(*address < ASCII_ZERO || *address > ASCII_NINE)
        {
            saw_dot = false;
            goto done;
        }
        address++;
    }

done:
    P101_TRACE_EXIT(env);
    return saw_dot;
}

static bool is_unix_path(const struct p101_env *env, const char *address)
{
    bool ret_val;

    P101_TRACE(env);
    ret_val = false;
    if(address != NULL && address[0] != '\0' && p101_strchr(env, address, '/') != NULL)
    {
        ret_val = true;
    }
    P101_TRACE_EXIT(env);
    return ret_val;
}

in_port_t p101_parse_in_port_t(const struct p101_env *env, struct p101_error *err, const char *str)
{
    in_port_t ret_val;

    P101_TRACE(env);
    P101_WRAPPER_FAULT_RETURN(env, err, ret_val, 0);
    ret_val = p101_parse_uint16_t(env, err, str, 0);
    P101_WRAPPER_DONE(env);
    return ret_val;
}

socklen_t p101_convert_address(const struct p101_env *env, struct p101_error *err, const char *address, struct sockaddr_storage *addr)
{
    struct sockaddr_un  sun;
    struct sockaddr_in  sin;
    struct sockaddr_in6 sin6;
    size_t              path_length;
    socklen_t           ret_val;
    int                 parse_result;

    P101_TRACE(env);
    P101_WRAPPER_FAULT_RETURN(env, err, ret_val, 0);
    ret_val = 0;

    if(addr == NULL)
    {
        P101_ERROR_RAISE_CHECK(err);
        goto done;
    }

    p101_memset(env, addr, 0, sizeof(*addr));
    addr->ss_family = AF_UNSPEC;
    if(address == NULL)
    {
        P101_ERROR_RAISE_CHECK(err);
        goto done;
    }
    if(p101_error_has_error(err))
    {
        goto done;
    }

    p101_memset(env, &sin, 0, sizeof(sin));
    if(is_strict_ipv4_literal(env, address))
    {
        parse_result = p101_inet_pton(env, err, AF_INET, address, &sin.sin_addr);
        if(parse_result == 1)
        {
            sin.sin_family = AF_INET;
#if defined(__APPLE__) || defined(__FreeBSD__)
            sin.sin_len = (uint8_t)sizeof(sin);
#endif
            p101_memcpy(env, addr, &sin, sizeof(sin));
            ret_val = (socklen_t)sizeof(sin);
            goto done;
        }
        if(p101_error_has_error(err))
        {
            if(p101_error_is_errno(err, EINVAL))
            {
                p101_error_reset(err);
            }
            else
            {
                goto done;
            }
        }
    }

    p101_memset(env, &sin6, 0, sizeof(sin6));
    parse_result = p101_inet_pton(env, err, AF_INET6, address, &sin6.sin6_addr);
    if(parse_result == 1)
    {
        sin6.sin6_family = AF_INET6;
#if defined(__APPLE__) || defined(__FreeBSD__)
        sin6.sin6_len = (uint8_t)sizeof(sin6);
#endif
        p101_memcpy(env, addr, &sin6, sizeof(sin6));
        ret_val = (socklen_t)sizeof(sin6);
        goto done;
    }
    if(p101_error_has_error(err))
    {
        if(p101_error_is_errno(err, EINVAL))
        {
            p101_error_reset(err);
        }
        else
        {
            goto done;
        }
    }

    if(!is_dotted_numeric_text(env, address) && is_unix_path(env, address))
    {
        path_length = p101_strlen(env, address);
        if(path_length <= sizeof(sun.sun_path) - 1U)
        {
            p101_memset(env, &sun, 0, sizeof(sun));
            p101_strncpy(env, sun.sun_path, address, sizeof(sun.sun_path) - 1U);
            sun.sun_family = AF_UNIX;
            ret_val        = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1U);
#if defined(__APPLE__) || defined(__FreeBSD__)
            sun.sun_len = (uint8_t)ret_val;
#endif
            p101_memcpy(env, addr, &sun, sizeof(sun));
            goto done;
        }
    }

    P101_ERROR_RAISE_USER(err, "The address is not an IPv4/IPv6 literal or an explicit Unix pathname.", P101_CONVERT_ERROR_ADDRESS);

done:
    P101_WRAPPER_DONE(env);
    return ret_val;
}
