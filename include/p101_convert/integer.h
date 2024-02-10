#ifndef LIBP101_CONVERT_P101_INTEGER_H
#define LIBP101_CONVERT_P101_INTEGER_H

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

#include <p101_env/env.h>
#include <p101_error/error.h>
#include <stdint.h>

char               p101_parse_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value);
short              p101_parse_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value);
int                p101_parse_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value);
long               p101_parse_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value);
long long          p101_parse_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value);
unsigned char      p101_parse_unsigned_char(const struct p101_env *env, struct p101_error *err, const char *str, unsigned char default_value);
unsigned short     p101_parse_unsigned_short(const struct p101_env *env, struct p101_error *err, const char *str, unsigned short default_value);
unsigned int       p101_parse_unsigned_int(const struct p101_env *env, struct p101_error *err, const char *str, unsigned int default_value);
unsigned long      p101_parse_unsigned_long(const struct p101_env *env, struct p101_error *err, const char *str, unsigned long default_value);
unsigned long long p101_parse_unsigned_long_long(const struct p101_env *env, struct p101_error *err, const char *str, unsigned long long default_value);
char               p101_parse_negative_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value);
short              p101_parse_negative_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value);
int                p101_parse_negative_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value);
long               p101_parse_negative_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value);
long long          p101_parse_negative_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value);
char               p101_parse_positive_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value);
short              p101_parse_positive_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value);
int                p101_parse_positive_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value);
long               p101_parse_positive_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value);
long long          p101_parse_positive_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value);
int8_t             p101_parse_int8_t(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value);
int16_t            p101_parse_int16_t(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value);
int32_t            p101_parse_int32_t(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value);
uint8_t            p101_parse_uint8_t(const struct p101_env *env, struct p101_error *err, const char *str, uint8_t default_value);
uint16_t           p101_parse_uint16_t(const struct p101_env *env, struct p101_error *err, const char *str, uint16_t default_value);
uint32_t           p101_parse_uint32_t(const struct p101_env *env, struct p101_error *err, const char *str, uint32_t default_value);
int8_t             p101_parse_negative_int8_t_char(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value);
int16_t            p101_parse_negative_int16_t_short(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value);
int32_t            p101_parse_negative_int32_t_int(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value);
int8_t             p101_parse_positive_int8_t_char(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value);
int16_t            p101_parse_positive_int16_t_short(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value);
int32_t            p101_parse_positive_int32_t_int(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value);

#endif
