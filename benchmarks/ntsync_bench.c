/*
 * Win32 synchronization benchmark for measuring ntsync impact under Wine.
 *
 * This program intentionally uses only common Win32 synchronization APIs and
 * the C runtime so it can be built by GitHub Actions with the MSVC toolchain.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_M_X64) && !defined(__x86_64__)
#error This benchmark must be built as x64.
#endif

#define BENCH_VERSION "1.0"
#define DEFAULT_ITERATIONS 1000000u
#define DEFAULT_WARMUP 10000u
#define WAIT_OBJECT_COUNT 8u
#define CACHELINE_SIZE 64u

typedef struct bench_options {
    uint32_t iterations;
    uint32_t warmup;
    uint32_t threads;
    const char *case_filter;
    int csv_only;
    int no_pause;
    int show_help;
} bench_options;

typedef struct bench_result {
    const char *name;
    uint32_t threads;
    uint32_t iterations;
    double elapsed_ms;
    double ops_per_sec;
    double avg_ns_per_op;
    double score;
    int ok;
} bench_result;

typedef int (*bench_fn)(const bench_options *options, bench_result *result);

typedef struct bench_case {
    const char *name;
    bench_fn run;
} bench_case;

typedef struct pingpong_ctx {
    uint32_t iterations;
    HANDLE ready[2];
    HANDLE done[2];
    HANDLE token[2];
    HANDLE shared;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
} pingpong_ctx;

typedef struct pingpong_thread_arg {
    pingpong_ctx *ctx;
    uint32_t index;
} pingpong_thread_arg;

typedef struct condvar_ctx {
    uint32_t iterations;
    SRWLOCK lock;
    CONDITION_VARIABLE cond;
    uint32_t turn;
    uint32_t ready_count;
    HANDLE ready_event;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
} condvar_ctx;

typedef struct condvar_thread_arg {
    condvar_ctx *ctx;
    uint32_t index;
} condvar_thread_arg;

typedef struct wait_any_ctx {
    uint32_t iterations;
    HANDLE event_ready;
    HANDLE worker_ready;
    HANDLE done;
    HANDLE events[WAIT_OBJECT_COUNT];
    LARGE_INTEGER start;
    LARGE_INTEGER end;
} wait_any_ctx;

typedef struct wait_all_ctx {
    uint32_t iterations;
    HANDLE iteration_ready;
    HANDLE worker_ready;
    HANDLE done;
    HANDLE events[WAIT_OBJECT_COUNT];
    LARGE_INTEGER start;
    LARGE_INTEGER end;
} wait_all_ctx;

typedef struct contended_ctx {
    uint32_t iterations;
    uint32_t threads;
    HANDLE mutex;
    volatile LONG ready_count;
    HANDLE start_event;
    volatile LONG64 counter;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
} contended_ctx;

typedef struct contended_thread_arg {
    contended_ctx *ctx;
    uint32_t index;
    unsigned char padding[CACHELINE_SIZE - sizeof(void *) - sizeof(uint32_t)];
} contended_thread_arg;

static LARGE_INTEGER g_qpc_frequency;

static uint32_t clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static double qpc_elapsed_ms(LARGE_INTEGER start, LARGE_INTEGER end)
{
    return ((double)(end.QuadPart - start.QuadPart) * 1000.0) /
           (double)g_qpc_frequency.QuadPart;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (!text || !*text)
        return 0;

    parsed = strtoul(text, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > 0xffffffffUL)
        return 0;

    *value = (uint32_t)parsed;
    return 1;
}

static int equals_ignore_case(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return 0;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static void close_handle(HANDLE *handle)
{
    if (handle && *handle) {
        CloseHandle(*handle);
        *handle = NULL;
    }
}

static void sleep_briefly(void)
{
    Sleep(0);
}

static uint32_t logical_cpu_count(void)
{
    SYSTEM_INFO info;

    GetNativeSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1u;
}

static const char *processor_architecture(void)
{
    SYSTEM_INFO info;

    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x64";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    default:
        return "unknown";
    }
}

static void fill_result(bench_result *result, const char *name, uint32_t threads,
                        uint32_t iterations, double elapsed_ms, double ops)
{
    result->name = name;
    result->threads = threads;
    result->iterations = iterations;
    result->elapsed_ms = elapsed_ms;
    result->ops_per_sec = elapsed_ms > 0.0 ? (ops * 1000.0) / elapsed_ms : 0.0;
    result->avg_ns_per_op = ops > 0.0 ? (elapsed_ms * 1000000.0) / ops : 0.0;
    result->score = result->ops_per_sec / 1000.0;
    result->ok = 1;
}

static DWORD WINAPI mutex_pingpong_thread(LPVOID param)
{
    pingpong_thread_arg *arg = (pingpong_thread_arg *)param;
    pingpong_ctx *ctx = arg->ctx;
    const uint32_t index = arg->index;
    const uint32_t other = index ^ 1u;
    uint32_t i;

    SetEvent(ctx->ready[index]);
    WaitForSingleObject(ctx->token[index], INFINITE);
    if (index == 0)
        QueryPerformanceCounter(&ctx->start);

    for (i = 0; i < ctx->iterations; i++) {
        WaitForSingleObject(ctx->shared, INFINITE);
        ReleaseMutex(ctx->shared);
        SetEvent(ctx->token[other]);

        if (i + 1 < ctx->iterations)
            WaitForSingleObject(ctx->token[index], INFINITE);
    }

    if (index == 1)
        QueryPerformanceCounter(&ctx->end);
    SetEvent(ctx->done[index]);
    return 0;
}

static int run_mutex_pingpong_once(uint32_t iterations, double *elapsed_ms)
{
    pingpong_ctx ctx;
    pingpong_thread_arg args[2];
    HANDLE threads[2] = {0};
    int ok = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = iterations;
    ctx.shared = CreateMutexA(NULL, FALSE, NULL);
    ctx.ready[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.ready[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.token[0] = CreateEventA(NULL, FALSE, FALSE, NULL);
    ctx.token[1] = CreateEventA(NULL, FALSE, FALSE, NULL);

    if (!ctx.shared || !ctx.ready[0] || !ctx.ready[1] || !ctx.done[0] || !ctx.done[1] ||
        !ctx.token[0] || !ctx.token[1])
        goto cleanup;

    args[0].ctx = &ctx;
    args[0].index = 0;
    args[1].ctx = &ctx;
    args[1].index = 1;
    threads[0] = CreateThread(NULL, 0, mutex_pingpong_thread, &args[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, mutex_pingpong_thread, &args[1], 0, NULL);
    if (!threads[0] || !threads[1])
        goto cleanup;

    if (WaitForMultipleObjects(2, ctx.ready, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;
    SetEvent(ctx.token[0]);
    if (WaitForMultipleObjects(2, ctx.done, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    *elapsed_ms = qpc_elapsed_ms(ctx.start, ctx.end);
    ok = 1;

cleanup:
    if (threads[0]) {
        WaitForSingleObject(threads[0], 1000);
        CloseHandle(threads[0]);
    }
    if (threads[1]) {
        WaitForSingleObject(threads[1], 1000);
        CloseHandle(threads[1]);
    }
    close_handle(&ctx.shared);
    close_handle(&ctx.ready[0]);
    close_handle(&ctx.ready[1]);
    close_handle(&ctx.done[0]);
    close_handle(&ctx.done[1]);
    close_handle(&ctx.token[0]);
    close_handle(&ctx.token[1]);
    return ok;
}

static int bench_mutex_pingpong(const bench_options *options, bench_result *result)
{
    double elapsed_ms;

    if (!run_mutex_pingpong_once(options->warmup, &elapsed_ms))
        return 0;
    sleep_briefly();
    if (!run_mutex_pingpong_once(options->iterations, &elapsed_ms))
        return 0;

    fill_result(result, "mutex_pingpong", 2, options->iterations, elapsed_ms,
                (double)options->iterations * 2.0);
    return 1;
}

static DWORD WINAPI semaphore_pingpong_thread(LPVOID param)
{
    pingpong_thread_arg *arg = (pingpong_thread_arg *)param;
    pingpong_ctx *ctx = arg->ctx;
    const uint32_t index = arg->index;
    const uint32_t other = index ^ 1u;
    uint32_t i;

    SetEvent(ctx->ready[index]);
    WaitForSingleObject(ctx->token[index], INFINITE);
    if (index == 0)
        QueryPerformanceCounter(&ctx->start);

    for (i = 0; i < ctx->iterations; i++) {
        ReleaseSemaphore(ctx->token[other], 1, NULL);
        if (i + 1 < ctx->iterations)
            WaitForSingleObject(ctx->token[index], INFINITE);
    }

    if (index == 1)
        QueryPerformanceCounter(&ctx->end);
    SetEvent(ctx->done[index]);
    return 0;
}

static int run_semaphore_pingpong_once(uint32_t iterations, double *elapsed_ms)
{
    pingpong_ctx ctx;
    pingpong_thread_arg args[2];
    HANDLE threads[2] = {0};
    int ok = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = iterations;
    ctx.ready[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.ready[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.token[0] = CreateSemaphoreA(NULL, 0, 1, NULL);
    ctx.token[1] = CreateSemaphoreA(NULL, 0, 1, NULL);

    if (!ctx.ready[0] || !ctx.ready[1] || !ctx.done[0] || !ctx.done[1] ||
        !ctx.token[0] || !ctx.token[1])
        goto cleanup;

    args[0].ctx = &ctx;
    args[0].index = 0;
    args[1].ctx = &ctx;
    args[1].index = 1;
    threads[0] = CreateThread(NULL, 0, semaphore_pingpong_thread, &args[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, semaphore_pingpong_thread, &args[1], 0, NULL);
    if (!threads[0] || !threads[1])
        goto cleanup;

    if (WaitForMultipleObjects(2, ctx.ready, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;
    ReleaseSemaphore(ctx.token[0], 1, NULL);
    if (WaitForMultipleObjects(2, ctx.done, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    *elapsed_ms = qpc_elapsed_ms(ctx.start, ctx.end);
    ok = 1;

cleanup:
    if (threads[0]) {
        WaitForSingleObject(threads[0], 1000);
        CloseHandle(threads[0]);
    }
    if (threads[1]) {
        WaitForSingleObject(threads[1], 1000);
        CloseHandle(threads[1]);
    }
    close_handle(&ctx.ready[0]);
    close_handle(&ctx.ready[1]);
    close_handle(&ctx.done[0]);
    close_handle(&ctx.done[1]);
    close_handle(&ctx.token[0]);
    close_handle(&ctx.token[1]);
    return ok;
}

static int bench_semaphore_pingpong(const bench_options *options, bench_result *result)
{
    double elapsed_ms;

    if (!run_semaphore_pingpong_once(options->warmup, &elapsed_ms))
        return 0;
    sleep_briefly();
    if (!run_semaphore_pingpong_once(options->iterations, &elapsed_ms))
        return 0;

    fill_result(result, "semaphore_pingpong", 2, options->iterations, elapsed_ms,
                (double)options->iterations * 2.0);
    return 1;
}

static DWORD WINAPI event_pingpong_thread(LPVOID param)
{
    pingpong_thread_arg *arg = (pingpong_thread_arg *)param;
    pingpong_ctx *ctx = arg->ctx;
    const uint32_t index = arg->index;
    const uint32_t other = index ^ 1u;
    uint32_t i;

    SetEvent(ctx->ready[index]);
    WaitForSingleObject(ctx->token[index], INFINITE);
    if (index == 0)
        QueryPerformanceCounter(&ctx->start);

    for (i = 0; i < ctx->iterations; i++) {
        SetEvent(ctx->token[other]);
        if (i + 1 < ctx->iterations)
            WaitForSingleObject(ctx->token[index], INFINITE);
    }

    if (index == 1)
        QueryPerformanceCounter(&ctx->end);
    SetEvent(ctx->done[index]);
    return 0;
}

static int run_event_pingpong_once(uint32_t iterations, double *elapsed_ms)
{
    pingpong_ctx ctx;
    pingpong_thread_arg args[2];
    HANDLE threads[2] = {0};
    int ok = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = iterations;
    ctx.ready[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.ready[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.token[0] = CreateEventA(NULL, FALSE, FALSE, NULL);
    ctx.token[1] = CreateEventA(NULL, FALSE, FALSE, NULL);

    if (!ctx.ready[0] || !ctx.ready[1] || !ctx.done[0] || !ctx.done[1] ||
        !ctx.token[0] || !ctx.token[1])
        goto cleanup;

    args[0].ctx = &ctx;
    args[0].index = 0;
    args[1].ctx = &ctx;
    args[1].index = 1;
    threads[0] = CreateThread(NULL, 0, event_pingpong_thread, &args[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, event_pingpong_thread, &args[1], 0, NULL);
    if (!threads[0] || !threads[1])
        goto cleanup;

    if (WaitForMultipleObjects(2, ctx.ready, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;
    SetEvent(ctx.token[0]);
    if (WaitForMultipleObjects(2, ctx.done, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    *elapsed_ms = qpc_elapsed_ms(ctx.start, ctx.end);
    ok = 1;

cleanup:
    if (threads[0]) {
        WaitForSingleObject(threads[0], 1000);
        CloseHandle(threads[0]);
    }
    if (threads[1]) {
        WaitForSingleObject(threads[1], 1000);
        CloseHandle(threads[1]);
    }
    close_handle(&ctx.ready[0]);
    close_handle(&ctx.ready[1]);
    close_handle(&ctx.done[0]);
    close_handle(&ctx.done[1]);
    close_handle(&ctx.token[0]);
    close_handle(&ctx.token[1]);
    return ok;
}

static int bench_event_pingpong(const bench_options *options, bench_result *result)
{
    double elapsed_ms;

    if (!run_event_pingpong_once(options->warmup, &elapsed_ms))
        return 0;
    sleep_briefly();
    if (!run_event_pingpong_once(options->iterations, &elapsed_ms))
        return 0;

    fill_result(result, "event_pingpong", 2, options->iterations, elapsed_ms,
                (double)options->iterations * 2.0);
    return 1;
}

static DWORD WINAPI condvar_pingpong_thread(LPVOID param)
{
    condvar_thread_arg *arg = (condvar_thread_arg *)param;
    condvar_ctx *ctx = arg->ctx;
    const uint32_t index = arg->index;
    uint32_t i;

    AcquireSRWLockExclusive(&ctx->lock);
    ctx->ready_count++;
    if (ctx->ready_count == 2)
        SetEvent(ctx->ready_event);
    while (ctx->ready_count < 3)
        SleepConditionVariableSRW(&ctx->cond, &ctx->lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&ctx->lock);

    if (index == 0)
        QueryPerformanceCounter(&ctx->start);

    for (i = 0; i < ctx->iterations; i++) {
        AcquireSRWLockExclusive(&ctx->lock);
        while (ctx->turn != index)
            SleepConditionVariableSRW(&ctx->cond, &ctx->lock, INFINITE, 0);
        ctx->turn = index ^ 1u;
        WakeConditionVariable(&ctx->cond);
        ReleaseSRWLockExclusive(&ctx->lock);
    }

    if (index == 1)
        QueryPerformanceCounter(&ctx->end);
    return 0;
}

static int run_condvar_pingpong_once(uint32_t iterations, double *elapsed_ms)
{
    condvar_ctx ctx;
    condvar_thread_arg args[2];
    HANDLE threads[2] = {0};
    int ok = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = iterations;
    ctx.turn = 0;
    InitializeSRWLock(&ctx.lock);
    InitializeConditionVariable(&ctx.cond);
    ctx.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ctx.ready_event)
        goto cleanup;

    args[0].ctx = &ctx;
    args[0].index = 0;
    args[1].ctx = &ctx;
    args[1].index = 1;
    threads[0] = CreateThread(NULL, 0, condvar_pingpong_thread, &args[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, condvar_pingpong_thread, &args[1], 0, NULL);
    if (!threads[0] || !threads[1])
        goto cleanup;

    if (WaitForSingleObject(ctx.ready_event, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;
    AcquireSRWLockExclusive(&ctx.lock);
    ctx.ready_count = 3;
    WakeAllConditionVariable(&ctx.cond);
    ReleaseSRWLockExclusive(&ctx.lock);

    if (WaitForMultipleObjects(2, threads, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    *elapsed_ms = qpc_elapsed_ms(ctx.start, ctx.end);
    ok = 1;

cleanup:
    if (threads[0]) {
        WaitForSingleObject(threads[0], 1000);
        CloseHandle(threads[0]);
    }
    if (threads[1]) {
        WaitForSingleObject(threads[1], 1000);
        CloseHandle(threads[1]);
    }
    close_handle(&ctx.ready_event);
    return ok;
}

static int bench_condvar_pingpong(const bench_options *options, bench_result *result)
{
    double elapsed_ms;

    if (!run_condvar_pingpong_once(options->warmup, &elapsed_ms))
        return 0;
    sleep_briefly();
    if (!run_condvar_pingpong_once(options->iterations, &elapsed_ms))
        return 0;

    fill_result(result, "condvar_pingpong", 2, options->iterations, elapsed_ms,
                (double)options->iterations * 2.0);
    return 1;
}

static DWORD WINAPI wait_any_thread(LPVOID param)
{
    wait_any_ctx *ctx = (wait_any_ctx *)param;
    uint32_t i;

    SetEvent(ctx->worker_ready);
    QueryPerformanceCounter(&ctx->start);
    for (i = 0; i < ctx->iterations; i++) {
        DWORD wait_result;

        SetEvent(ctx->event_ready);
        wait_result = WaitForMultipleObjects(WAIT_OBJECT_COUNT, ctx->events, FALSE, INFINITE);
        if (wait_result < WAIT_OBJECT_0 || wait_result >= WAIT_OBJECT_0 + WAIT_OBJECT_COUNT)
            break;
    }
    QueryPerformanceCounter(&ctx->end);
    SetEvent(ctx->done);
    return 0;
}

static int run_wait_any_once(uint32_t iterations, double *elapsed_ms)
{
    wait_any_ctx ctx;
    HANDLE thread = NULL;
    uint32_t i;
    int ok = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = iterations;
    ctx.event_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    ctx.worker_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ctx.event_ready || !ctx.worker_ready || !ctx.done)
        goto cleanup;

    for (i = 0; i < WAIT_OBJECT_COUNT; i++) {
        ctx.events[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!ctx.events[i])
            goto cleanup;
    }

    thread = CreateThread(NULL, 0, wait_any_thread, &ctx, 0, NULL);
    if (!thread)
        goto cleanup;
    if (WaitForSingleObject(ctx.worker_ready, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    for (i = 0; i < iterations; i++) {
        if (WaitForSingleObject(ctx.event_ready, INFINITE) != WAIT_OBJECT_0)
            goto cleanup;
        SetEvent(ctx.events[i % WAIT_OBJECT_COUNT]);
    }

    if (WaitForSingleObject(ctx.done, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    *elapsed_ms = qpc_elapsed_ms(ctx.start, ctx.end);
    ok = 1;

cleanup:
    if (thread) {
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
    }
    close_handle(&ctx.event_ready);
    close_handle(&ctx.worker_ready);
    close_handle(&ctx.done);
    for (i = 0; i < WAIT_OBJECT_COUNT; i++)
        close_handle(&ctx.events[i]);
    return ok;
}

static int bench_wait_any(const bench_options *options, bench_result *result)
{
    double elapsed_ms;

    if (!run_wait_any_once(options->warmup, &elapsed_ms))
        return 0;
    sleep_briefly();
    if (!run_wait_any_once(options->iterations, &elapsed_ms))
        return 0;

    fill_result(result, "wait_any_8", 2, options->iterations, elapsed_ms,
                (double)options->iterations);
    return 1;
}

static DWORD WINAPI wait_all_thread(LPVOID param)
{
    wait_all_ctx *ctx = (wait_all_ctx *)param;
    uint32_t i;

    SetEvent(ctx->worker_ready);
    QueryPerformanceCounter(&ctx->start);
    for (i = 0; i < ctx->iterations; i++) {
        SetEvent(ctx->iteration_ready);
        if (WaitForMultipleObjects(WAIT_OBJECT_COUNT, ctx->events, TRUE, INFINITE) != WAIT_OBJECT_0)
            break;
    }
    QueryPerformanceCounter(&ctx->end);
    SetEvent(ctx->done);
    return 0;
}

static int run_wait_all_once(uint32_t iterations, double *elapsed_ms)
{
    wait_all_ctx ctx;
    HANDLE thread = NULL;
    uint32_t i;
    int ok = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = iterations;
    ctx.iteration_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    ctx.worker_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    ctx.done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ctx.iteration_ready || !ctx.worker_ready || !ctx.done)
        goto cleanup;

    for (i = 0; i < WAIT_OBJECT_COUNT; i++) {
        ctx.events[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!ctx.events[i])
            goto cleanup;
    }

    thread = CreateThread(NULL, 0, wait_all_thread, &ctx, 0, NULL);
    if (!thread)
        goto cleanup;
    if (WaitForSingleObject(ctx.worker_ready, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    for (i = 0; i < iterations; i++) {
        uint32_t j;

        if (WaitForSingleObject(ctx.iteration_ready, INFINITE) != WAIT_OBJECT_0)
            goto cleanup;
        for (j = 0; j < WAIT_OBJECT_COUNT; j++)
            SetEvent(ctx.events[j]);
    }

    if (WaitForSingleObject(ctx.done, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;

    *elapsed_ms = qpc_elapsed_ms(ctx.start, ctx.end);
    ok = 1;

cleanup:
    if (thread) {
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
    }
    close_handle(&ctx.iteration_ready);
    close_handle(&ctx.worker_ready);
    close_handle(&ctx.done);
    for (i = 0; i < WAIT_OBJECT_COUNT; i++)
        close_handle(&ctx.events[i]);
    return ok;
}

static int bench_wait_all(const bench_options *options, bench_result *result)
{
    double elapsed_ms;

    if (!run_wait_all_once(options->warmup, &elapsed_ms))
        return 0;
    sleep_briefly();
    if (!run_wait_all_once(options->iterations, &elapsed_ms))
        return 0;

    fill_result(result, "wait_all_8", 2, options->iterations, elapsed_ms,
                (double)options->iterations);
    return 1;
}

static DWORD WINAPI contended_mutex_thread(LPVOID param)
{
    contended_thread_arg *arg = (contended_thread_arg *)param;
    contended_ctx *ctx = arg->ctx;
    uint32_t i;

    InterlockedIncrement(&ctx->ready_count);
    WaitForSingleObject(ctx->start_event, INFINITE);

    for (i = 0; i < ctx->iterations; i++) {
        WaitForSingleObject(ctx->mutex, INFINITE);
        InterlockedIncrement64(&ctx->counter);
        ReleaseMutex(ctx->mutex);
    }

    return 0;
}

static int run_contended_mutex_once(uint32_t iterations, uint32_t threads_count, double *elapsed_ms)
{
    contended_ctx ctx;
    contended_thread_arg *args = NULL;
    HANDLE *threads = NULL;
    uint32_t i;
    int ok = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.iterations = iterations;
    ctx.threads = threads_count;
    ctx.mutex = CreateMutexA(NULL, FALSE, NULL);
    ctx.start_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ctx.mutex || !ctx.start_event)
        goto cleanup;

    args = (contended_thread_arg *)calloc(threads_count, sizeof(*args));
    threads = (HANDLE *)calloc(threads_count, sizeof(*threads));
    if (!args || !threads)
        goto cleanup;

    for (i = 0; i < threads_count; i++) {
        args[i].ctx = &ctx;
        args[i].index = i;
        threads[i] = CreateThread(NULL, 0, contended_mutex_thread, &args[i], 0, NULL);
        if (!threads[i])
            goto cleanup;
    }

    while ((uint32_t)ctx.ready_count < threads_count)
        Sleep(0);

    QueryPerformanceCounter(&ctx.start);
    SetEvent(ctx.start_event);
    if (WaitForMultipleObjects(threads_count, threads, TRUE, INFINITE) != WAIT_OBJECT_0)
        goto cleanup;
    QueryPerformanceCounter(&ctx.end);

    *elapsed_ms = qpc_elapsed_ms(ctx.start, ctx.end);
    ok = 1;

cleanup:
    if (threads) {
        for (i = 0; i < threads_count; i++) {
            if (threads[i]) {
                WaitForSingleObject(threads[i], 1000);
                CloseHandle(threads[i]);
            }
        }
    }
    free(threads);
    free(args);
    close_handle(&ctx.mutex);
    close_handle(&ctx.start_event);
    return ok;
}

static int bench_contended_mutex(const bench_options *options, bench_result *result)
{
    double elapsed_ms;
    uint32_t threads = clamp_u32(options->threads, 2, MAXIMUM_WAIT_OBJECTS);
    uint32_t per_thread_iterations = clamp_u32(options->iterations / threads, 1, options->iterations);

    if (!run_contended_mutex_once(clamp_u32(options->warmup / threads, 1, options->warmup), threads,
                                  &elapsed_ms))
        return 0;
    sleep_briefly();
    if (!run_contended_mutex_once(per_thread_iterations, threads, &elapsed_ms))
        return 0;

    fill_result(result, "contended_mutex", threads, per_thread_iterations * threads, elapsed_ms,
                (double)per_thread_iterations * (double)threads);
    return 1;
}

static const bench_case g_cases[] = {
    {"mutex_pingpong", bench_mutex_pingpong},
    {"semaphore_pingpong", bench_semaphore_pingpong},
    {"event_pingpong", bench_event_pingpong},
    {"condvar_pingpong", bench_condvar_pingpong},
    {"wait_any_8", bench_wait_any},
    {"wait_all_8", bench_wait_all},
    {"contended_mutex", bench_contended_mutex},
};

static void print_usage(const char *program)
{
    size_t i;

    printf("ntsync benchmark %s\n", BENCH_VERSION);
    printf("Usage: %s [options]\n\n", program);
    printf("Options:\n");
    printf("  --iterations N   Total iterations per case (default: %u)\n", DEFAULT_ITERATIONS);
    printf("  --threads N      Worker threads for contended cases (default: logical CPUs)\n");
    printf("  --warmup N       Warmup iterations before measured run (default: %u)\n", DEFAULT_WARMUP);
    printf("  --case NAME      Run one case only\n");
    printf("  --csv-only       Print only CSV rows\n");
    printf("  --no-pause       Do not wait for Enter before exit\n");
    printf("  --help           Show this help\n\n");
    printf("Cases:\n");
    for (i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++)
        printf("  %s\n", g_cases[i].name);
}

static int parse_args(int argc, char **argv, bench_options *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    options->iterations = DEFAULT_ITERATIONS;
    options->warmup = DEFAULT_WARMUP;
    options->threads = logical_cpu_count();

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0) {
            if (++i >= argc || !parse_u32(argv[i], &options->iterations)) {
                fprintf(stderr, "Invalid --iterations value.\n");
                return 0;
            }
        } else if (strcmp(argv[i], "--threads") == 0) {
            if (++i >= argc || !parse_u32(argv[i], &options->threads)) {
                fprintf(stderr, "Invalid --threads value.\n");
                return 0;
            }
        } else if (strcmp(argv[i], "--warmup") == 0) {
            if (++i >= argc || !parse_u32(argv[i], &options->warmup)) {
                fprintf(stderr, "Invalid --warmup value.\n");
                return 0;
            }
        } else if (strcmp(argv[i], "--case") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing --case value.\n");
                return 0;
            }
            options->case_filter = argv[i];
        } else if (strcmp(argv[i], "--csv-only") == 0) {
            options->csv_only = 1;
            options->no_pause = 1;
        } else if (strcmp(argv[i], "--no-pause") == 0) {
            options->no_pause = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "/?") == 0) {
            options->show_help = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 0;
        }
    }

    options->threads = clamp_u32(options->threads, 1, MAXIMUM_WAIT_OBJECTS);
    options->iterations = clamp_u32(options->iterations, 1, 0xffffffffu);
    options->warmup = clamp_u32(options->warmup, 1, options->iterations);
    return 1;
}

static int case_is_selected(const bench_options *options, const char *name)
{
    return !options->case_filter || equals_ignore_case(options->case_filter, name);
}

static void print_environment(const bench_options *options)
{
    OSVERSIONINFOEXA version;
    DWORD wine_get_version = 0;
    HMODULE ntdll;

    memset(&version, 0, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);
    GetVersionExA((OSVERSIONINFOA *)&version);

    ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version"))
        wine_get_version = 1;

    printf("ntsync benchmark %s\n", BENCH_VERSION);
    printf("Process: x64\n");
    printf("Native architecture: %s\n", processor_architecture());
    printf("Logical CPUs: %u\n", logical_cpu_count());
    printf("Timer frequency: %lld Hz\n", (long long)g_qpc_frequency.QuadPart);
    printf("OS version: %lu.%lu build %lu\n", version.dwMajorVersion, version.dwMinorVersion,
           version.dwBuildNumber);
    printf("Wine detected: %s\n", wine_get_version ? "yes" : "no");
    printf("Defaults: iterations=%u warmup=%u threads=%u\n\n", options->iterations,
           options->warmup, options->threads);
}

static void print_table_header(void)
{
    printf("%-22s %7s %12s %12s %15s %14s %10s\n", "case", "threads", "iterations",
           "elapsed_ms", "ops_per_sec", "avg_ns/op", "score");
    printf("%-22s %7s %12s %12s %15s %14s %10s\n", "----------------------", "-------",
           "------------", "------------", "---------------", "--------------", "----------");
}

static void print_table_row(const bench_result *result)
{
    printf("%-22s %7u %12u %12.3f %15.0f %14.1f %10.3f\n", result->name,
           result->threads, result->iterations, result->elapsed_ms, result->ops_per_sec,
           result->avg_ns_per_op, result->score);
}

static void print_csv_header(void)
{
    printf("case,threads,iterations,elapsed_ms,ops_per_sec,avg_ns_per_op,score\n");
}

static void print_csv_row(const bench_result *result)
{
    printf("%s,%u,%u,%.3f,%.0f,%.1f,%.3f\n", result->name, result->threads,
           result->iterations, result->elapsed_ms, result->ops_per_sec, result->avg_ns_per_op,
           result->score);
}

static void maybe_pause(const bench_options *options)
{
    char buffer[8];

    if (options->no_pause)
        return;
    printf("\nPress Enter to exit...");
    fflush(stdout);
    (void)fgets(buffer, sizeof(buffer), stdin);
}

int main(int argc, char **argv)
{
    bench_options options;
    bench_result results[sizeof(g_cases) / sizeof(g_cases[0])];
    size_t result_count = 0;
    size_t i;
    int selected_count = 0;
    int failed_count = 0;

    if (!QueryPerformanceFrequency(&g_qpc_frequency)) {
        fprintf(stderr, "QueryPerformanceFrequency failed.\n");
        return 1;
    }

    if (!parse_args(argc, argv, &options)) {
        print_usage(argv[0]);
        maybe_pause(&options);
        return 2;
    }

    if (options.show_help) {
        print_usage(argv[0]);
        maybe_pause(&options);
        return 0;
    }

    if (!options.csv_only)
        print_environment(&options);
    else
        print_csv_header();

    if (!options.csv_only)
        print_table_header();

    for (i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++) {
        bench_result result;

        if (!case_is_selected(&options, g_cases[i].name))
            continue;

        selected_count++;
        memset(&result, 0, sizeof(result));
        result.name = g_cases[i].name;

        if (!g_cases[i].run(&options, &result)) {
            failed_count++;
            if (!options.csv_only)
                fprintf(stderr, "case failed: %s\n", g_cases[i].name);
            continue;
        }

        results[result_count++] = result;
        if (options.csv_only)
            print_csv_row(&result);
        else
            print_table_row(&result);
    }

    if (selected_count == 0) {
        fprintf(stderr, "No benchmark case matched '%s'.\n", options.case_filter);
        maybe_pause(&options);
        return 2;
    }

    if (!options.csv_only) {
        printf("\n--- CSV ---\n");
        print_csv_header();
        for (i = 0; i < result_count; i++)
            print_csv_row(&results[i]);
        printf("--- END CSV ---\n\n");
        printf("Higher ops/sec and score are better. Lower avg_ns/op is better.\n");
        printf("Compare runs with the same EXE, machine, Wine version, and parameters before/after ntsync.\n");
    }

    maybe_pause(&options);
    return failed_count ? 1 : 0;
}
