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

#include "p101_convert/errors.h"
#include <errno.h>
#include <limits.h>
#include <p101_c/p101_ctype.h>
#include <p101_c/p101_inttypes.h>
#include <p101_convert/integer.h>
#include <p101_env/wrapper.h>

static intmax_t  parse_integer(const struct p101_env *env, struct p101_error *err, const char *str, intmax_t default_value, intmax_t min_value, intmax_t max_value);
static uintmax_t parse_unsigned_integer(const struct p101_env *env, struct p101_error *err, const char *str, uintmax_t default_value, uintmax_t max_value);

#define BASE_TEN 10    // NOLINT(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define P101_PARSE_PROLOGUE_ARG3(env_arg, return_type, default_arg)                                                                                                                                                                                                \
    return_type parsed_result;                                                                                                                                                                                                                                     \
    P101_TRACE(env_arg);                                                                                                                                                                                                                                           \
    P101_WRAPPER_FAULT_RETURN((env_arg), err, parsed_result, (default_arg))

#define P101_PARSE_EPILOGUE(env_arg)                                                                                                                                                                                                                               \
    P101_WRAPPER_DONE(env_arg);                                                                                                                                                                                                                                    \
    return parsed_result

static intmax_t parse_integer(const struct p101_env *env, struct p101_error *err, const char *str, intmax_t default_value, intmax_t min_value, intmax_t max_value)
{
    char    *endptr;
    bool     has_error;
    bool     is_range_error;
    intmax_t parsed_value;
    intmax_t ret_val;

    P101_TRACE(env);
    ret_val = default_value;
    if(str == NULL)
    {
        P101_ERROR_RAISE_CHECK(err);
        goto done;
    }
    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto done;
    }

    parsed_value = p101_strtoimax(env, err, str, &endptr, BASE_TEN);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        is_range_error = p101_error_is_errno(err, ERANGE);
        if(is_range_error)
        {
            p101_error_reset(err);
            P101_ERROR_RAISE_USER(err, "The integer is outside the supported range.", P101_CONVERT_ERROR_RANGE);
        }
        goto done;
    }

    if(endptr == str)
    {
        P101_ERROR_RAISE_USER(err, "The string does not contain an integer.", P101_CONVERT_ERROR_SYNTAX);
        goto done;
    }
    if(*endptr != '\0')
    {
        P101_ERROR_RAISE_USER(err, "Unexpected characters follow the integer.", P101_CONVERT_ERROR_SYNTAX);
        goto done;
    }
    if(parsed_value < min_value || parsed_value > max_value)
    {
        P101_ERROR_RAISE_USER(err, "The integer is outside the target type's range.", P101_CONVERT_ERROR_RANGE);
        goto done;
    }

    ret_val = parsed_value;

done:
    P101_TRACE_EXIT(env);
    return ret_val;
}

static uintmax_t parse_unsigned_integer(const struct p101_env *env, struct p101_error *err, const char *str, uintmax_t default_value, uintmax_t max_value)
{
    char       *endptr;
    const char *cursor;
    bool        has_error;
    bool        is_range_error;
    int         is_space;
    uintmax_t   parsed_value;
    uintmax_t   ret_val;

    P101_TRACE(env);
    ret_val = default_value;
    if(str == NULL)
    {
        P101_ERROR_RAISE_CHECK(err);
        goto done;
    }
    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto done;
    }

    // strtoumax() accepts a leading '-' and returns the value WRAPPED around
    // (so "-1" comes back as UINTMAX_MAX) without setting errno. The range
    // check further down cannot catch that for the widest types, because the
    // wrapped value is already inside the type's range -- "-1" would silently
    // become 18446744073709551615. Refuse the sign here instead. Whitespace is
    // skipped the same way strtoumax() itself skips it, so " -1" is caught too.
    cursor = str;

    is_space = p101_isspace(env, (unsigned char)*cursor);
    while(is_space != 0)
    {
        cursor++;
        is_space = p101_isspace(env, (unsigned char)*cursor);
    }

    if(*cursor == '-')
    {
        P101_ERROR_RAISE_USER(err, "A negative integer cannot be converted to an unsigned type.", P101_CONVERT_ERROR_RANGE);
        goto done;
    }

    parsed_value = p101_strtoumax(env, err, str, &endptr, BASE_TEN);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        is_range_error = p101_error_is_errno(err, ERANGE);
        if(is_range_error)
        {
            p101_error_reset(err);
            P101_ERROR_RAISE_USER(err, "The integer is outside the supported range.", P101_CONVERT_ERROR_RANGE);
        }
        goto done;
    }

    if(endptr == str)
    {
        P101_ERROR_RAISE_USER(err, "The string does not contain an integer.", P101_CONVERT_ERROR_SYNTAX);
        goto done;
    }
    if(*endptr != '\0')
    {
        P101_ERROR_RAISE_USER(err, "Unexpected characters follow the integer.", P101_CONVERT_ERROR_SYNTAX);
        goto done;
    }
    if(parsed_value > max_value)
    {
        P101_ERROR_RAISE_USER(err, "The integer is outside the target type's range.", P101_CONVERT_ERROR_RANGE);
        goto done;
    }

    ret_val = parsed_value;

done:
    P101_TRACE_EXIT(env);
    return ret_val;
}

#define DEFINE_SIGNED_PARSE_CONVERTER(function_name, result_type)                                                                                                                                                                                                  \
    static result_type function_name(const struct p101_env *env, struct p101_error *err, const char *str, result_type default_value, intmax_t min_value, intmax_t max_value)                                                                                       \
    {                                                                                                                                                                                                                                                              \
        intmax_t    parsed_wide_result;                                                                                                                                                                                                                            \
        result_type parsed_result;                                                                                                                                                                                                                                 \
        parsed_wide_result = parse_integer(env, err, str, default_value, min_value, max_value);                                                                                                                                                                    \
        parsed_result      = (result_type)parsed_wide_result;                                                                                                                                                                                                      \
        return parsed_result;                                                                                                                                                                                                                                      \
    }

#define DEFINE_SIGNED_PARSE_SAME_CONVERTER(function_name, result_type)                                                                                                                                                                                             \
    static result_type function_name(const struct p101_env *env, struct p101_error *err, const char *str, result_type default_value, intmax_t min_value, intmax_t max_value)                                                                                       \
    {                                                                                                                                                                                                                                                              \
        result_type parsed_result;                                                                                                                                                                                                                                 \
        parsed_result = parse_integer(env, err, str, default_value, min_value, max_value);                                                                                                                                                                         \
        return parsed_result;                                                                                                                                                                                                                                      \
    }

#define DEFINE_UNSIGNED_PARSE_CONVERTER(function_name, result_type)                                                                                                                                                                                                \
    static result_type function_name(const struct p101_env *env, struct p101_error *err, const char *str, result_type default_value, uintmax_t max_value)                                                                                                          \
    {                                                                                                                                                                                                                                                              \
        uintmax_t   parsed_wide_result;                                                                                                                                                                                                                            \
        result_type parsed_result;                                                                                                                                                                                                                                 \
        parsed_wide_result = parse_unsigned_integer(env, err, str, default_value, max_value);                                                                                                                                                                      \
        parsed_result      = (result_type)parsed_wide_result;                                                                                                                                                                                                      \
        return parsed_result;                                                                                                                                                                                                                                      \
    }

#define DEFINE_UNSIGNED_PARSE_SAME_CONVERTER(function_name, result_type)                                                                                                                                                                                           \
    static result_type function_name(const struct p101_env *env, struct p101_error *err, const char *str, result_type default_value, uintmax_t max_value)                                                                                                          \
    {                                                                                                                                                                                                                                                              \
        result_type parsed_result;                                                                                                                                                                                                                                 \
        parsed_result = parse_unsigned_integer(env, err, str, default_value, max_value);                                                                                                                                                                           \
        return parsed_result;                                                                                                                                                                                                                                      \
    }

DEFINE_SIGNED_PARSE_CONVERTER(convert_char, char)
DEFINE_SIGNED_PARSE_CONVERTER(convert_short, short)
DEFINE_SIGNED_PARSE_CONVERTER(convert_int, int)
DEFINE_SIGNED_PARSE_SAME_CONVERTER(convert_long, long)
DEFINE_SIGNED_PARSE_CONVERTER(convert_long_long, long long)
DEFINE_SIGNED_PARSE_CONVERTER(convert_signed_char, signed char)
DEFINE_SIGNED_PARSE_CONVERTER(convert_int8, int8_t)
DEFINE_SIGNED_PARSE_CONVERTER(convert_int16, int16_t)
DEFINE_SIGNED_PARSE_CONVERTER(convert_int32, int32_t)
DEFINE_SIGNED_PARSE_SAME_CONVERTER(convert_int64, int64_t)
DEFINE_UNSIGNED_PARSE_CONVERTER(convert_unsigned_char, unsigned char)
DEFINE_UNSIGNED_PARSE_CONVERTER(convert_unsigned_short, unsigned short)
DEFINE_UNSIGNED_PARSE_CONVERTER(convert_unsigned_int, unsigned int)
DEFINE_UNSIGNED_PARSE_SAME_CONVERTER(convert_unsigned_long, unsigned long)
DEFINE_UNSIGNED_PARSE_CONVERTER(convert_unsigned_long_long, unsigned long long)
DEFINE_UNSIGNED_PARSE_CONVERTER(convert_uint8, uint8_t)
DEFINE_UNSIGNED_PARSE_CONVERTER(convert_uint16, uint16_t)
DEFINE_UNSIGNED_PARSE_CONVERTER(convert_uint32, uint32_t)
DEFINE_UNSIGNED_PARSE_SAME_CONVERTER(convert_uint64, uint64_t)

#undef DEFINE_UNSIGNED_PARSE_SAME_CONVERTER
#undef DEFINE_UNSIGNED_PARSE_CONVERTER
#undef DEFINE_SIGNED_PARSE_SAME_CONVERTER
#undef DEFINE_SIGNED_PARSE_CONVERTER

char p101_parse_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, char, default_value);
    parsed_result = convert_char(env, err, str, default_value, CHAR_MIN, CHAR_MAX);
    P101_PARSE_EPILOGUE(env);
}

short p101_parse_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, short, default_value);
    parsed_result = convert_short(env, err, str, default_value, SHRT_MIN, SHRT_MAX);
    P101_PARSE_EPILOGUE(env);
}

int p101_parse_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int, default_value);
    parsed_result = convert_int(env, err, str, default_value, INT_MIN, INT_MAX);
    P101_PARSE_EPILOGUE(env);
}

long p101_parse_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, long, default_value);
    parsed_result = convert_long(env, err, str, default_value, LONG_MIN, LONG_MAX);
    P101_PARSE_EPILOGUE(env);
}

long long p101_parse_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, long long, default_value);
    parsed_result = convert_long_long(env, err, str, default_value, LLONG_MIN, LLONG_MAX);
    P101_PARSE_EPILOGUE(env);
}

unsigned char p101_parse_unsigned_char(const struct p101_env *env, struct p101_error *err, const char *str, unsigned char default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, unsigned char, default_value);
    parsed_result = convert_unsigned_char(env, err, str, default_value, UCHAR_MAX);
    P101_PARSE_EPILOGUE(env);
}

unsigned short p101_parse_unsigned_short(const struct p101_env *env, struct p101_error *err, const char *str, unsigned short default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, unsigned short, default_value);
    parsed_result = convert_unsigned_short(env, err, str, default_value, USHRT_MAX);
    P101_PARSE_EPILOGUE(env);
}

unsigned int p101_parse_unsigned_int(const struct p101_env *env, struct p101_error *err, const char *str, unsigned int default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, unsigned int, default_value);
    parsed_result = convert_unsigned_int(env, err, str, default_value, UINT_MAX);
    P101_PARSE_EPILOGUE(env);
}

unsigned long p101_parse_unsigned_long(const struct p101_env *env, struct p101_error *err, const char *str, unsigned long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, unsigned long, default_value);
    parsed_result = convert_unsigned_long(env, err, str, default_value, ULONG_MAX);
    P101_PARSE_EPILOGUE(env);
}

unsigned long long p101_parse_unsigned_long_long(const struct p101_env *env, struct p101_error *err, const char *str, unsigned long long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, unsigned long long, default_value);
    parsed_result = convert_unsigned_long_long(env, err, str, default_value, ULLONG_MAX);
    P101_PARSE_EPILOGUE(env);
}

signed char p101_parse_negative_char(const struct p101_env *env, struct p101_error *err, const char *str, signed char default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, signed char, default_value);
    parsed_result = convert_signed_char(env, err, str, default_value, SCHAR_MIN, -1);
    P101_PARSE_EPILOGUE(env);
}

short p101_parse_negative_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, short, default_value);
    parsed_result = convert_short(env, err, str, default_value, SHRT_MIN, -1);
    P101_PARSE_EPILOGUE(env);
}

int p101_parse_negative_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int, default_value);
    parsed_result = convert_int(env, err, str, default_value, INT_MIN, -1);
    P101_PARSE_EPILOGUE(env);
}

long p101_parse_negative_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, long, default_value);
    parsed_result = convert_long(env, err, str, default_value, LONG_MIN, -1L);
    P101_PARSE_EPILOGUE(env);
}

long long p101_parse_negative_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, long long, default_value);
    parsed_result = convert_long_long(env, err, str, default_value, LLONG_MIN, -1LL);
    P101_PARSE_EPILOGUE(env);
}

char p101_parse_positive_char(const struct p101_env *env, struct p101_error *err, const char *str, char default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, char, default_value);
    parsed_result = convert_char(env, err, str, default_value, 1, CHAR_MAX);
    P101_PARSE_EPILOGUE(env);
}

short p101_parse_positive_short(const struct p101_env *env, struct p101_error *err, const char *str, short default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, short, default_value);
    parsed_result = convert_short(env, err, str, default_value, 1, SHRT_MAX);
    P101_PARSE_EPILOGUE(env);
}

int p101_parse_positive_int(const struct p101_env *env, struct p101_error *err, const char *str, int default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int, default_value);
    parsed_result = convert_int(env, err, str, default_value, 1, INT_MAX);
    P101_PARSE_EPILOGUE(env);
}

long p101_parse_positive_long(const struct p101_env *env, struct p101_error *err, const char *str, long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, long, default_value);
    parsed_result = convert_long(env, err, str, default_value, 1, LONG_MAX);
    P101_PARSE_EPILOGUE(env);
}

long long p101_parse_positive_long_long(const struct p101_env *env, struct p101_error *err, const char *str, long long default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, long long, default_value);
    parsed_result = convert_long_long(env, err, str, default_value, 1, LLONG_MAX);
    P101_PARSE_EPILOGUE(env);
}

int8_t p101_parse_int8_t(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int8_t, default_value);
    parsed_result = convert_int8(env, err, str, default_value, INT8_MIN, INT8_MAX);
    P101_PARSE_EPILOGUE(env);
}

int16_t p101_parse_int16_t(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int16_t, default_value);
    parsed_result = convert_int16(env, err, str, default_value, INT16_MIN, INT16_MAX);
    P101_PARSE_EPILOGUE(env);
}

int32_t p101_parse_int32_t(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int32_t, default_value);
    parsed_result = convert_int32(env, err, str, default_value, INT32_MIN, INT32_MAX);
    P101_PARSE_EPILOGUE(env);
}

int64_t p101_parse_int64_t(const struct p101_env *env, struct p101_error *err, const char *str, int64_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int64_t, default_value);
    parsed_result = convert_int64(env, err, str, default_value, INT64_MIN, INT64_MAX);
    P101_PARSE_EPILOGUE(env);
}

uint8_t p101_parse_uint8_t(const struct p101_env *env, struct p101_error *err, const char *str, uint8_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, uint8_t, default_value);
    parsed_result = convert_uint8(env, err, str, default_value, UINT8_MAX);
    P101_PARSE_EPILOGUE(env);
}

uint16_t p101_parse_uint16_t(const struct p101_env *env, struct p101_error *err, const char *str, uint16_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, uint16_t, default_value);
    parsed_result = convert_uint16(env, err, str, default_value, UINT16_MAX);
    P101_PARSE_EPILOGUE(env);
}

uint32_t p101_parse_uint32_t(const struct p101_env *env, struct p101_error *err, const char *str, uint32_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, uint32_t, default_value);
    parsed_result = convert_uint32(env, err, str, default_value, UINT32_MAX);
    P101_PARSE_EPILOGUE(env);
}

uint64_t p101_parse_uint64_t(const struct p101_env *env, struct p101_error *err, const char *str, uint64_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, uint64_t, default_value);
    parsed_result = convert_uint64(env, err, str, default_value, UINT64_MAX);
    P101_PARSE_EPILOGUE(env);
}

int8_t p101_parse_negative_int8_t(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int8_t, default_value);
    parsed_result = convert_int8(env, err, str, default_value, INT8_MIN, -1);
    P101_PARSE_EPILOGUE(env);
}

int16_t p101_parse_negative_int16_t(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int16_t, default_value);
    parsed_result = convert_int16(env, err, str, default_value, INT16_MIN, -1);
    P101_PARSE_EPILOGUE(env);
}

int32_t p101_parse_negative_int32_t(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int32_t, default_value);
    parsed_result = convert_int32(env, err, str, default_value, INT32_MIN, -1);
    P101_PARSE_EPILOGUE(env);
}

int64_t p101_parse_negative_int64_t(const struct p101_env *env, struct p101_error *err, const char *str, int64_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int64_t, default_value);
    parsed_result = convert_int64(env, err, str, default_value, INT64_MIN, -1);
    P101_PARSE_EPILOGUE(env);
}

int8_t p101_parse_positive_int8_t(const struct p101_env *env, struct p101_error *err, const char *str, int8_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int8_t, default_value);
    parsed_result = convert_int8(env, err, str, default_value, 1, INT8_MAX);
    P101_PARSE_EPILOGUE(env);
}

int16_t p101_parse_positive_int16_t(const struct p101_env *env, struct p101_error *err, const char *str, int16_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int16_t, default_value);
    parsed_result = convert_int16(env, err, str, default_value, 1, INT16_MAX);
    P101_PARSE_EPILOGUE(env);
}

int32_t p101_parse_positive_int32_t(const struct p101_env *env, struct p101_error *err, const char *str, int32_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int32_t, default_value);
    parsed_result = convert_int32(env, err, str, default_value, 1, INT32_MAX);
    P101_PARSE_EPILOGUE(env);
}

int64_t p101_parse_positive_int64_t(const struct p101_env *env, struct p101_error *err, const char *str, int64_t default_value)
{
    P101_PARSE_PROLOGUE_ARG3(env, int64_t, default_value);
    parsed_result = convert_int64(env, err, str, default_value, 1, INT64_MAX);
    P101_PARSE_EPILOGUE(env);
}

#undef P101_PARSE_EPILOGUE
#undef P101_PARSE_PROLOGUE_ARG3
