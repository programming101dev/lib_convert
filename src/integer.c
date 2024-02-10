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

#include "p101_convert/integer.h"
#include <limits.h>
#include <p101_c/p101_inttypes.h>

#define BASE_TEN 10

static intmax_t  parse_integer(const struct p101_env *env, struct p101_error *err, const char *str, intmax_t default_value, intmax_t min_value, intmax_t max_value);
static uintmax_t parse_unsigned_integer(const struct p101_env *env, struct p101_error *err, const char *str, uintmax_t default_value, uintmax_t max_value);

static intmax_t parse_integer(const struct p101_env *env, struct p101_error *err, const char *str, intmax_t default_value, intmax_t min_value, intmax_t max_value)
{
    char    *endptr;
    intmax_t parsed_value;
    intmax_t value;

    P101_TRACE(env);
    parsed_value = p101_strtoimax(env, err, str, &endptr, BASE_TEN);

    if(p101_error_has_error(err) || *endptr != '\0' || endptr == str)
    {
        if(!p101_error_has_error(err) && *endptr != '\0')
        {
            // There are characters after the number
            P101_ERROR_RAISE_SYSTEM(err, "Unexpected characters after the number", 1);
        }
        else if(!p101_error_has_error(err) && endptr == str)
        {
            // No digits were found
            P101_ERROR_RAISE_SYSTEM(err, "No digits were found", 1);
        }
        else if(!p101_error_has_error(err))
        {
            // Other parsing errors
            P101_ERROR_RAISE_SYSTEM(err, "Parsing error", 1);
        }

        value = default_value;
    }
    else if(parsed_value < min_value)
    {
        // Parsed value is below min_value
        P101_ERROR_RAISE_SYSTEM(err, "Parsed value is below minimum", 1);
        value = min_value;
    }
    else if(parsed_value > max_value)
    {
        // Parsed value is above max_value
        P101_ERROR_RAISE_SYSTEM(err, "Parsed value is above maximum", 1);
        value = max_value;
    }
    else
    {
        // Parsed value is within range
        value = parsed_value;
    }

    return value;
}

static uintmax_t parse_unsigned_integer(const struct p101_env *env, struct p101_error *err, const char *str, uintmax_t default_value, uintmax_t max_value)
{
    char     *endptr;
    uintmax_t parsed_value;
    uintmax_t value;

    P101_TRACE(env);
    parsed_value = p101_strtoumax(env, err, str, &endptr, BASE_TEN);

    if(p101_error_has_error(err) || *endptr != '\0' || endptr == str)
    {
        if(!p101_error_has_error(err) && *endptr != '\0')
        {
            // There are characters after the number
            P101_ERROR_RAISE_SYSTEM(err, "Unexpected characters after the number", 1);
        }
        else if(!p101_error_has_error(err) && endptr == str)
        {
            // No digits were found
            P101_ERROR_RAISE_SYSTEM(err, "No digits were found", 1);
        }
        else if(!p101_error_has_error(err))
        {
            // Other parsing errors
            P101_ERROR_RAISE_SYSTEM(err, "Parsing error", 1);
        }

        value = default_value;
    }
    else if(parsed_value > max_value)
    {
        // Parsed value is above max_value
        P101_ERROR_RAISE_SYSTEM(err, "Parsed value is above maximum", 1);
        value = max_value;
    }
    else
    {
        // Parsed value is within range
        value = parsed_value;
    }

    return value;
}

char p101_parse_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value)
{
    P101_TRACE(env);

    return (char)parse_integer(env, err, str, default_value, CHAR_MIN, CHAR_MAX);
}

short p101_parse_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value)
{
    P101_TRACE(env);

    return (short)parse_integer(env, err, str, default_value, SHRT_MIN, SHRT_MAX);
}

int p101_parse_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value)
{
    P101_TRACE(env);

    return (int)parse_integer(env, err, str, default_value, INT_MIN, INT_MAX);
}

long p101_parse_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value)
{
    P101_TRACE(env);

    return (long)parse_integer(env, err, str, default_value, LONG_MIN, LONG_MAX);
}

long long p101_parse_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value)
{
    P101_TRACE(env);

    return (long long)parse_integer(env, err, str, default_value, LLONG_MIN, LLONG_MAX);
}

unsigned char p101_parse_unsigned_char(const struct p101_env *env, struct p101_error *err, const char *str, unsigned char default_value)
{
    P101_TRACE(env);

    return (unsigned char)parse_unsigned_integer(env, err, str, default_value, UCHAR_MAX);
}

unsigned short p101_parse_unsigned_short(const struct p101_env *env, struct p101_error *err, const char *str, unsigned short default_value)
{
    P101_TRACE(env);

    return (unsigned short)parse_unsigned_integer(env, err, str, default_value, USHRT_MAX);
}

unsigned int p101_parse_unsigned_int(const struct p101_env *env, struct p101_error *err, const char *str, unsigned int default_value)
{
    P101_TRACE(env);

    return (unsigned int)parse_unsigned_integer(env, err, str, default_value, UINT_MAX);
}

unsigned long p101_parse_unsigned_long(const struct p101_env *env, struct p101_error *err, const char *str, unsigned long default_value)
{
    P101_TRACE(env);

    return (unsigned long)parse_unsigned_integer(env, err, str, default_value, ULONG_MAX);
}

unsigned long long p101_parse_unsigned_long_long(const struct p101_env *env, struct p101_error *err, const char *str, unsigned long long default_value)
{
    P101_TRACE(env);

    return (unsigned long long)parse_unsigned_integer(env, err, str, default_value, ULLONG_MAX);
}

char p101_parse_negative_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value)
{
    P101_TRACE(env);

    return (char)parse_integer(env, err, str, default_value, CHAR_MIN, -1);
}

short p101_parse_negative_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value)
{
    P101_TRACE(env);

    return (short)parse_integer(env, err, str, default_value, SHRT_MIN, -1);
}

int p101_parse_negative_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value)
{
    P101_TRACE(env);

    return (int)parse_integer(env, err, str, default_value, INT_MIN, -1);
}

long p101_parse_negative_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value)
{
    P101_TRACE(env);

    return (long)parse_integer(env, err, str, default_value, LONG_MIN, -1L);
}

long long p101_parse_negative_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value)
{
    P101_TRACE(env);

    return (long long)parse_integer(env, err, str, default_value, LLONG_MIN, -1LL);
}

char p101_parse_positive_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value)
{
    P101_TRACE(env);

    return (char)parse_integer(env, err, str, default_value, 0, CHAR_MAX);
}

short p101_parse_positive_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value)
{
    P101_TRACE(env);

    return (short)parse_integer(env, err, str, default_value, 0, SHRT_MAX);
}

int p101_parse_positive_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value)
{
    P101_TRACE(env);

    return (int)parse_integer(env, err, str, default_value, 0, INT_MAX);
}

long p101_parse_positive_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value)
{
    P101_TRACE(env);

    return (long)parse_integer(env, err, str, default_value, 0, LONG_MAX);
}

long long p101_parse_positive_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value)
{
    P101_TRACE(env);

    return (long long)parse_integer(env, err, str, default_value, 0, LLONG_MAX);
}

int8_t p101_parse_int8_t(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value)
{
    P101_TRACE(env);

    return (int8_t)parse_integer(env, err, str, default_value, INT8_MIN, INT8_MAX);
}

int16_t p101_parse_int16_t(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value)
{
    P101_TRACE(env);

    return (int16_t)parse_integer(env, err, str, default_value, INT16_MIN, INT16_MAX);
}

int32_t p101_parse_int32_t(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value)
{
    P101_TRACE(env);

    return (int32_t)parse_integer(env, err, str, default_value, INT32_MIN, INT32_MAX);
}

uint8_t p101_parse_uint8_t(const struct p101_env *env, struct p101_error *err, const char *str, uint8_t default_value)
{
    P101_TRACE(env);

    return (uint8_t)parse_unsigned_integer(env, err, str, default_value, UINT8_MAX);
}

uint16_t p101_parse_uint16_t(const struct p101_env *env, struct p101_error *err, const char *str, uint16_t default_value)
{
    P101_TRACE(env);

    return (uint16_t)parse_unsigned_integer(env, err, str, default_value, UINT16_MAX);
}

uint32_t p101_parse_uint32_t(const struct p101_env *env, struct p101_error *err, const char *str, uint32_t default_value)
{
    P101_TRACE(env);

    return (uint32_t)parse_unsigned_integer(env, err, str, default_value, UINT32_MAX);
}

int8_t p101_parse_negative_int8_t_char(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value)
{
    P101_TRACE(env);

    return (int8_t)parse_integer(env, err, str, default_value, INT8_MIN, -1);
}

int16_t p101_parse_negative_int16_t_short(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value)
{
    P101_TRACE(env);

    return (int16_t)parse_integer(env, err, str, default_value, INT16_MIN, -1);
}

int32_t p101_parse_negative_int32_t_int(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value)
{
    P101_TRACE(env);

    return (int32_t)parse_integer(env, err, str, default_value, INT32_MIN, -1);
}

int8_t p101_parse_positive_int8_t_char(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value)
{
    P101_TRACE(env);

    return (int8_t)parse_integer(env, err, str, default_value, 0, INT8_MAX);
}

int16_t p101_parse_positive_int16_t_short(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value)
{
    P101_TRACE(env);

    return (int16_t)parse_integer(env, err, str, default_value, 0, INT16_MAX);
}

int32_t p101_parse_positive_int32_t_int(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value)
{
    P101_TRACE(env);

    return (int32_t)parse_integer(env, err, str, default_value, 0, INT32_MAX);
}
