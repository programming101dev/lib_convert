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

    if(addr == NULL)
    {
        P101_ERROR_RAISE_CHECK(err);
        goto done;
    }

    // Try to parse the address as IPv4
    if(p101_inet_pton(env, err, AF_INET, address, &sin.sin_addr) == 0)
    {
        addr->ss_family = AF_INET;
        p101_memcpy(env, addr, &sin, sizeof(struct sockaddr_in));
        goto done;
    }

    p101_error_reset(err);

    // Try to parse the address as IPv6
    if(p101_inet_pton(env, err, AF_INET6, address, &sin6.sin6_addr) == 0)
    {
        addr->ss_family = AF_INET6;
        p101_memcpy(env, addr, &sin6, sizeof(struct sockaddr_in6));
        goto done;
    }

    p101_error_reset(err);

    // If parsing as IPv4 or IPv6 fails, check if the address is a valid Unix domain socket
    if(strlen(address) <= sizeof(sun.sun_path) - 1)
    {
        p101_memset(env, &sun, 0, sizeof(sun));
        p101_strncpy(env, sun.sun_path, address, sizeof(sun.sun_path) - 1);
        sun.sun_family = AF_UNIX;
        p101_memcpy(env, addr, &sun, sizeof(struct sockaddr_un));
        addr->ss_family = AF_UNIX;
        goto done;
    }

    // If none of the above conditions are met, set the address family to AF_UNSPEC
    addr->ss_family = AF_UNSPEC;

done:
    return;
}
