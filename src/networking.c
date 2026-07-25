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
#include "p101_convert/integer.h"
#include <netinet/in.h>
#include <p101_c/p101_string.h>
#include <p101_posix/arpa/p101_inet.h>
#include <sys/socket.h>
#include <sys/un.h>

in_port_t parse_in_port_t(const struct p101_env *env, struct p101_error *err, const char *str)
{
    P101_TRACE(env);

    return p101_parse_uint16_t(env, err, str, 0);
}

void convert_address(const struct p101_env *env, struct p101_error *err, const char *address, struct sockaddr_storage *addr)
{
    struct sockaddr_un  sun;
    struct sockaddr_in  sin;
    struct sockaddr_in6 sin6;

    P101_TRACE(env);

    if(addr == NULL || address == NULL)
    {
        P101_ERROR_RAISE_CHECK(err);
        goto done;
    }

    // Start from a known state. Whatever family we settle on, the bytes it does
    // not use must be zero rather than whatever the caller left on the stack.
    p101_memset(env, addr, 0, sizeof(*addr));

    // inet_pton() returns 1 on success, 0 when the string is not an address of
    // that family, and -1 on error -- so success is "== 1", not "== 0". On a
    // non-match it leaves the destination untouched, which is why each local
    // sockaddr is zeroed before it is used.
    p101_memset(env, &sin, 0, sizeof(sin));

    if(p101_inet_pton(env, err, AF_INET, address, &sin.sin_addr) == 1)
    {
        sin.sin_family = AF_INET;
        p101_memcpy(env, addr, &sin, sizeof(struct sockaddr_in));
        addr->ss_family = AF_INET;
        goto done;
    }

    p101_error_reset(err);
    p101_memset(env, &sin6, 0, sizeof(sin6));

    if(p101_inet_pton(env, err, AF_INET6, address, &sin6.sin6_addr) == 1)
    {
        sin6.sin6_family = AF_INET6;
        p101_memcpy(env, addr, &sin6, sizeof(struct sockaddr_in6));
        addr->ss_family = AF_INET6;
        goto done;
    }

    p101_error_reset(err);

    // Not an IPv4 or IPv6 literal: a string short enough to fit in sun_path is
    // taken to be a Unix domain socket path.
    if(p101_strlen(env, address) <= sizeof(sun.sun_path) - 1)
    {
        p101_memset(env, &sun, 0, sizeof(sun));
        p101_strncpy(env, sun.sun_path, address, sizeof(sun.sun_path) - 1);
        sun.sun_family = AF_UNIX;
        p101_memcpy(env, addr, &sun, sizeof(struct sockaddr_un));
        addr->ss_family = AF_UNIX;
        goto done;
    }

    // Not an address of any family we understand, and too long to be a path.
    addr->ss_family = AF_UNSPEC;

done:
    return;
}
