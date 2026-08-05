#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fmtmsg.h>
#include <fnmatch.h>
#include <ftw.h>
#include <limits.h>
#include <math.h>
#include <p101_convert/errors.h>
#include <p101_convert/integer.h>
#include <p101_convert/networking.h>
#include <p101_env/env.h>
#include <p101_error/error.h>
#include <pthread.h>
#include <search.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utmpx.h>

static int    failures;
static size_t fault_resource_events;
static FILE  *outcome_stream;

#define P101_TEST_ERRNO_SENTINEL 0x5A5A

#ifdef __linux__
    #define P101_TEST_PLATFORM "linux"
#elif defined(__APPLE__)
    #define P101_TEST_PLATFORM "macos"
#elif defined(__FreeBSD__)
    #define P101_TEST_PLATFORM "freebsd"
#else
    #define P101_TEST_PLATFORM "posix"
#endif

#define EXPECT(condition)                                                                                                                                                                                                                                          \
    do                                                                                                                                                                                                                                                             \
    {                                                                                                                                                                                                                                                              \
        if(!(condition))                                                                                                                                                                                                                                           \
        {                                                                                                                                                                                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                                                                                                                                                   \
            failures++;                                                                                                                                                                                                                                            \
        }                                                                                                                                                                                                                                                          \
    } while(0)

struct fault_state
{
    int checks;
    int code;
};

static void write_outcome(const char *wrapper, const char *domain, const char *symbol, int code, int passed)
{
    int written;

    if(outcome_stream == NULL)
    {
        return;
    }
    written = fprintf(outcome_stream, "P101WRAPPER\t1\tFAULT\t%s\tlib_convert\t%s\t%s\t%s\t%d\t%s\n", P101_TEST_PLATFORM, wrapper, domain, symbol, code, passed ? "PASS" : "FAIL");
    if(written < 0 || fflush(outcome_stream) != 0)
    {
        fprintf(stderr, "FAIL: cannot write wrapper outcome receipt\n");
        failures++;
    }
}

static int fail_next_call(const struct p101_env *env, const char *call_name, void *user_data)
{
    struct fault_state *state;

    (void)env;
    (void)call_name;
    state = user_data;
    state->checks++;
    return state->code;
}

static void count_fd_event(const struct p101_env *env, p101_env_fd_event event, int fd, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    (void)env;
    (void)event;
    (void)fd;
    (void)file_name;
    (void)function_name;
    (void)line_number;
    (void)user_data;
    fault_resource_events++;
}

static void count_alloc_event(const struct p101_env *env, p101_env_alloc_event event, const void *ptr, const void *new_ptr, size_t size, const char *file_name, const char *function_name, int line_number, void *user_data)
{
    (void)env;
    (void)event;
    (void)ptr;
    (void)new_ptr;
    (void)size;
    (void)file_name;
    (void)function_name;
    (void)line_number;
    (void)user_data;
    fault_resource_events++;
}

static void count_resource_event(const struct p101_env *env, p101_env_resource_kind event, const char *resource_class, const char *resource_id, const char *related_id, size_t size, const char *metadata, const char *file_name, const char *function_name,
                                 int line_number, void *user_data)
{
    (void)env;
    (void)event;
    (void)resource_class;
    (void)resource_id;
    (void)related_id;
    (void)size;
    (void)metadata;
    (void)file_name;
    (void)function_name;
    (void)line_number;
    (void)user_data;
    fault_resource_events++;
}

/* P101_TEST_CASE(p101_convert_address) */
static void test_p101_convert_address(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        socklen_t result = p101_convert_address(env, err, NULL, NULL);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_convert_address", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            struct sockaddr_storage native_argument_3 = {0};
            socklen_t               native_result     = p101_convert_address(native_env, native_err, "p101", &native_argument_3);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_char) */
static void test_p101_parse_char(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        char result = p101_parse_char(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_char", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            char native_result = p101_parse_char(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_in_port_t) */
static void test_p101_parse_in_port_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        in_port_t result = p101_parse_in_port_t(env, err, NULL);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_in_port_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            in_port_t native_result = p101_parse_in_port_t(native_env, native_err, "p101");
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_int) */
static void test_p101_parse_int(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int result = p101_parse_int(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_int", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int native_result = p101_parse_int(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_int16_t) */
static void test_p101_parse_int16_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int16_t result = p101_parse_int16_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_int16_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int16_t native_result = p101_parse_int16_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_int32_t) */
static void test_p101_parse_int32_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int32_t result = p101_parse_int32_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_int32_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int32_t native_result = p101_parse_int32_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_int64_t) */
static void test_p101_parse_int64_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int64_t result = p101_parse_int64_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_int64_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int64_t native_result = p101_parse_int64_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_int8_t) */
static void test_p101_parse_int8_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int8_t result = p101_parse_int8_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_int8_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int8_t native_result = p101_parse_int8_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_long) */
static void test_p101_parse_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        long result = p101_parse_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            long native_result = p101_parse_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_long_long) */
static void test_p101_parse_long_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        long long result = p101_parse_long_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_long_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            long long native_result = p101_parse_long_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_char) */
static void test_p101_parse_negative_char(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        signed char result = p101_parse_negative_char(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_char", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            signed char native_result = p101_parse_negative_char(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_int) */
static void test_p101_parse_negative_int(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int result = p101_parse_negative_int(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_int", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int native_result = p101_parse_negative_int(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_int16_t) */
static void test_p101_parse_negative_int16_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int16_t result = p101_parse_negative_int16_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_int16_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int16_t native_result = p101_parse_negative_int16_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_int32_t) */
static void test_p101_parse_negative_int32_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int32_t result = p101_parse_negative_int32_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_int32_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int32_t native_result = p101_parse_negative_int32_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_int64_t) */
static void test_p101_parse_negative_int64_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int64_t result = p101_parse_negative_int64_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_int64_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int64_t native_result = p101_parse_negative_int64_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_int8_t) */
static void test_p101_parse_negative_int8_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int8_t result = p101_parse_negative_int8_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_int8_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int8_t native_result = p101_parse_negative_int8_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_long) */
static void test_p101_parse_negative_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        long result = p101_parse_negative_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            long native_result = p101_parse_negative_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_long_long) */
static void test_p101_parse_negative_long_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        long long result = p101_parse_negative_long_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_long_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            long long native_result = p101_parse_negative_long_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_negative_short) */
static void test_p101_parse_negative_short(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        short result = p101_parse_negative_short(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_negative_short", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            short native_result = p101_parse_negative_short(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_char) */
static void test_p101_parse_positive_char(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        char result = p101_parse_positive_char(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_char", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            char native_result = p101_parse_positive_char(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_int) */
static void test_p101_parse_positive_int(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int result = p101_parse_positive_int(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_int", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int native_result = p101_parse_positive_int(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_int16_t) */
static void test_p101_parse_positive_int16_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int16_t result = p101_parse_positive_int16_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_int16_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int16_t native_result = p101_parse_positive_int16_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_int32_t) */
static void test_p101_parse_positive_int32_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int32_t result = p101_parse_positive_int32_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_int32_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int32_t native_result = p101_parse_positive_int32_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_int64_t) */
static void test_p101_parse_positive_int64_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int64_t result = p101_parse_positive_int64_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_int64_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int64_t native_result = p101_parse_positive_int64_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_int8_t) */
static void test_p101_parse_positive_int8_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        int8_t result = p101_parse_positive_int8_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_int8_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            int8_t native_result = p101_parse_positive_int8_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_long) */
static void test_p101_parse_positive_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        long result = p101_parse_positive_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            long native_result = p101_parse_positive_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_long_long) */
static void test_p101_parse_positive_long_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        long long result = p101_parse_positive_long_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_long_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            long long native_result = p101_parse_positive_long_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_positive_short) */
static void test_p101_parse_positive_short(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        short result = p101_parse_positive_short(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_positive_short", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            short native_result = p101_parse_positive_short(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_short) */
static void test_p101_parse_short(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        short result = p101_parse_short(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_short", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            short native_result = p101_parse_short(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_uint16_t) */
static void test_p101_parse_uint16_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        uint16_t result = p101_parse_uint16_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_uint16_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            uint16_t native_result = p101_parse_uint16_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_uint32_t) */
static void test_p101_parse_uint32_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        uint32_t result = p101_parse_uint32_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_uint32_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            uint32_t native_result = p101_parse_uint32_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_uint64_t) */
static void test_p101_parse_uint64_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        uint64_t result = p101_parse_uint64_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_uint64_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            uint64_t native_result = p101_parse_uint64_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_uint8_t) */
static void test_p101_parse_uint8_t(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        uint8_t result = p101_parse_uint8_t(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_uint8_t", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            uint8_t native_result = p101_parse_uint8_t(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_unsigned_char) */
static void test_p101_parse_unsigned_char(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        unsigned char result = p101_parse_unsigned_char(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_unsigned_char", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            unsigned char native_result = p101_parse_unsigned_char(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_unsigned_int) */
static void test_p101_parse_unsigned_int(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        unsigned int result = p101_parse_unsigned_int(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_unsigned_int", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            unsigned int native_result = p101_parse_unsigned_int(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_unsigned_long) */
static void test_p101_parse_unsigned_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        unsigned long result = p101_parse_unsigned_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_unsigned_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            unsigned long native_result = p101_parse_unsigned_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_unsigned_long_long) */
static void test_p101_parse_unsigned_long_long(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        unsigned long long result = p101_parse_unsigned_long_long(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_unsigned_long_long", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            unsigned long long native_result = p101_parse_unsigned_long_long(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

/* P101_TEST_CASE(p101_parse_unsigned_short) */
static void test_p101_parse_unsigned_short(struct p101_env *env, struct p101_error *err)
{
#ifdef __linux__
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__APPLE__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#elif defined(__FreeBSD__)
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#else
    static const int         errors[]      = {EIO};
    static const char *const error_names[] = {"EIO"};
#endif

    for(size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); index++)
    {
        struct fault_state state = {0, errors[index]};
        int                failures_before;

        failures_before = failures;
        EXPECT(p101_error_has_no_error(err));
        fault_resource_events = 0U;
        errno                 = P101_TEST_ERRNO_SENTINEL;
        p101_env_set_fault_injector(env, fail_next_call, &state);
        unsigned short result = p101_parse_unsigned_short(env, err, NULL, 0);
        (void)result;
        EXPECT(state.checks == 1);
        EXPECT(p101_error_is_errno(err, state.code));
        EXPECT(errno == P101_TEST_ERRNO_SENTINEL);
        EXPECT(result == (0));
        EXPECT(fault_resource_events == 0U);
        write_outcome("p101_parse_unsigned_short", "errno", error_names[index], state.code, failures == failures_before);
        p101_error_reset(err);
    }
    p101_env_set_fault_injector(env, NULL, NULL);
    {
        int   native_status = 0;
        pid_t native_pid    = fork();

        EXPECT(native_pid >= 0);
        if(native_pid == 0)
        {
            struct p101_error *native_err;
            struct p101_env   *native_env;

            (void)alarm(2U);
            (void)unsetenv("P101_CALL_LOG");
            (void)unsetenv("P101_RESOURCE_LOG");
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                _Exit(77);
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                p101_error_destroy(native_err);
                _Exit(77);
            }
            unsigned short native_result = p101_parse_unsigned_short(native_env, native_err, "p101", 0);
            (void)native_result;
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
            _Exit(EXIT_SUCCESS);
        }
        if(native_pid > 0)
        {
            EXPECT(waitpid(native_pid, &native_status, 0) == native_pid);
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status))
            {
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

int main(void)
{
    const char        *outcome_path;
    struct p101_error *err;
    struct p101_env   *env;

    outcome_path = getenv("P101_WRAPPER_OUTCOME_LOG");
    if(outcome_path != NULL && outcome_path[0] != '\0')
    {
        outcome_stream = fopen(outcome_path, "a");
        if(outcome_stream == NULL)
        {
            fprintf(stderr, "FAIL: cannot open wrapper outcome receipt\n");
            return EXIT_FAILURE;
        }
    }
    err = p101_error_create(false);
    if(err == NULL)
    {
        if(outcome_stream != NULL)
        {
            (void)fclose(outcome_stream);
        }
        return EXIT_FAILURE;
    }
    env = p101_env_create(err, NULL);
    if(env == NULL)
    {
        p101_error_destroy(err);
        if(outcome_stream != NULL)
        {
            (void)fclose(outcome_stream);
        }
        return EXIT_FAILURE;
    }
    p101_env_set_fd_observer(env, count_fd_event, NULL);
    p101_env_set_alloc_observer(env, count_alloc_event, NULL);
    p101_env_set_resource_observer(env, count_resource_event, NULL);
    test_p101_convert_address(env, err);
    test_p101_parse_char(env, err);
    test_p101_parse_in_port_t(env, err);
    test_p101_parse_int(env, err);
    test_p101_parse_int16_t(env, err);
    test_p101_parse_int32_t(env, err);
    test_p101_parse_int64_t(env, err);
    test_p101_parse_int8_t(env, err);
    test_p101_parse_long(env, err);
    test_p101_parse_long_long(env, err);
    test_p101_parse_negative_char(env, err);
    test_p101_parse_negative_int(env, err);
    test_p101_parse_negative_int16_t(env, err);
    test_p101_parse_negative_int32_t(env, err);
    test_p101_parse_negative_int64_t(env, err);
    test_p101_parse_negative_int8_t(env, err);
    test_p101_parse_negative_long(env, err);
    test_p101_parse_negative_long_long(env, err);
    test_p101_parse_negative_short(env, err);
    test_p101_parse_positive_char(env, err);
    test_p101_parse_positive_int(env, err);
    test_p101_parse_positive_int16_t(env, err);
    test_p101_parse_positive_int32_t(env, err);
    test_p101_parse_positive_int64_t(env, err);
    test_p101_parse_positive_int8_t(env, err);
    test_p101_parse_positive_long(env, err);
    test_p101_parse_positive_long_long(env, err);
    test_p101_parse_positive_short(env, err);
    test_p101_parse_short(env, err);
    test_p101_parse_uint16_t(env, err);
    test_p101_parse_uint32_t(env, err);
    test_p101_parse_uint64_t(env, err);
    test_p101_parse_uint8_t(env, err);
    test_p101_parse_unsigned_char(env, err);
    test_p101_parse_unsigned_int(env, err);
    test_p101_parse_unsigned_long(env, err);
    test_p101_parse_unsigned_long_long(env, err);
    test_p101_parse_unsigned_short(env, err);
    p101_env_destroy(env);
    p101_error_destroy(err);
    if(outcome_stream != NULL && fclose(outcome_stream) != 0)
    {
        fprintf(stderr, "FAIL: cannot close wrapper outcome receipt\n");
        failures++;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
