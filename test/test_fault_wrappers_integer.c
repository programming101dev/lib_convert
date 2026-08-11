#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fmtmsg.h>
#include <fnmatch.h>
#include <ftw.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <p101_convert/errors.h>
#include <p101_convert/integer.h>
#include <p101_convert/networking.h>
#include <p101_env/env.h>
#include <p101_error/error.h>
#include <pthread.h>
#include <search.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
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
static bool   native_child_process;
static int    native_child_status = EXIT_SUCCESS;

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

#define P101_NATIVE_CLEANUP_ERRNO(expression)                                                                                                                                                                                                                      \
    do                                                                                                                                                                                                                                                             \
    {                                                                                                                                                                                                                                                              \
        int p101_cleanup_status_;                                                                                                                                                                                                                                  \
                                                                                                                                                                                                                                                                   \
        p101_cleanup_status_ = (expression);                                                                                                                                                                                                                       \
        if(p101_cleanup_status_ != 0)                                                                                                                                                                                                                              \
        {                                                                                                                                                                                                                                                          \
            fprintf(stderr, "native cleanup failed: %s: %s\n", #expression, strerror(errno));                                                                                                                                                                      \
            native_passed = false;                                                                                                                                                                                                                                 \
        }                                                                                                                                                                                                                                                          \
    } while(0)

#define P101_NATIVE_CLEANUP_STATUS(expression)                                                                                                                                                                                                                     \
    do                                                                                                                                                                                                                                                             \
    {                                                                                                                                                                                                                                                              \
        int p101_cleanup_status_ = (expression);                                                                                                                                                                                                                   \
        if(p101_cleanup_status_ != 0)                                                                                                                                                                                                                              \
        {                                                                                                                                                                                                                                                          \
            fprintf(stderr, "native cleanup failed: %s: status %d\n", #expression, p101_cleanup_status_);                                                                                                                                                          \
            native_passed = false;                                                                                                                                                                                                                                 \
        }                                                                                                                                                                                                                                                          \
    } while(0)

#define P101_NATIVE_CLEANUP_UNLINK_IF_PRESENT(path)                                                                                                                                                                                                                \
    do                                                                                                                                                                                                                                                             \
    {                                                                                                                                                                                                                                                              \
        bool p101_cleanup_ok_;                                                                                                                                                                                                                                     \
                                                                                                                                                                                                                                                                   \
        p101_cleanup_ok_ = native_unlink_if_present(path);                                                                                                                                                                                                         \
        if(!p101_cleanup_ok_)                                                                                                                                                                                                                                      \
        {                                                                                                                                                                                                                                                          \
            native_passed = false;                                                                                                                                                                                                                                 \
        }                                                                                                                                                                                                                                                          \
    } while(0)

#define P101_NATIVE_FORMAT_PID_PATH_OR_SKIP(buffer, format)                                                                                                                                                                                                        \
    do                                                                                                                                                                                                                                                             \
    {                                                                                                                                                                                                                                                              \
        bool p101_format_ok_;                                                                                                                                                                                                                                      \
                                                                                                                                                                                                                                                                   \
        p101_format_ok_ = native_format_pid_path((buffer), sizeof(buffer), (format));                                                                                                                                                                              \
        if(!p101_format_ok_)                                                                                                                                                                                                                                       \
        {                                                                                                                                                                                                                                                          \
            fprintf(stderr, "native setup failed: path formatting\n");                                                                                                                                                                                             \
            native_child_status = 77;                                                                                                                                                                                                                              \
            goto native_child_done_;                                                                                                                                                                                                                               \
        }                                                                                                                                                                                                                                                          \
    } while(0)

struct fault_state
{
    int checks;
    int code;
};

static pid_t native_waitpid_nointr(pid_t pid, int *status) P101_ATTR_SEMANTIC_ROLE("p101:test:eintr-safe-wait-adapter")
{
    pid_t result;

    do
    {
        result = waitpid(pid, status, 0);
    } while(result < 0 && errno == EINTR);
    return result;
}

static void write_outcome(const char *wrapper, const char *domain, const char *symbol, int code, int passed)
{
    int written;

    if(outcome_stream != NULL)
    {
        written = fprintf(outcome_stream, "P101WRAPPER\t1\tFAULT\t%s\tlib_convert\t%s\t%s\t%s\t%d\t%s\n", P101_TEST_PLATFORM, wrapper, domain, symbol, code, passed ? "PASS" : "FAIL");
        if(written < 0 || fflush(outcome_stream) != 0)
        {
            fprintf(stderr, "FAIL: cannot write wrapper outcome receipt\n");
            failures++;
        }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            char native_result = p101_parse_char(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_char: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_char: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_char\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_char: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int native_result = p101_parse_int(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_int: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_int: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_int\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_int: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int16_t native_result = p101_parse_int16_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_int16_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_int16_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_int16_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_int16_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int32_t native_result = p101_parse_int32_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_int32_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_int32_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_int32_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_int32_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int64_t native_result = p101_parse_int64_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_int64_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_int64_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_int64_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_int64_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int8_t native_result = p101_parse_int8_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_int8_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_int8_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_int8_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_int8_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            long native_result = p101_parse_long(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            long long native_result = p101_parse_long_long(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_long_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_long_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_long_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_long_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            signed char native_result = p101_parse_negative_char(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_char: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_char: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_char\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_char: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int native_result = p101_parse_negative_int(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_int: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_int: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_int\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_int: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int16_t native_result = p101_parse_negative_int16_t(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_int16_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_int16_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_int16_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_int16_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int32_t native_result = p101_parse_negative_int32_t(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_int32_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_int32_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_int32_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_int32_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int64_t native_result = p101_parse_negative_int64_t(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_int64_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_int64_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_int64_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_int64_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int8_t native_result = p101_parse_negative_int8_t(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_int8_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_int8_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_int8_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_int8_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            long native_result = p101_parse_negative_long(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            long long native_result = p101_parse_negative_long_long(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_long_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_long_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_long_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_long_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            short native_result = p101_parse_negative_short(native_env, native_err, "-1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_negative_short: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_negative_short: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_negative_short\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_negative_short: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            char native_result = p101_parse_positive_char(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_char: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_char: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_char\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_char: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int native_result = p101_parse_positive_int(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_int: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_int: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_int\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_int: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int16_t native_result = p101_parse_positive_int16_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_int16_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_int16_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_int16_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_int16_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int32_t native_result = p101_parse_positive_int32_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_int32_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_int32_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_int32_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_int32_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int64_t native_result = p101_parse_positive_int64_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_int64_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_int64_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_int64_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_int64_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            int8_t native_result = p101_parse_positive_int8_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_int8_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_int8_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_int8_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_int8_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            long native_result = p101_parse_positive_long(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            long long native_result = p101_parse_positive_long_long(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_long_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_long_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_long_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_long_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            short native_result = p101_parse_positive_short(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_positive_short: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_positive_short: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_positive_short\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_positive_short: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            short native_result = p101_parse_short(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_short: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_short: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_short\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_short: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            uint16_t native_result = p101_parse_uint16_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_uint16_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_uint16_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_uint16_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_uint16_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            uint32_t native_result = p101_parse_uint32_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_uint32_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_uint32_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_uint32_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_uint32_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            uint64_t native_result = p101_parse_uint64_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_uint64_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_uint64_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_uint64_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_uint64_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            uint8_t native_result = p101_parse_uint8_t(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_uint8_t: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_uint8_t: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_uint8_t\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_uint8_t: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            unsigned char native_result = p101_parse_unsigned_char(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_unsigned_char: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_unsigned_char: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_unsigned_char\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_unsigned_char: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            unsigned int native_result = p101_parse_unsigned_int(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_unsigned_int: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_unsigned_int: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_unsigned_int\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_unsigned_int: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            unsigned long native_result = p101_parse_unsigned_long(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_unsigned_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_unsigned_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_unsigned_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_unsigned_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            unsigned long long native_result = p101_parse_unsigned_long_long(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_unsigned_long_long: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_unsigned_long_long: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_unsigned_long_long\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_unsigned_long_long: %d\n", WEXITSTATUS(native_status));
                }
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
            bool               native_passed = true;
            struct p101_error *native_err    = NULL;
            struct p101_env   *native_env    = NULL;
            FILE              *native_stdin_result;

            native_child_process = true;
            failures             = 0;
            (void)alarm(2U);
            if(unsetenv("P101_CALL_LOG") != 0 || unsetenv("P101_RESOURCE_LOG") != 0)
            {
                fprintf(stderr, "native setup failed: cannot clear p101 logging environment\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_stdin_result = freopen("/dev/null", "r", stdin);
            if(native_stdin_result == NULL)
            {
                fprintf(stderr, "native setup failed: cannot make standard input deterministic\n");
                native_child_status = 77;
                goto native_child_done_;
            }
            native_err = p101_error_create(false);
            if(native_err == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            native_env = p101_env_create(native_err, NULL);
            if(native_env == NULL)
            {
                native_child_status = 77;
                goto native_child_done_;
            }
            unsigned short native_result = p101_parse_unsigned_short(native_env, native_err, "1", 0);
            (void)native_result;
            if(p101_error_has_error(native_err))
            {
                bool native_error_declared = false;

                for(size_t native_error_index = 0U; native_error_index < sizeof(errors) / sizeof(errors[0]); native_error_index++)
                {
                    if(p101_error_is_errno(native_err, errors[native_error_index]))
                    {
                        native_error_declared = true;
                    }
                }
                if(!native_error_declared)
                {
                    fprintf(stderr, "native smoke produced an undeclared platform failure: p101_parse_unsigned_short: %s\n", p101_error_get_message(native_err));
                    native_passed = false;
                }
                p101_error_reset(native_err);
            }
            native_child_status = native_passed ? EXIT_SUCCESS : EXIT_FAILURE;
        native_child_done_:
            p101_env_destroy(native_env);
            p101_error_destroy(native_err);
        }
        if(native_pid > 0)
        {
            EXPECT(native_waitpid_nointr(native_pid, &native_status) == native_pid);
            if(WIFSIGNALED(native_status))
            {
                fprintf(stderr, "native smoke terminated by signal: p101_parse_unsigned_short: %d\n", WTERMSIG(native_status));
            }
            EXPECT(WIFEXITED(native_status));
            if(WIFEXITED(native_status) && WEXITSTATUS(native_status) == 77)
            {
                fprintf(stderr, "native smoke fixture unavailable: p101_parse_unsigned_short\n");
            }
            else if(WIFEXITED(native_status))
            {
                if(WEXITSTATUS(native_status) != EXIT_SUCCESS)
                {
                    fprintf(stderr, "native smoke exited unsuccessfully: p101_parse_unsigned_short: %d\n", WEXITSTATUS(native_status));
                }
                EXPECT(WEXITSTATUS(native_status) == EXIT_SUCCESS);
            }
        }
        p101_error_reset(err);
    }
}

int main(void)
{
    const char        *outcome_path;
    struct p101_error *err = NULL;
    struct p101_env   *env = NULL;
    int                status;

    outcome_path = getenv("P101_WRAPPER_OUTCOME_LOG");
    if(outcome_path != NULL && outcome_path[0] != '\0')
    {
        outcome_stream = fopen(outcome_path, "a");
        if(outcome_stream == NULL)
        {
            fprintf(stderr, "FAIL: cannot open wrapper outcome receipt\n");
            failures++;
        }
    }
    if(failures == 0)
    {
        err = p101_error_create(false);
    }
    if(err != NULL)
    {
        env = p101_env_create(err, NULL);
    }
    if(env == NULL)
    {
        failures++;
    }
    else
    {
        p101_env_set_fd_observer(env, count_fd_event, NULL);
        p101_env_set_alloc_observer(env, count_alloc_event, NULL);
        p101_env_set_resource_observer(env, count_resource_event, NULL);
        if(!native_child_process)
        {
            test_p101_parse_char(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_int(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_int16_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_int32_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_int64_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_int8_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_long_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_char(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_int(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_int16_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_int32_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_int64_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_int8_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_long_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_negative_short(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_char(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_int(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_int16_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_int32_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_int64_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_int8_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_long_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_positive_short(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_short(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_uint16_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_uint32_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_uint64_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_uint8_t(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_unsigned_char(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_unsigned_int(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_unsigned_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_unsigned_long_long(env, err);
        }
        if(!native_child_process)
        {
            test_p101_parse_unsigned_short(env, err);
        }
    }
    p101_env_destroy(env);
    p101_error_destroy(err);
    if(outcome_stream != NULL && fclose(outcome_stream) != 0)
    {
        fprintf(stderr, "FAIL: cannot close wrapper outcome receipt\n");
        failures++;
    }
    if(native_child_process)
    {
        status = native_child_status;
        if(status == EXIT_SUCCESS && failures != 0)
        {
            status = EXIT_FAILURE;
        }
    }
    else
    {
        status = failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    return status;
}
