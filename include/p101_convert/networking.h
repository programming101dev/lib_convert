#ifndef LIBP101_CONVERT_P101_NETWORKING_H
#define LIBP101_CONVERT_P101_NETWORKING_H

/*
 * Copyright 2022-2024 D'Arcy Smith.
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

#include <arpa/inet.h>
#include <p101_env/env.h>
#include <p101_error/error.h>
#include <sys/socket.h>

in_port_t parse_in_port_t(const struct p101_env *env, struct p101_error *err, const char *str);
void      convert_address(const struct p101_env *env, struct p101_error *err, const char *address, struct sockaddr_storage *addr);

#endif
