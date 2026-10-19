// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// _audioif - the audio pump's PLATFORM DRIVER, and the hardware layer of the
// live audio path. The other half of the pump, the portable one, lives in the
// DSP repo: the pull loop, the push ring, the event queue, the tap, the
// status words, the fault register and the lock's contract are all `audiodsp`
// and are built into every port it ships on.
//
// What is here is what that half is not allowed to know:
//
//   the thread -- a FreeRTOS task pinned to the core the interpreter is not
//   on, a _beginthreadex CRT thread on Windows, a pthread on unix;
//
//   the mutex the pump lock is made of, recursive and priority-inheriting
//   where the OS has one;
//
//   the clock, the pacing and the two waits;
//
//   the sink and the source -- a file descriptor on a desktop, an I2S channel
//   and an `Input` that reads a microphone on a board.
//
// It binds by defining audiodsp_port_driver(), which overrides the weak
// default in the engine at link time. The hook table is in THIS translation
// unit, beside MP_REGISTER_MODULE, on purpose: the module table holds an
// undefined reference to `_audioif_module`, so this object is pulled into the
// link whether it arrives loose or inside an archive -- which is the failure
// mode weak binding has, and the one that would leave the default winning
// silently. `audiopump.driver()` is the run-time check that says it worked.
//
// The module is private by convention -- the way usbif ships a C `_usbif`
// under a Python package. `audiodev` is the public face.

#include <stdint.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "audiocore/__init__.h"
#include "audiopump/audiopump.h"
#include "shared/audiodsp_port.h"
#include "shared/audiodsp_pump_lock.h"
#include "shared/audiodsp_sample.h"

#include "audiopump_i2s.h"

// AUDIOIF_DRIVER_ESP32 comes from micropython.cmake, not from the IDF: a user
// C module is compiled without ESP_PLATFORM, and the POSIX branch below LINKS
// on esp32 (newlib has pthread.h), so keying off the wrong macro gets you a
// silently unpinned pump with no sink. That trap cost the spike a whole
// firmware once; it is the reason audiopump.driver() exists.
#if defined(AUDIOIF_DRIVER_ESP32) || defined(ESP_PLATFORM)
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define AUDIOIF_DRV_ESP (1)
#define AUDIOIF_DRV_WIN (0)
#define AUDIOIF_DRV_POSIX (0)
#define AUDIOIF_DRV_NONE (0)
#define AUDIOIF_DRV_NAME "esp32"
#elif defined(__EMSCRIPTEN__)
// Not a mistake and not a gap: the WebAssembly build has no threads (no
// -pthread, no SharedArrayBuffer) and no hardware to drive. Emscripten still
// has <pthread.h> and pthread_create still LINKS -- it aborts the whole module
// at run time with "Tried to spawn a new thread, but this is not supported" --
// so a driver here would be the same "compiles, links, silently wrong" trap in
// a new place. This file compiles to nothing, no driver binds, and
// audiopump.driver() says "none", which is exactly true.
#define AUDIOIF_DRV_ESP (0)
#define AUDIOIF_DRV_WIN (0)
#define AUDIOIF_DRV_POSIX (0)
#define AUDIOIF_DRV_NONE (1)
#elif defined(_WIN32)
// A THIRD branch rather than a few #ifdefs inside the POSIX one, because
// nothing in the POSIX branch survives here:
//
//   MinGW-w64's gcc on this box is thread-model=win32, so <pthread.h>
//   compiles and pthread_create does not link. Even where winpthreads is
//   present it is a shim over the same CreateThread this branch calls
//   directly, with a DLL between the pump and the scheduler.
//
//   MicroPython's windows port has no `_thread` module at all, so the pump
//   being a native C thread is not an optimisation here -- it is the only way
//   a second thread exists in this interpreter.
//
// WIN32_LEAN_AND_MEAN keeps <windows.h> from dragging in the RPC and OLE
// headers, whose `small` and `interface` typedefs collide with ordinary C;
// NOMINMAX keeps its min/max macros away from anything that uses those names.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define AUDIOIF_DRV_ESP (0)
#define AUDIOIF_DRV_WIN (1)
#define AUDIOIF_DRV_POSIX (0)
#define AUDIOIF_DRV_NONE (0)
#define AUDIOIF_DRV_NAME "win32"
#else
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#define AUDIOIF_DRV_ESP (0)
#define AUDIOIF_DRV_WIN (0)
#define AUDIOIF_DRV_POSIX (1)
#define AUDIOIF_DRV_NONE (0)
#define AUDIOIF_DRV_NAME "pthread"
#endif

#if !AUDIOIF_DRV_NONE

// --- the clock ------------------------------------------------------------

#if AUDIOIF_DRV_WIN
// QueryPerformanceCounter is the only monotonic microsecond clock Windows
// has: GetTickCount64 is one scheduler tick granular (15.6 ms by default --
// three blocks at 48 kHz/256), and timeGetTime is in winmm, which this box has
// wedged. The frequency is fixed for the life of the boot, so it is read once.
//
// The counter is divided BEFORE it is scaled. `ticks * 1000000` overflows a
// signed 64-bit at 9.2e12 ticks, which on a 10 MHz QPC is ten days of uptime
// -- an ordinary desktop reaches it, and the pump would start reporting
// negative block times on a machine nobody had rebooted.
static LARGE_INTEGER audiopump_qpc_freq;
#endif

static uint64_t audiopump_now_us(void) {
    #if AUDIOIF_DRV_ESP
    return (uint64_t)esp_timer_get_time();
    #elif AUDIOIF_DRV_WIN
    if (audiopump_qpc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&audiopump_qpc_freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t freq = (uint64_t)audiopump_qpc_freq.QuadPart;
    const uint64_t ticks = (uint64_t)now.QuadPart;
    return (ticks / freq) * 1000000ULL + ((ticks % freq) * 1000000ULL) / freq;
    #else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
    #endif
}

// --- the mutex ------------------------------------------------------------
//
// Recursive on purpose: a node's pull legitimately re-enters helpers that
// lock, and a control-path entry legitimately calls another locked helper.
// Recursive means neither has to know about the other. See the contract in
// audiodsp's shared/audiodsp_pump_lock.h -- this is only the machine under it.

#if AUDIOIF_DRV_ESP

// xSemaphoreCreateRecursiveMutex gives priority inheritance, which matters
// here and nowhere else: the pump runs above the interpreter, so an
// interpreter holding the swap has to be lifted to finish it or the pump waits
// on a thread nothing is scheduling.
//
// STATIC, and built under a one-shot compare-exchange. Two reasons, both
// learned rather than guessed:
//
//   The lazy `if (m == NULL) m = create()` this started as is a race between
//   two cores the moment a pump exists.
//
//   xSemaphoreCreateRecursiveMutex() calls malloc. The first taker may be the
//   pump thread, mid-block, and an allocation there is the one thing the
//   contract forbids. The static form allocates nothing.
static SemaphoreHandle_t audiopump_mutex;
static StaticSemaphore_t audiopump_mutex_storage;
static volatile uint32_t audiopump_mutex_state;  // 0 none, 1 building, 2 ready

static void audiopump_lock_ensure(void) {
    if (__atomic_load_n(&audiopump_mutex_state, __ATOMIC_ACQUIRE) == 2) {
        return;
    }
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&audiopump_mutex_state, &expected, 1,
        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        audiopump_mutex =
            xSemaphoreCreateRecursiveMutexStatic(&audiopump_mutex_storage);
        __atomic_store_n(&audiopump_mutex_state, 2, __ATOMIC_RELEASE);
        return;
    }
    // Somebody else is building it. Microseconds at most, once per boot.
    while (__atomic_load_n(&audiopump_mutex_state, __ATOMIC_ACQUIRE) != 2) {
        portYIELD();
    }
}

static void audiopump_lock_take(void) {
    audiopump_lock_ensure();
    if (audiopump_mutex != NULL) {
        xSemaphoreTakeRecursive(audiopump_mutex, portMAX_DELAY);
    }
}

static void audiopump_lock_give(void) {
    if (audiopump_mutex != NULL) {
        xSemaphoreGiveRecursive(audiopump_mutex);
    }
}

#elif AUDIOIF_DRV_WIN

// A CRITICAL_SECTION is recursive by definition, and InitializeCriticalSection
// on a static is safe to do under a one-shot interlocked flag.
static CRITICAL_SECTION audiopump_cs;
static LONG audiopump_cs_state;  // 0 none, 1 building, 2 ready

static void audiopump_lock_ensure(void) {
    if (InterlockedCompareExchange(&audiopump_cs_state, 1, 0) == 0) {
        InitializeCriticalSection(&audiopump_cs);
        InterlockedExchange(&audiopump_cs_state, 2);
    }
    while (InterlockedCompareExchange(&audiopump_cs_state, 2, 2) != 2) {
        Sleep(0);
    }
}

static void audiopump_lock_take(void) {
    audiopump_lock_ensure();
    EnterCriticalSection(&audiopump_cs);
}

static void audiopump_lock_give(void) {
    LeaveCriticalSection(&audiopump_cs);
}

#else

// PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP exists on glibc and nowhere portably,
// so the attribute route and a pthread_once. It runs exactly once and never on
// the audio path after that.
static pthread_mutex_t audiopump_mutex;
static pthread_once_t audiopump_mutex_once = PTHREAD_ONCE_INIT;

static void audiopump_lock_make(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&audiopump_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void audiopump_lock_take(void) {
    pthread_once(&audiopump_mutex_once, audiopump_lock_make);
    pthread_mutex_lock(&audiopump_mutex);
}

static void audiopump_lock_give(void) {
    pthread_mutex_unlock(&audiopump_mutex);
}

#endif

// --- the two waits --------------------------------------------------------
//
// `park_spin` is the PUMP's own wait, and the engine has two callers for it:
// the park at a block boundary, and the wait for room in the output ring that
// makes a desktop pump pace itself instead of dropping audio. Its contract
// (shared/audiodsp_port.h) is that it returns when thread_wake() is called, and
// that it may return early.
//
// Both desktop branches used to satisfy that contract by NOT WAITING AT ALL --
// sched_yield() and SwitchToThread() return immediately, so the engine's
// `while (park_req) park_spin(...)` was a tight loop burning a whole core for
// as long as the pump was parked. That was invisible while the only parks were
// microseconds long inside produce(); it is not invisible once a pump waits on
// a ring for milliseconds at a time, and it is the reason an idle pump thread
// could never cost ~0.
//
// So both are real blocking waits now, and the wake is real. The one property
// each mechanism has to have is that a wake which arrives BEFORE the wait is
// RETAINED -- otherwise a drain that runs between the engine's "is there room"
// check and its sleep is a lost wakeup, and the pump stalls until the ceiling.
// A FreeRTOS task notification counts, a Win32 auto-reset event latches one
// signal, and the POSIX branch keeps a counter under the condvar's mutex.

#if AUDIOIF_DRV_WIN
// Auto-reset, so a SetEvent with nobody waiting is remembered and consumed by
// the next wait. Created on the INTERPRETER thread in thread_start, before the
// pump exists, so there is no lazy-init race between waker and waiter; closed
// in teardown after the thread has been released.
static HANDLE audiopump_wake_event;
#elif AUDIOIF_DRV_POSIX
// A condvar on CLOCK_MONOTONIC where the platform has it: a timed wait on
// CLOCK_REALTIME jumps when the system clock is stepped, and an NTP correction
// would turn a 20 ms ceiling into a stall or a spin. `signals` is what makes a
// wake that arrives first survive until the wait.
//
// Statically initialised, so nothing on the audio path allocates and there is
// nothing to create twice. The pthread_once only upgrades the clock.
static pthread_mutex_t audiopump_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audiopump_wake_cond = PTHREAD_COND_INITIALIZER;
static unsigned audiopump_wake_signals;
static bool audiopump_wake_monotonic;
static pthread_once_t audiopump_wake_once = PTHREAD_ONCE_INIT;

static void audiopump_wake_make(void) {
    #ifdef CLOCK_MONOTONIC
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        return;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0
        && pthread_cond_init(&audiopump_wake_cond, &attr) == 0) {
        audiopump_wake_monotonic = true;
    }
    pthread_condattr_destroy(&attr);
    #endif
}
#endif

static void audiopump_park_spin(uint32_t max_us) {
    #if AUDIOIF_DRV_ESP
    // Not vTaskDelay(1): this port's tick is 100 Hz, so the shortest delay is
    // 10 ms and a parked pump would underrun whatever it is feeding. A
    // direct-to-task notify wakes on the give, in a context switch.
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(max_us / 1000));
    #elif AUDIOIF_DRV_WIN
    if (audiopump_wake_event == NULL) {
        // No event, so no wait that a wake could end. Yield rather than sleep:
        // this is the old behaviour and it is what a pull() on the
        // interpreter's own thread would want if it somehow got here.
        (void)max_us;
        SwitchToThread();
        return;
    }
    DWORD ms = (DWORD)(max_us / 1000u);
    if (ms == 0) {
        ms = 1;
    }
    (void)WaitForSingleObject(audiopump_wake_event, ms);
    #else
    pthread_once(&audiopump_wake_once, audiopump_wake_make);
    pthread_mutex_lock(&audiopump_wake_mutex);
    if (audiopump_wake_signals == 0) {
        struct timespec ts;
        clock_gettime(audiopump_wake_monotonic ? CLOCK_MONOTONIC
            : CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(max_us / 1000000u);
        ts.tv_nsec += (long)((max_us % 1000000u) * 1000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec += 1;
        }
        (void)pthread_cond_timedwait(&audiopump_wake_cond,
            &audiopump_wake_mutex, &ts);
    }
    // Consumed whether it was a signal or a timeout: the callers are all
    // `while (condition) park_spin()`, so an extra turn round a loop is free
    // and a signal left behind would make the NEXT wait return at once.
    audiopump_wake_signals = 0;
    pthread_mutex_unlock(&audiopump_wake_mutex);
    #endif
}

#if AUDIOIF_DRV_WIN
// Hold for `wait_us`, accurately. Sleep()'s floor is the scheduler tick, so a
// pump paced with it would stutter by three blocks at a time; a waitable timer
// created with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is 100 ns granular on
// Windows 10 1803 and later. Where the flag is refused -- an older build, or a
// policy that disallows it -- the timer is created without it and the last
// millisecond is spun out against QPC, which is what nanosleep would have
// given for free.
//
// ONE timer, and therefore one thread may wait on it. CreateWaitableTimerExW
// without CREATE_WAITABLE_TIMER_MANUAL_RESET makes a SYNCHRONIZATION timer: a
// signal releases exactly one waiter, and SetWaitableTimer from a second
// thread cancels whatever the first was waiting for. Both of this hook's
// callers are real -- the pump's pace, and the 10 ms teardown wait on the
// interpreter thread -- and they overlap at every shutdown of a paced pump.
// Two threads on one auto-reset timer is one of them waiting on INFINITE for
// a signal that has already been taken: a hang, not a slow teardown.
//
// So the timer belongs to the thread that made it, and anybody else gets
// Sleep plus the same closing spin. The only other caller waits 10 ms at a
// time during teardown, where a scheduler tick of slop is invisible.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x00000002)
#endif
static HANDLE audiopump_pace_timer;
static DWORD audiopump_pace_owner;
static bool audiopump_pace_coarse;
#endif

static void audiopump_sleep_us(uint64_t wait_us) {
    #if AUDIOIF_DRV_ESP
    // One of this hook's callers is a finaliser, so it must not raise: no
    // mp_hal_delay_ms, which ends in mp_handle_pending. A whole tick or more
    // goes to the scheduler; anything shorter is a ROM busy-wait, because the
    // shortest delay this port can schedule is 10 ms.
    const uint32_t tick_us = (uint32_t)portTICK_PERIOD_MS * 1000u;
    if (wait_us >= tick_us) {
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000ULL));
    } else if (wait_us) {
        esp_rom_delay_us((uint32_t)wait_us);
    }
    #elif AUDIOIF_DRV_WIN
    if (audiopump_pace_timer == NULL) {
        audiopump_pace_timer = CreateWaitableTimerExW(NULL, NULL,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (audiopump_pace_timer == NULL) {
            audiopump_pace_timer = CreateWaitableTimerExW(NULL, NULL, 0,
                TIMER_ALL_ACCESS);
            audiopump_pace_coarse = (audiopump_pace_timer != NULL);
        }
        audiopump_pace_owner = (audiopump_pace_timer != NULL)
            ? GetCurrentThreadId() : 0;
    }
    // Whoever made it waits on it; see the note above the handle.
    HANDLE timer = (audiopump_pace_owner == GetCurrentThreadId())
        ? audiopump_pace_timer : NULL;
    const uint64_t deadline = audiopump_now_us() + wait_us;
    // A coarse timer rounds UP to the tick, so leave it a millisecond of
    // headroom and spin the rest. A high-resolution one gets the lot.
    const uint64_t sleep_us = audiopump_pace_coarse
        ? (wait_us > 1000 ? wait_us - 1000 : 0) : wait_us;
    if (timer != NULL && sleep_us) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(sleep_us * 10ULL);   // 100 ns, relative
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        }
    } else if (timer == NULL && wait_us > 2000) {
        // No timer at all: do not burn a core for a whole block. Sleep the
        // whole milliseconds bar one and let the spin below close the gap.
        Sleep((DWORD)((wait_us - 1000) / 1000ULL));
    }
    while (audiopump_now_us() < deadline) {
        YieldProcessor();
    }
    #else
    struct timespec ts;
    ts.tv_sec = (time_t)(wait_us / 1000000ULL);
    ts.tv_nsec = (long)((wait_us % 1000000ULL) * 1000ULL);
    nanosleep(&ts, NULL);
    #endif
}

// --- the thread -----------------------------------------------------------

static void (*audiopump_entry_fn)(void *);
static void *audiopump_entry_arg;
static volatile bool audiopump_thread_live;

#if AUDIOIF_DRV_ESP
static TaskHandle_t audiopump_task_handle;
static bool audiopump_task_psram;
static int audiopump_task_core;

static void audiopump_task(void *arg) {
    (void)arg;
    audiopump_entry_fn(audiopump_entry_arg);
    // Never self-delete. A WithCaps task must be freed with
    // vTaskDeleteWithCaps, and IDF's own note says to do that from another
    // task's context; thread_release() does it. Suspending here also means the
    // stack high-water mark is still readable.
    for (;;) {
        vTaskSuspend(NULL);
    }
}
#elif AUDIOIF_DRV_WIN
// _beginthreadex, not CreateThread. The loop's sink write is a CRT call and a
// CRT thread wants the per-thread state _beginthreadex installs; and the
// handle it returns belongs to the caller, so release closes it. A
// CreateThread handle dropped on the floor would be one handle leaked per
// spawn, which in a player that stops and starts a note at a time is a leak
// per note.
//
// No priority is raised. On a board the pump runs above the interpreter
// because one core has to be given up; on a desktop there are cores to spare,
// and a pump at ABOVE_NORMAL would starve the very interpreter that has to
// reach shutdown().
static HANDLE audiopump_thread;

static unsigned __stdcall audiopump_trampoline(void *arg) {
    (void)arg;
    audiopump_entry_fn(audiopump_entry_arg);
    return 0;
}
#else
static pthread_t audiopump_thread;

static void *audiopump_trampoline(void *arg) {
    (void)arg;
    audiopump_entry_fn(audiopump_entry_arg);
    return NULL;
}
#endif

// --- the watchdog ---------------------------------------------------------
//
// Off unless the build asks for it (-DAUDIODSP_PUMP_LOCK_LEDGER=1). A lock that
// never comes back is invisible from outside the process: the run stops
// printing and that is all anybody learns. This thread watches the lock's
// take/give counters, and when nothing has moved for a few seconds it says who
// owns the lock, how deep, who is queued behind them, where the pump loop is,
// and -- the question "one core is busy" really asks -- which thread has been
// burning CPU while nothing happened. Then it takes the process down, so a
// campaign of runs does not need a timeout per run to make progress.
#if AUDIODSP_PUMP_LOCK_LEDGER && AUDIOIF_DRV_WIN

#include <stdio.h>

static HANDLE audiopump_wd_thread;
static HANDLE audiopump_wd_main;        // a real handle, not the pseudo one
static DWORD audiopump_wd_main_tid;
static volatile LONG audiopump_wd_quit;

static uint64_t audiopump_wd_cpu_us(HANDLE thread) {
    FILETIME create, exit, kernel, user;
    if (thread == NULL || !GetThreadTimes(thread, &create, &exit, &kernel,
        &user)) {
        return 0;
    }
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) / 10ULL;   // 100 ns units -> us
}

static const char *audiopump_wd_site(uint8_t site) {
    switch (site) {
        case 0: return "ctrl";
        case 1: return "pump";
        default: return "nest";
    }
}

static const char *audiopump_wd_phase(uint32_t phase) {
    switch (phase) {
        case AUDIODSP_PUMP_PHASE_TOP: return "top";
        case AUDIODSP_PUMP_PHASE_RING_WAIT: return "ring-wait";
        case AUDIODSP_PUMP_PHASE_PARK: return "park";
        case AUDIODSP_PUMP_PHASE_LOCK: return "lock-acquire";
        case AUDIODSP_PUMP_PHASE_PULL: return "pull";
        case AUDIODSP_PUMP_PHASE_DIGEST: return "digest";
        case AUDIODSP_PUMP_PHASE_SINK: return "sink";
        case AUDIODSP_PUMP_PHASE_PACE: return "pace";
        case AUDIODSP_PUMP_PHASE_RESET: return "reset";
        case AUDIODSP_PUMP_PHASE_END: return "end";
        default: return "idle";
    }
}

// A backtrace with no debugger on the box. Suspend the thread, read RIP out of
// its context, then sweep its stack for words that point into this image --
// which is every return address on it, plus some noise. Resolve the offsets
// afterwards with `addr2line -e micropython.exe`. Crude, and it is the
// difference between "a thread is stuck" and knowing in which function.
static void audiopump_wd_where(const char *who, HANDLE thread) {
    if (thread == NULL) {
        return;
    }
    const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(thread) == (DWORD)-1) {
        return;
    }
    if (GetThreadContext(thread, &ctx)) {
        fprintf(stderr, "%s: base=%p rip=%p", who, (void *)base,
            (void *)(uintptr_t)ctx.Rip);
        if ((uintptr_t)ctx.Rip - base < 0x1000000) {
            fprintf(stderr, " (+0x%llx)",
                (unsigned long long)((uintptr_t)ctx.Rip - base));
        }
        fprintf(stderr, "\n%s: stack", who);
        // Bounded by what is actually mapped: a suspended thread's stack is
        // committed only as far as it has grown, and walking off the top of it
        // is an access violation that eats the dump it was printing.
        MEMORY_BASIC_INFORMATION mbi;
        const uintptr_t sp = (uintptr_t)ctx.Rsp;
        if (VirtualQuery((LPCVOID)sp, &mbi, sizeof(mbi)) == sizeof(mbi)
            && mbi.State == MEM_COMMIT) {
            const uintptr_t top = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            unsigned shown = 0;
            for (uintptr_t at = sp; at + sizeof(uintptr_t) <= top
                 && shown < 24; at += sizeof(uintptr_t)) {
                const uintptr_t w = *(const uintptr_t *)at;
                if (w - base < 0x1000000) {
                    fprintf(stderr, " +0x%llx",
                        (unsigned long long)(w - base));
                    shown++;
                }
            }
        }
        fprintf(stderr, "\n");
    }
    ResumeThread(thread);
}

static void audiopump_wd_dump(const char *why, uint64_t main_us,
    uint64_t pump_us) {
    audiodsp_pump_lock_ledger_t led;
    audiodsp_pump_lock_ledger_read(&led);
    // Unbuffered from here: everything below is being printed because the
    // process is about to be taken down, and a buffered dump is no dump.
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "\n=== LEDGER (%s) ===\n", why);
    fprintf(stderr, "main tid=%lu cpu=%llu us   pump tid=%lu cpu=%llu us\n",
        (unsigned long)audiopump_wd_main_tid, (unsigned long long)main_us,
        (unsigned long)(audiopump_thread != NULL
            ? GetThreadId(audiopump_thread) : 0),
        (unsigned long long)pump_us);
    fprintf(stderr, "owner=%lu depth=%ld waiters=%ld takes=%llu gives=%llu "
        "phase=%s\n", (unsigned long)led.owner, (long)led.depth,
        (long)led.waiters, (unsigned long long)led.takes,
        (unsigned long long)led.gives, audiopump_wd_phase(led.phase));
    if (led.want_us) {
        fprintf(stderr, "WAITING: tid=%lu site=%s for %llu us\n",
            (unsigned long)led.want_tid, audiopump_wd_site(led.want_site),
            (unsigned long long)(audiopump_now_us() - led.want_us));
    }
    if (led.bad_gives) {
        const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        fprintf(stderr, "UNBALANCED RELEASE: %llu of them; first from tid=%lu "
            "site=%s caller=+0x%llx\n", (unsigned long long)led.bad_gives,
            (unsigned long)led.bad_tid, audiopump_wd_site(led.bad_site),
            (unsigned long long)((uintptr_t)led.bad_ra - base));
    }
    // The CRITICAL_SECTION's own fields. LockCount is -1 when the section is
    // free; each waiter takes it further negative. RecursionCount and
    // OwningThread are the pair that says who is inside and how deep. A
    // section with no owner, a zero recursion count and waiters queued behind
    // it is a section nobody is going to wake.
    fprintf(stderr, "cs: LockCount=%ld RecursionCount=%ld OwningThread=%lu "
        "LockSemaphore=%p SpinCount=%llu state=%ld\n",
        (long)audiopump_cs.LockCount, (long)audiopump_cs.RecursionCount,
        (unsigned long)(uintptr_t)audiopump_cs.OwningThread,
        (void *)audiopump_cs.LockSemaphore,
        (unsigned long long)audiopump_cs.SpinCount,
        (long)audiopump_cs_state);
    fprintf(stderr, "pump: blocks=%llu parked=%llu error=%llu "
        "ring_waits=%llu fault=%llu\n",
        (unsigned long long)audiopump_c_status(0),    // STATUS_BLOCKS
        (unsigned long long)audiopump_c_status(7),    // STATUS_PARKED
        (unsigned long long)audiopump_c_status(5),    // STATUS_ERROR
        (unsigned long long)audiopump_c_status(32),   // STATUS_RING_WAITS
        (unsigned long long)audiopump_c_status(24));  // STATUS_FAULT
    audiopump_wd_where("main", audiopump_wd_main);
    audiopump_wd_where("pump", audiopump_thread);
    const uint32_t n = AUDIODSP_PUMP_LOCK_LEDGER_SLOTS;
    const uint32_t start = led.next > n ? led.next - n : 0;
    for (uint32_t i = start; i < led.next; i++) {
        const audiodsp_pump_lock_event_t *e = &led.events[i % n];
        static const char *what[] = { "want", "got ", "gave" };
        fprintf(stderr, "  %10llu us tid=%-6lu %s %s depth=%-4d waiters=%-3d "
            "from +0x%llx\n",
            (unsigned long long)e->us, (unsigned long)e->tid,
            audiopump_wd_site(e->site),
            what[e->what < 3 ? e->what : 0], (int)e->depth, (int)e->waiters,
            (unsigned long long)((uintptr_t)e->ra
                - (uintptr_t)GetModuleHandleW(NULL)));
    }
    fflush(stderr);
}

static unsigned __stdcall audiopump_wd_loop(void *arg) {
    (void)arg;
    audiodsp_pump_lock_ledger_t led;
    audiodsp_pump_lock_ledger_read(&led);
    uint64_t last = led.takes + led.gives;
    uint64_t last_main = audiopump_wd_cpu_us(audiopump_wd_main);
    uint64_t last_pump = audiopump_wd_cpu_us(audiopump_thread);
    unsigned still = 0;
    while (!audiopump_wd_quit) {
        Sleep(200);
        audiodsp_pump_lock_ledger_read(&led);
        // TWO stalls, and only the second one is the interesting one. A lock
        // whose counters have stopped is a lock nobody is using; a lock whose
        // counters are RACING while one thread has been queued behind them for
        // seconds is the hang this exists to name.
        if (led.bad_gives) {
            audiopump_wd_dump("a release with nothing held",
                audiopump_wd_cpu_us(audiopump_wd_main) - last_main,
                audiopump_wd_cpu_us(audiopump_thread) - last_pump);
            _exit(98);
        }
        const uint64_t stuck_us = led.want_us
            ? audiopump_now_us() - led.want_us : 0;
        if (stuck_us == 0) {
            // Nobody is queued, so this is the baseline the CPU figures in the
            // dump are measured against.
            last_main = audiopump_wd_cpu_us(audiopump_wd_main);
            last_pump = audiopump_wd_cpu_us(audiopump_thread);
        }
        if (stuck_us > 3000000ULL) {
            audiopump_wd_dump("a waiter has been queued for 3 s",
                audiopump_wd_cpu_us(audiopump_wd_main) - last_main,
                audiopump_wd_cpu_us(audiopump_thread) - last_pump);
            _exit(97);
        }
        const uint64_t now = led.takes + led.gives;
        if (now != last) {
            last = now;
            still = 0;
            last_main = audiopump_wd_cpu_us(audiopump_wd_main);
            last_pump = audiopump_wd_cpu_us(audiopump_thread);
            continue;
        }
        if (++still < 15) {         // three seconds of nothing
            continue;
        }
        audiopump_wd_dump("the lock has not moved for 3 s",
            audiopump_wd_cpu_us(audiopump_wd_main) - last_main,
            audiopump_wd_cpu_us(audiopump_thread) - last_pump);
        _exit(97);
    }
    return 0;
}

static void audiopump_wd_start(void) {
    if (audiopump_wd_thread != NULL) {
        return;
    }
    audiopump_wd_main_tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
        GetCurrentProcess(), &audiopump_wd_main, 0, FALSE,
        DUPLICATE_SAME_ACCESS);
    audiopump_wd_quit = 0;
    audiopump_wd_thread = (HANDLE)_beginthreadex(NULL, 0, audiopump_wd_loop,
        NULL, 0, NULL);
}

static void audiopump_wd_stop(void) {
    if (audiopump_wd_thread == NULL) {
        return;
    }
    audiopump_wd_quit = 1;
    WaitForSingleObject(audiopump_wd_thread, 2000);
    CloseHandle(audiopump_wd_thread);
    audiopump_wd_thread = NULL;
    if (audiopump_wd_main != NULL) {
        CloseHandle(audiopump_wd_main);
        audiopump_wd_main = NULL;
    }
}
#else
#define audiopump_wd_start() ((void)0)
#define audiopump_wd_stop() ((void)0)
#endif

static bool audiopump_thread_start(void (*entry)(void *), void *arg,
    const audiodsp_port_thread_cfg_t *cfg, int *where) {
    audiopump_entry_fn = entry;
    audiopump_entry_arg = arg;
    #if AUDIOIF_DRV_ESP
    int core = cfg->core;
    if (core < 0) {
        // The core the interpreter is NOT on. MP_TASK_COREID is 1 on every
        // dual-core esp32 this workspace builds for.
        core = (xPortGetCoreID() == 0) ? 1 : 0;
    }
    BaseType_t ok;
    audiopump_task_psram = cfg->psram;
    if (cfg->psram) {
        ok = xTaskCreatePinnedToCoreWithCaps(audiopump_task, "audiopump",
            (configSTACK_DEPTH_TYPE)cfg->stack, NULL,
            (UBaseType_t)cfg->prio, &audiopump_task_handle,
            (BaseType_t)core, MALLOC_CAP_SPIRAM);
    } else {
        ok = xTaskCreatePinnedToCore(audiopump_task, "audiopump",
            (uint32_t)cfg->stack, NULL, (UBaseType_t)cfg->prio,
            &audiopump_task_handle, (BaseType_t)core);
    }
    if (ok != pdPASS) {
        audiopump_task_handle = NULL;
        return false;
    }
    audiopump_task_core = core;
    *where = core;
    #elif AUDIOIF_DRV_WIN
    (void)cfg;
    // Before the thread, on the interpreter's thread: the pump waits on this
    // handle and the interpreter signals it, so it must exist before either of
    // them can look at it. Auto-reset and unsignalled.
    if (audiopump_wake_event == NULL) {
        audiopump_wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    audiopump_thread = (HANDLE)_beginthreadex(NULL, 0, audiopump_trampoline,
        NULL, 0, NULL);
    if (audiopump_thread == NULL) {
        return false;
    }
    *where = -1;
    #else
    (void)cfg;
    if (pthread_create(&audiopump_thread, NULL, audiopump_trampoline,
        NULL) != 0) {
        return false;
    }
    *where = -1;
    #endif
    audiopump_thread_live = true;
    audiopump_wd_start();
    return true;
}

// The other end of park_spin. Called from the interpreter thread by stop(),
// unpark(), park(), the teardown -- and, every tick, by the output ring's
// drain when it has given the pump room to carry on.
static void audiopump_thread_wake(void) {
    #if AUDIOIF_DRV_ESP
    if (audiopump_task_handle != NULL) {
        xTaskNotifyGive(audiopump_task_handle);
    }
    #elif AUDIOIF_DRV_WIN
    if (audiopump_wake_event != NULL) {
        SetEvent(audiopump_wake_event);
    }
    #else
    pthread_mutex_lock(&audiopump_wake_mutex);
    audiopump_wake_signals = 1;
    // Signal INSIDE the mutex. Outside it is the classic lost wakeup: the
    // waiter can have tested the counter, not yet be on the queue, and miss
    // the signal entirely -- which here costs a 20 ms stall per drain and
    // reads as a pump that is mysteriously slow.
    pthread_cond_signal(&audiopump_wake_cond);
    pthread_mutex_unlock(&audiopump_wake_mutex);
    #endif
}

static void audiopump_thread_release(void) {
    if (!audiopump_thread_live) {
        return;
    }
    audiopump_thread_live = false;
    audiopump_wd_stop();
    #if AUDIOIF_DRV_ESP
    TaskHandle_t handle = audiopump_task_handle;
    audiopump_task_handle = NULL;
    if (handle != NULL) {
        if (audiopump_task_psram) {
            // A WithCaps task must be freed with the matching call, and IDF's
            // own note says to do it from another task's context -- which this
            // is. Self-deleting one aborts.
            vTaskDeleteWithCaps(handle);
        } else {
            vTaskDelete(handle);
        }
    }
    #elif AUDIOIF_DRV_WIN
    if (audiopump_thread != NULL) {
        WaitForSingleObject(audiopump_thread, INFINITE);
        CloseHandle(audiopump_thread);
        audiopump_thread = NULL;
    }
    #else
    pthread_join(audiopump_thread, NULL);
    #endif
}

static uintptr_t audiopump_self_id(void) {
    #if AUDIOIF_DRV_ESP
    return (uintptr_t)xTaskGetCurrentTaskHandle();
    #elif AUDIOIF_DRV_WIN
    return (uintptr_t)GetCurrentThreadId();
    #else
    return (uintptr_t)pthread_self();
    #endif
}

static uint64_t audiopump_status_tid(void) {
    #if AUDIOIF_DRV_ESP
    // The core, not the handle: on a board that is the number anybody reading
    // the status block wants, and it is what the pin proof asserts against.
    return (uint64_t)xPortGetCoreID();
    #else
    return (uint64_t)audiopump_self_id();
    #endif
}

static uint32_t audiopump_stack_free(void) {
    #if AUDIOIF_DRV_ESP
    return (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    #else
    return 0;
    #endif
}

// --- the sink -------------------------------------------------------------

#if AUDIOIF_DRV_ESP

static i2s_chan_handle_t audiopump_i2s_tx;
// The other half of the same clock tree. i2s_new_channel(&cfg, &tx, &rx) fills
// both handles from one port, so BCLK, WS and MCLK are generated once and the
// capture is frame-aligned with the playback by construction. That is what
// machine.I2S cannot do on this board -- it opens one direction per port
// object, which is why board_peripherals runs its AudioSession with
// duplex=False.
static i2s_chan_handle_t audiopump_i2s_rx;
// Written from the driver's ISR, read by the interpreter: bytes the DMA has
// clocked out, whether or not anything was written into them (with
// auto_clear on, a starved descriptor still fires on_sent).
static volatile uint64_t audiopump_dma_bytes;
static volatile uint64_t audiopump_rx_bytes;
// Bytes `i2s_channel_write` took off us since `play()` (the reset sets it to
// the DMA's count). A diagnostic beside the DMA's count, not the underrun
// count: their difference is the ring's fill level plus any silence, and the
// fill level moves on its own -- see "the underrun count" below.
static volatile uint64_t audiopump_sink_bytes;
// Bytes the DMA holds when full, as audiopump_i2s_open computes it.
static volatile uint32_t audiopump_dma_depth;
// Bytes of audio one second of wire carries: rate x frame.
static volatile uint32_t audiopump_byte_rate;
// Bytes of one frame on the wire.
static volatile uint32_t audiopump_frame_bytes;

// --- the underrun count ------------------------------------------------------
//
// Two witnesses, both taken in the DMA's own interrupt, and both events rather
// than a difference of two running totals. The difference this replaces --
// "where the wire is" minus "what the pump has fed" -- read the DMA ring's
// fill level as well as any silence: it moved by a descriptor whenever the
// pump's write landed, went DOWN as the ring refilled, and kept a start-up
// offset for the whole performance. On the P4 a clean playback read thousands
// of bytes and a `gc.collect()` made the number fall (audioif#26). An event
// count cannot fall and reads zero until something actually goes quiet.
//
//   The ring runs dry. The driver keeps every buffer the DMA has finished
//   with in a queue for the writer; it is full only when every buffer but
//   the one just finished is free and zeroed, so the DMA's next descriptor
//   carries no audio. The driver reports exactly that moment as
//   `on_send_q_ovf`, once per descriptor, for as long as nothing is written.
//   A healthy writer is blocked waiting for that queue, so it never fills.
//
//   The DMA stops. A flash erase on the ESP32-S3 suspends the cache that the
//   PSRAM descriptors and audio sit behind, and the DMA itself halts: no
//   descriptor finishes, nothing overflows, and the first witness sees
//   nothing (measured on the T-Embed: 1.14 s of audio lost to a 32 kB write
//   with the DMA never behind the pump). The interval between two finished
//   descriptors sees it. One descriptor lasts `audiopump_desc_us`; a gap of
//   more than twice that is charged, less the one descriptor it should have
//   been. Twice is far outside interrupt jitter and any clock divider's
//   error, so a clean run charges nothing.
//
// Counting is off until the pump's first write after `play()` or `resume()`:
// before that, the zeros the channel clocks are the start-up latency or a
// pause somebody asked for, not starvation.
static volatile uint32_t audiopump_desc_bytes;     // one descriptor, in bytes
static volatile uint32_t audiopump_desc_us;        // ... and in microseconds
static volatile uint32_t audiopump_starved_descs;  // dry descriptors counted
static volatile uint64_t audiopump_halted_us;      // DMA gaps beyond one descriptor
static volatile int64_t audiopump_last_sent_us;    // last descriptor finished
static volatile bool audiopump_starved_live;       // counting now
static volatile bool audiopump_starved_pending;    // count from the next write

// Who holds the open channel (AUDIOPUMP_I2S_HOLD_*), and what it was opened
// with. A second holder joins only if it asks for the same wire.
static uint8_t audiopump_i2s_holds;
static audiopump_i2s_cfg_t audiopump_i2s_live;

// --- capture: the RX half, handed to the interpreter ------------------------
//
// The capture half of the channel pair, as PCM Python can read. `Input` above
// it is an audiosample the PUMP pulls on its own task; a recorder on the
// interpreter thread has no way to reach that. This is the other way out: the
// RX DMA's interrupt copies every finished descriptor into a ring, and
// `rx_read()` empties it.
//
// Why the interrupt, and not a bigger DMA. The channel pair is allocated with
// one config, so the RX DMA is exactly as deep as TX's -- six descriptors of
// 5 ms, which is the output's latency floor and is chosen for the output. A
// recorder that reads every 50 ms from Python would lose most of what that
// holds. The ring decouples the two: TX keeps its 30 ms, RX gets as much
// history as the reader asks for.
//
// It takes one slot, the other, their average, or both, because a codec with
// two microphones on one wire (the ES7210 puts MIC1 left and MIC2 right) and a
// mono recorder want a choice the hardware does not make. The conversion is
// done here, in the interrupt, so the ring holds what the reader will get.
//
// Single producer (the interrupt) and single consumer (the interpreter), so
// the ring itself needs no lock. The spinlock below is only for taking the
// ring away while an interrupt on the other core might be inside it.
#define AUDIOPUMP_TAKE_LEFT  (0)
#define AUDIOPUMP_TAKE_RIGHT (1)
#define AUDIOPUMP_TAKE_MIX   (2)
#define AUDIOPUMP_TAKE_BOTH  (3)

typedef struct {
    uint8_t *buf;
    uint32_t len;               // bytes, a whole number of output frames
    uint32_t wpos;              // the interrupt's
    uint32_t rpos;              // the interpreter's
    volatile uint32_t written;  // bytes ever written; differences are exact
    volatile uint32_t taken;    // bytes ever read
    volatile uint32_t frames;   // frames that reached the ring
    volatile uint32_t dropped;  // frames the ring had no room for
    uint8_t take;
    uint8_t out_frame;          // 2 for one channel out, 4 for two
    volatile bool armed;
} audiopump_cap_t;

static audiopump_cap_t audiopump_cap;
static portMUX_TYPE audiopump_cap_mux = portMUX_INITIALIZER_UNLOCKED;

// In the interrupt. `in` is the finished descriptor: signed 16-bit stereo,
// because the channel is opened stereo whenever there is an RX half to read.
// A full ring keeps what it has and counts the rest, so a slow reader loses
// the newest audio rather than corrupting the oldest.
static IRAM_ATTR void audiopump_cap_push(const int16_t *in, uint32_t frames) {
    audiopump_cap_t *c = &audiopump_cap;
    const uint32_t taken = __atomic_load_n(&c->taken, __ATOMIC_ACQUIRE);
    const uint32_t room = (c->len - (c->written - taken)) / c->out_frame;
    const uint32_t n = frames < room ? frames : room;
    uint8_t *buf = c->buf;
    uint32_t pos = c->wpos;
    for (uint32_t f = 0; f < n; f++) {
        const int16_t l = in[2 * f];
        const int16_t r = in[2 * f + 1];
        int16_t *out = (int16_t *)(void *)(buf + pos);
        if (c->out_frame == 4) {
            out[0] = l;
            out[1] = r;
        } else if (c->take == AUDIOPUMP_TAKE_LEFT) {
            out[0] = l;
        } else if (c->take == AUDIOPUMP_TAKE_RIGHT) {
            out[0] = r;
        } else {
            out[0] = (int16_t)(((int32_t)l + (int32_t)r) >> 1);
        }
        pos += c->out_frame;
        if (pos >= c->len) {
            pos = 0;
        }
    }
    c->wpos = pos;
    c->frames += n;
    c->dropped += frames - n;
    __atomic_store_n(&c->written, c->written + n * c->out_frame,
        __ATOMIC_RELEASE);
}

// On the interpreter. Whole frames only; returns bytes copied.
static uint32_t audiopump_cap_pull(uint8_t *dst, uint32_t want) {
    audiopump_cap_t *c = &audiopump_cap;
    if (c->buf == NULL) {
        return 0;
    }
    want -= want % c->out_frame;
    const uint32_t written = __atomic_load_n(&c->written, __ATOMIC_ACQUIRE);
    const uint32_t have = written - c->taken;
    const uint32_t n = want < have ? want : have;
    const uint32_t first = n < c->len - c->rpos ? n : c->len - c->rpos;
    memcpy(dst, c->buf + c->rpos, first);
    memcpy(dst + first, c->buf, n - first);
    uint32_t pos = c->rpos + n;
    if (pos >= c->len) {
        pos -= c->len;
    }
    c->rpos = pos;
    __atomic_store_n(&c->taken, c->taken + n, __ATOMIC_RELEASE);
    return n;
}

// Take the ring away. The interrupt checks `armed` inside the same spinlock,
// so once this returns nothing is writing into the buffer it frees.
static void audiopump_cap_release(void) {
    portENTER_CRITICAL(&audiopump_cap_mux);
    audiopump_cap.armed = false;
    uint8_t *buf = audiopump_cap.buf;
    audiopump_cap.buf = NULL;
    portEXIT_CRITICAL(&audiopump_cap_mux);
    if (buf != NULL) {
        heap_caps_free(buf);
    }
}

static IRAM_ATTR bool audiopump_on_sent(i2s_chan_handle_t handle,
    i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)user_ctx;
    audiopump_dma_bytes += event->size;
    // The DMA-stopped witness. Microseconds only: a 64-bit division here
    // would call into libgcc from an interrupt.
    const int64_t now = esp_timer_get_time();
    const int64_t last = audiopump_last_sent_us;
    audiopump_last_sent_us = now;
    if (audiopump_starved_live && last != 0) {
        const int64_t gap = now - last;
        const int64_t one = (int64_t)audiopump_desc_us;
        if (one > 0 && gap > 2 * one) {
            audiopump_halted_us += (uint64_t)(gap - one);
        }
    }
    return false;
}

// The ring-ran-dry witness: every buffer but the one just finished is free,
// so the descriptor the DMA starts now carries no audio.
static IRAM_ATTR bool audiopump_on_send_q_ovf(i2s_chan_handle_t handle,
    i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)event;
    (void)user_ctx;
    if (audiopump_starved_live) {
        audiopump_starved_descs++;
    }
    return false;
}

static IRAM_ATTR bool audiopump_on_recv(i2s_chan_handle_t handle,
    i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)user_ctx;
    audiopump_rx_bytes += event->size;
    if (audiopump_cap.armed) {
        portENTER_CRITICAL_ISR(&audiopump_cap_mux);
        if (audiopump_cap.armed && audiopump_cap.buf != NULL) {
            audiopump_cap_push((const int16_t *)event->dma_buf,
                (uint32_t)(event->size / 4));
        }
        portEXIT_CRITICAL_ISR(&audiopump_cap_mux);
    }
    return false;
}

static void audiopump_i2s_close(void) {
    // Everything goes, whoever was holding it: this is the teardown a soft
    // reset and `i2s_stop()` take. The ring first, so no interrupt is inside
    // it when the channel stops.
    audiopump_cap_release();
    audiopump_i2s_holds = 0;
    if (audiopump_i2s_rx != NULL) {
        i2s_channel_disable(audiopump_i2s_rx);
    }
    if (audiopump_i2s_tx != NULL) {
        i2s_channel_disable(audiopump_i2s_tx);
    }
    if (audiopump_i2s_rx != NULL) {
        i2s_del_channel(audiopump_i2s_rx);
        audiopump_i2s_rx = NULL;
    }
    if (audiopump_i2s_tx != NULL) {
        i2s_del_channel(audiopump_i2s_tx);
        audiopump_i2s_tx = NULL;
    }
}

static bool audiopump_sink_ready(void) {
    return audiopump_i2s_tx != NULL;
}

static uint32_t audiopump_sink_write(const uint8_t *buffer, uint32_t length,
    uint32_t timeout_ms, bool *timed_out) {
    if (audiopump_i2s_tx == NULL) {
        return 0;
    }
    size_t written = 0;
    const esp_err_t err = i2s_channel_write(audiopump_i2s_tx, buffer, length,
        &written, pdMS_TO_TICKS(timeout_ms));
    *timed_out = (err == ESP_ERR_TIMEOUT);
    audiopump_sink_bytes += written;
    if (written > 0 && audiopump_starved_pending) {
        // The first audio of this performance is in the ring: from here on,
        // a descriptor that goes out empty is the pump being late.
        audiopump_starved_pending = false;
        audiopump_starved_live = true;
    }
    return (uint32_t)written;
}

static uint64_t audiopump_sink_dma_bytes(void) {
    return audiopump_dma_bytes;
}

static uint64_t audiopump_sink_rx_bytes(void) {
    return audiopump_rx_bytes;
}

#else

// The desktop sink is a file descriptor and the loop only ever writes to it.
// On Windows the open carries _O_BINARY, and that is not a detail: without it
// the CRT turns every 0x0A byte of PCM into 0x0D 0x0A on the way out, so the
// file is longer than the audio, every frame after the first newline-valued
// sample is shifted, and the byte-identity proof compares a mangled file
// against a good one. It is the one difference between the two opens.
static int audiopump_sink_fd = -1;

static bool audiopump_sink_open(const char *path) {
    #if AUDIOIF_DRV_WIN
    audiopump_sink_fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
        _S_IREAD | _S_IWRITE);
    #else
    audiopump_sink_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    #endif
    return audiopump_sink_fd >= 0;
}

static void audiopump_sink_close(void) {
    if (audiopump_sink_fd >= 0) {
        #if AUDIOIF_DRV_WIN
        _close(audiopump_sink_fd);
        #else
        close(audiopump_sink_fd);
        #endif
        audiopump_sink_fd = -1;
    }
}

static bool audiopump_sink_ready(void) {
    return audiopump_sink_fd >= 0;
}

static uint32_t audiopump_sink_write(const uint8_t *buffer, uint32_t length,
    uint32_t timeout_ms, bool *timed_out) {
    (void)timeout_ms;
    *timed_out = false;
    if (audiopump_sink_fd < 0) {
        return 0;
    }
    #if AUDIOIF_DRV_WIN
    const int written = _write(audiopump_sink_fd, buffer,
        (unsigned int)length);
    #else
    const ssize_t written = write(audiopump_sink_fd, buffer, length);
    #endif
    return written > 0 ? (uint32_t)written : 0;
}

#endif

// --- letting go -----------------------------------------------------------

static void audiopump_driver_teardown(void) {
    #if AUDIOIF_DRV_ESP
    audiopump_i2s_close();
    #elif AUDIOIF_DRV_WIN
    // The pace timer belongs to whichever thread first paced -- which is the
    // pump, and it has just been joined. Closing it here, from the interpreter
    // thread, is the only moment nobody is waiting on it.
    if (audiopump_pace_timer != NULL) {
        CloseHandle(audiopump_pace_timer);
        audiopump_pace_timer = NULL;
        audiopump_pace_owner = 0;
        audiopump_pace_coarse = false;
    }
    // Same moment, same reason: the engine calls teardown() only after the
    // thread has been released, so nothing is waiting on this handle and
    // nothing will signal it until the next thread_start makes a new one.
    if (audiopump_wake_event != NULL) {
        CloseHandle(audiopump_wake_event);
        audiopump_wake_event = NULL;
    }
    #endif
}

// --- memory for a decoder --------------------------------------------------
//
// What audiodsp's codecs allocate (the Vorbis decoder's state: tens of
// kilobytes in small, long-lived blocks that no DMA touches). PSRAM first, so
// it stays out of the internal RAM that Wi-Fi and DMA need; internal RAM only
// on a board without PSRAM or with it full. Plain malloc would put every
// block under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB on the P4) inside.
// heap_caps_* are thread-safe and never raise, as the hooks must be.
#if AUDIOIF_DRV_ESP && defined(AUDIODSP_PORT_MEM_HOOKS)
#define AUDIOIF_MEM_PREFERRED (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static void *audioif_mem_alloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2, AUDIOIF_MEM_PREFERRED, MALLOC_CAP_8BIT);
}

static void *audioif_mem_realloc(void *ptr, size_t size) {
    return heap_caps_realloc_prefer(ptr, size, 2, AUDIOIF_MEM_PREFERRED, MALLOC_CAP_8BIT);
}

static void audioif_mem_free(void *ptr) {
    heap_caps_free(ptr);
}
#endif

// --- the table, and the binding -------------------------------------------

static const audiodsp_port_ops_t audiopump_driver_ops = {
    .name = AUDIOIF_DRV_NAME,
    .lock_take = audiopump_lock_take,
    .lock_give = audiopump_lock_give,
    .now_us = audiopump_now_us,
    .thread_start = audiopump_thread_start,
    .thread_wake = audiopump_thread_wake,
    .thread_release = audiopump_thread_release,
    .self_id = audiopump_self_id,
    .status_tid = audiopump_status_tid,
    .stack_free = audiopump_stack_free,
    .park_spin = audiopump_park_spin,
    .sleep_us = audiopump_sleep_us,
    #if !AUDIOIF_DRV_ESP
    .sink_open = audiopump_sink_open,
    .sink_close = audiopump_sink_close,
    #endif
    .sink_ready = audiopump_sink_ready,
    .sink_write = audiopump_sink_write,
    #if AUDIOIF_DRV_ESP
    .sink_dma_bytes = audiopump_sink_dma_bytes,
    .sink_rx_bytes = audiopump_sink_rx_bytes,
    #endif
    .teardown = audiopump_driver_teardown,
    #if AUDIOIF_DRV_ESP && defined(AUDIODSP_PORT_MEM_HOOKS)
    .mem_alloc = audioif_mem_alloc,
    .mem_realloc = audioif_mem_realloc,
    .mem_free = audioif_mem_free,
    #endif
};

// The override. Strong here, weak in the engine; and this definition sits in
// the same object as _audioif_module below, which the firmware's module table
// always references, so no archive can leave it out.
const audiodsp_port_ops_t *audiodsp_port_driver(void) {
    return &audiopump_driver_ops;
}

// --- the I2S sink and source, opened from Python ---------------------------
//
// Pins, rate and slot mode are board facts, and board facts stay in Python
// beside every other board decision -- exactly as usbif_i2s.c does it.

#if AUDIOIF_DRV_ESP
static uint32_t audiopump_i2s_open_raw(const audiopump_i2s_cfg_t *cfg);

static uint32_t audiopump_i2s_depth(void) {
    return audiopump_dma_depth;
}

// The pins and the clock, which two holders of one channel have to agree on.
// Raises naming the first thing that differs, because "Peripheral in use"
// would send a caller looking for a second owner that does not exist.
static void audiopump_i2s_check_join(const audiopump_i2s_cfg_t *want,
    bool want_rx) {
    const audiopump_i2s_cfg_t *live = &audiopump_i2s_live;
    // A holder that named no port joins whichever one the channel is on.
    if ((want->port >= 0 && live->port >= 0 && want->port != live->port)
        || want->bclk != live->bclk
        || want->ws != live->ws
        || (want->dout >= 0 && want->dout != live->dout)
        || (want->din >= 0 && want->din != live->din)) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Peripheral in use"));
    }
    if (want_rx && live->din < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "the I2S channel was opened without its microphone pin; open the "
            "output with data_in= to record while it plays"));
    }
    if (want->rate != live->rate) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT(
            "the I2S channel is clocked at %d Hz and this asks for %d Hz; "
            "playback and capture share the clock, so they must share the rate"),
            live->rate, want->rate);
    }
    if (want->bits != live->bits || want->channels != live->channels) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "the I2S channel is open with another sample width or slot count"));
    }
}

// The OUTPUT's open. Joins a channel the capture reader holds; refuses a
// second output, as it always has.
uint32_t audiopump_i2s_open(const audiopump_i2s_cfg_t *cfg) {
    if (audiopump_i2s_tx != NULL) {
        if (audiopump_i2s_holds & AUDIOPUMP_I2S_HOLD_OUT) {
            mp_raise_ValueError(MP_ERROR_TEXT("i2s already open"));
        }
        audiopump_i2s_check_join(cfg, false);
        audiopump_i2s_holds |= AUDIOPUMP_I2S_HOLD_OUT;
        return audiopump_i2s_depth();
    }
    const uint32_t depth = audiopump_i2s_open_raw(cfg);
    audiopump_i2s_holds = AUDIOPUMP_I2S_HOLD_OUT;
    return depth;
}

static uint32_t audiopump_i2s_open_raw(const audiopump_i2s_cfg_t *cfg) {
    if (audiopump_i2s_tx != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("i2s already open"));
    }
    audiopump_arm_guard();

    const bool duplex = cfg->din >= 0;
    // Two ports means two clock dividers. They start from the same PLL, so
    // they are nominally the same rate, but nothing locks them sample to
    // sample: over minutes the capture and the playback drift apart and the
    // pump, which is paced by the Input's read, ends up handing TX slightly
    // more or slightly less than it consumes. A single-port duplex has no
    // such thing -- there BCLK and WS are generated once.
    const bool split = duplex && cfg->in_port >= 0
        && cfg->in_port != cfg->port;

    // No port named is CircuitPython's own default: the driver picks a free
    // one. I2S_NUM_AUTO is the enum after the last port, not -1, so -1 has
    // to be turned into it here or `I2SOut(bclk, ws, data)` is refused as
    // an invalid port.
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(
        cfg->port < 0 ? I2S_NUM_AUTO : (i2s_port_t)cfg->port,
        I2S_ROLE_MASTER);
    chan_config.dma_desc_num = (uint32_t)cfg->dma_desc;
    chan_config.dma_frame_num = (uint32_t)cfg->dma_frame;
    chan_config.auto_clear = true;   // zeros on underrun, never stale data
    esp_err_t err = i2s_new_channel(&chan_config, &audiopump_i2s_tx,
        (duplex && !split) ? &audiopump_i2s_rx : NULL);
    if (err == ESP_OK && split) {
        i2s_chan_config_t rx_config = I2S_CHANNEL_DEFAULT_CONFIG(
            (i2s_port_t)cfg->in_port, I2S_ROLE_MASTER);
        rx_config.dma_desc_num = (uint32_t)cfg->dma_desc;
        rx_config.dma_frame_num = (uint32_t)cfg->dma_frame;
        rx_config.auto_clear = true;
        err = i2s_new_channel(&rx_config, NULL, &audiopump_i2s_rx);
    }
    if (err != ESP_OK) {
        audiopump_i2s_close();
        audiopump_i2s_tx = NULL;
        audiopump_i2s_rx = NULL;
        // ESP_ERR_NOT_FOUND is every peripheral already taken, and
        // CircuitPython's own words for it are "Peripheral in use". Any other
        // failure here is wiring or configuration, and OSError(EIO) is what
        // this entry point has always raised for those.
        if (err == ESP_ERR_NOT_FOUND) {
            mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("Peripheral in use"));
        }
        mp_raise_OSError(MP_EIO);
    }

    const int bits = cfg->bits;
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT :
        (bits == 24 ? I2S_DATA_BIT_WIDTH_24BIT : I2S_DATA_BIT_WIDTH_16BIT),
        cfg->channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)cfg->rate),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = cfg->mclk < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)cfg->mclk,
            .bclk = (gpio_num_t)cfg->bclk,
            .ws = (gpio_num_t)cfg->ws,
            .dout = (gpio_num_t)cfg->dout,
            // Not on the split path: there `din` belongs to the other
            // peripheral, and claiming it here would route the microphone
            // into the speaker's port as well.
            .din = (duplex && !split) ? (gpio_num_t)cfg->din
                                      : I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    switch (cfg->mclk_fs) {
        case 128: std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128; break;
        case 384: std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384; break;
        case 512: std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512; break;
        default:  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256; break;
    }

    err = i2s_channel_init_std_mode(audiopump_i2s_tx, &std_cfg);
    if (err == ESP_OK && duplex && !split) {
        // The same std config on both halves. On one port that is not a
        // convenience -- the IDF derives BCLK/WS once and the second channel
        // has to agree with it or the init is refused.
        err = i2s_channel_init_std_mode(audiopump_i2s_rx, &std_cfg);
    } else if (err == ESP_OK && split) {
        // Its own pins, its own MCLK. The ES7210 needs one and there is no
        // MCLK on the speaker's wire at all, so this is not a variation on
        // the TX config -- it is a second device.
        i2s_std_config_t rx_cfg = std_cfg;
        rx_cfg.gpio_cfg.bclk = (gpio_num_t)(cfg->in_bclk < 0
            ? cfg->bclk : cfg->in_bclk);
        rx_cfg.gpio_cfg.ws = (gpio_num_t)(cfg->in_ws < 0
            ? cfg->ws : cfg->in_ws);
        rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
        rx_cfg.gpio_cfg.din = (gpio_num_t)cfg->din;
        rx_cfg.gpio_cfg.mclk = cfg->in_mclk < 0
            ? I2S_GPIO_UNUSED : (gpio_num_t)cfg->in_mclk;
        err = i2s_channel_init_std_mode(audiopump_i2s_rx, &rx_cfg);
    }
    if (err == ESP_OK) {
        i2s_event_callbacks_t cbs = {
            .on_sent = audiopump_on_sent,
            .on_send_q_ovf = audiopump_on_send_q_ovf,
        };
        err = i2s_channel_register_event_callback(audiopump_i2s_tx, &cbs, NULL);
    }
    if (err == ESP_OK && duplex) {
        i2s_event_callbacks_t rx_cbs = { .on_recv = audiopump_on_recv };
        err = i2s_channel_register_event_callback(audiopump_i2s_rx, &rx_cbs,
            NULL);
    }
    if (err == ESP_OK) {
        audiopump_dma_bytes = 0;
        audiopump_rx_bytes = 0;
        audiopump_sink_bytes = 0;
        const uint32_t frame_bytes = (uint32_t)(cfg->bits / 8)
            * (uint32_t)(cfg->channels == 1 ? 1 : 2);
        audiopump_dma_depth = (uint32_t)chan_config.dma_desc_num
            * (uint32_t)chan_config.dma_frame_num * frame_bytes;
        audiopump_byte_rate = (uint32_t)cfg->rate * frame_bytes;
        audiopump_frame_bytes = frame_bytes;
        audiopump_desc_bytes = (uint32_t)chan_config.dma_frame_num
            * frame_bytes;
        audiopump_desc_us = (uint32_t)(((uint64_t)chan_config.dma_frame_num
            * 1000000ULL) / (uint32_t)cfg->rate);
        audiopump_last_sent_us = 0;
        audiopump_i2s_starved_reset();
        // RX first: it is the half that must not miss the beginning of what
        // TX emits, and enabling it first makes any gap between the two an
        // over-estimate of the round trip rather than an under-estimate.
        if (duplex) {
            err = i2s_channel_enable(audiopump_i2s_rx);
        }
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(audiopump_i2s_tx);
    }
    if (err != ESP_OK) {
        audiopump_i2s_close();
        mp_raise_OSError(MP_EIO);
    }
    audiopump_i2s_live = *cfg;
    // Bytes the DMA holds when full: the block-to-wire latency floor.
    return chan_config.dma_desc_num * chan_config.dma_frame_num
        * (uint32_t)(bits / 8) * (uint32_t)(cfg->channels == 1 ? 1 : 2);
}

static mp_obj_t audiopump_i2s_start(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_port, ARG_bclk, ARG_ws, ARG_dout, ARG_rate, ARG_bits,
           ARG_channels, ARG_mclk, ARG_mclk_fs, ARG_dma_desc, ARG_dma_frame,
           ARG_din, ARG_in_port, ARG_in_bclk, ARG_in_ws, ARG_in_mclk };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_port,      MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_bclk,      MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_ws,        MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_dout,      MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_rate,      MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 48000 } },
        { MP_QSTR_bits,      MP_ARG_INT,  { .u_int = 16 } },
        { MP_QSTR_channels,  MP_ARG_INT,  { .u_int = 2 } },
        { MP_QSTR_mclk,      MP_ARG_INT,  { .u_int = -1 } },
        { MP_QSTR_mclk_fs,   MP_ARG_INT,  { .u_int = 256 } },
        { MP_QSTR_dma_desc,  MP_ARG_INT,  { .u_int = 6 } },
        { MP_QSTR_dma_frame, MP_ARG_INT,  { .u_int = 240 } },
        // >= 0 opens the RX half of the SAME channel pair, so one clock tree
        // drives both directions.
        { MP_QSTR_din,       MP_ARG_INT,  { .u_int = -1 } },
        // ... unless the capture device is on a DIFFERENT I2S peripheral,
        // which is what the LilyGO T-Embed is: a MAX98357A on port 1 and an
        // ES7210 on port 0, with their own pins. Then RX is a second
        // channel with its own clock divider, and the two are NOT
        // sample-locked -- see the note below the call.
        { MP_QSTR_in_port,   MP_ARG_INT,  { .u_int = -1 } },
        { MP_QSTR_in_bclk,   MP_ARG_INT,  { .u_int = -1 } },
        { MP_QSTR_in_ws,     MP_ARG_INT,  { .u_int = -1 } },
        { MP_QSTR_in_mclk,   MP_ARG_INT,  { .u_int = -1 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    const audiopump_i2s_cfg_t cfg = {
        .port = args[ARG_port].u_int,
        .bclk = args[ARG_bclk].u_int,
        .ws = args[ARG_ws].u_int,
        .dout = args[ARG_dout].u_int,
        .rate = args[ARG_rate].u_int,
        .bits = args[ARG_bits].u_int,
        .channels = args[ARG_channels].u_int,
        .mclk = args[ARG_mclk].u_int,
        .mclk_fs = args[ARG_mclk_fs].u_int,
        .dma_desc = args[ARG_dma_desc].u_int,
        .dma_frame = args[ARG_dma_frame].u_int,
        .din = args[ARG_din].u_int,
        .in_port = args[ARG_in_port].u_int,
        .in_bclk = args[ARG_in_bclk].u_int,
        .in_ws = args[ARG_in_ws].u_int,
        .in_mclk = args[ARG_in_mclk].u_int,
    };
    return mp_obj_new_int_from_uint(audiopump_i2s_open(&cfg));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiopump_i2s_start_obj, 5,
    audiopump_i2s_start);

static mp_obj_t audiopump_i2s_stop(void) {
    audiopump_i2s_close();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_i2s_stop_obj, audiopump_i2s_stop);

static mp_obj_t audiopump_i2s_dma_bytes(void) {
    return mp_obj_new_int_from_uint((mp_uint_t)audiopump_dma_bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_i2s_dma_bytes_obj,
    audiopump_i2s_dma_bytes);

static mp_obj_t audiopump_i2s_rx_bytes(void) {
    return mp_obj_new_int_from_uint((mp_uint_t)audiopump_rx_bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_i2s_rx_bytes_obj,
    audiopump_i2s_rx_bytes);

// How far into the DMA's output stream the pump has accounted for -- bytes
// it handed over, plus any silence already charged as starvation. Beside the
// two counters above so that `audiobusio.I2SOut.starved()` is a number with
// its working attached rather than one you have to believe: in health this
// leads `i2s_dma_bytes()` by the depth of the DMA ring and never falls behind
// it.
static mp_obj_t audiopump_i2s_sink_bytes(void) {
    return mp_obj_new_int_from_uint((mp_uint_t)audiopump_sink_bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_i2s_sink_bytes_obj,
    audiopump_i2s_sink_bytes);

static mp_obj_t audiopump_i2s_starved_bytes(void) {
    return mp_obj_new_int_from_uint((mp_uint_t)audiopump_i2s_starved());
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_i2s_starved_bytes_obj,
    audiopump_i2s_starved_bytes);

// --- rx_open / rx_read: recording while the output plays -------------------
//
//     _audioif.rx_open(0, 12, 10, 9, 11, 24000, mclk=13, ring=20000, take=2)
//     n = _audioif.rx_read(buf)          # never blocks; whole frames
//     _audioif.rx_close()
//
// Opens the channel pair if nothing has (TX then clocks zeros until an output
// joins), or joins the one `audiobusio.I2SOut(..., data_in=)` opened. Either
// way the output and the recorder are the two halves of one channel and one
// clock, so neither can take the port away from the other: closing the
// output leaves the recording running, and closing the recorder leaves the
// output playing.
//
// `dout` is the speaker's pin even for a recorder that never plays, because
// the pair is allocated together and an output arriving later has to find
// its half already wired.
static mp_obj_t audiopump_rx_open(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_port, ARG_bclk, ARG_ws, ARG_dout, ARG_din, ARG_rate, ARG_mclk,
           ARG_mclk_fs, ARG_ring, ARG_take };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_port,     MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_bclk,     MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_ws,       MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_dout,     MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_din,      MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_rate,     MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 48000 } },
        { MP_QSTR_mclk,     MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_mclk_fs,  MP_ARG_INT, { .u_int = 256 } },
        // Bytes of history the reader may fall behind by before audio is
        // dropped. In the reader's format, so 20000 is 416 ms of one channel
        // at 24 kHz.
        { MP_QSTR_ring,     MP_ARG_INT, { .u_int = 20000 } },
        // 0 left slot, 1 right, 2 their average (one channel out), 3 both.
        { MP_QSTR_take,     MP_ARG_INT, { .u_int = AUDIOPUMP_TAKE_MIX } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    if (audiopump_i2s_holds & AUDIOPUMP_I2S_HOLD_IN) {
        mp_raise_ValueError(MP_ERROR_TEXT("capture already open"));
    }
    const int take = args[ARG_take].u_int;
    if (take < AUDIOPUMP_TAKE_LEFT || take > AUDIOPUMP_TAKE_BOTH) {
        mp_raise_ValueError(MP_ERROR_TEXT("take must be 0, 1, 2 or 3"));
    }
    if (args[ARG_din].u_int < 0 || args[ARG_dout].u_int < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("rx_open needs both dout and din"));
    }
    const uint8_t out_frame = take == AUDIOPUMP_TAKE_BOTH ? 4 : 2;
    uint32_t len = (uint32_t)args[ARG_ring].u_int;
    len -= len % out_frame;
    if (len < 64u * out_frame) {
        mp_raise_ValueError(MP_ERROR_TEXT("ring is too small"));
    }

    const int rate = args[ARG_rate].u_int;
    const audiopump_i2s_cfg_t cfg = {
        .port = args[ARG_port].u_int,
        .bclk = args[ARG_bclk].u_int,
        .ws = args[ARG_ws].u_int,
        .dout = args[ARG_dout].u_int,
        .rate = rate,
        .bits = 16,
        .channels = 2,
        .mclk = args[ARG_mclk].u_int,
        .mclk_fs = args[ARG_mclk_fs].u_int,
        .dma_desc = 6,
        .dma_frame = audiopump_i2s_dma_frame(rate),
        .din = args[ARG_din].u_int,
        .in_port = -1,
        .in_bclk = -1,
        .in_ws = -1,
        .in_mclk = -1,
    };
    if (audiopump_i2s_tx != NULL) {
        audiopump_i2s_check_join(&cfg, true);
    }

    // The ring before the channel, so a refusal for memory leaves nothing
    // open. Internal RAM first: the interrupt copies into it.
    uint8_t *buf = heap_caps_malloc_prefer(len, 2,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError,
            MP_ERROR_TEXT("no memory for the capture ring"));
    }
    portENTER_CRITICAL(&audiopump_cap_mux);
    audiopump_cap.buf = buf;
    audiopump_cap.len = len;
    audiopump_cap.wpos = 0;
    audiopump_cap.rpos = 0;
    audiopump_cap.written = 0;
    audiopump_cap.taken = 0;
    audiopump_cap.frames = 0;
    audiopump_cap.dropped = 0;
    audiopump_cap.take = (uint8_t)take;
    audiopump_cap.out_frame = out_frame;
    audiopump_cap.armed = audiopump_i2s_tx != NULL;
    portEXIT_CRITICAL(&audiopump_cap_mux);

    if (audiopump_i2s_tx == NULL) {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            (void)audiopump_i2s_open_raw(&cfg);
            nlr_pop();
        } else {
            audiopump_cap_release();
            nlr_jump(nlr.ret_val);
        }
        audiopump_i2s_holds = AUDIOPUMP_I2S_HOLD_IN;
        audiopump_cap.armed = true;
    } else {
        audiopump_i2s_holds |= AUDIOPUMP_I2S_HOLD_IN;
    }
    return mp_obj_new_int_from_uint(len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiopump_rx_open_obj, 6, audiopump_rx_open);

// Copy what has been captured into `buf`, up to its length in whole frames.
// Returns the bytes copied, 0 when nothing is waiting. Never blocks: a
// recorder decides for itself how long to wait, on its own thread.
static mp_obj_t audiopump_rx_read(mp_obj_t buf_in) {
    if (!(audiopump_i2s_holds & AUDIOPUMP_I2S_HOLD_IN)
        || audiopump_cap.buf == NULL) {
        mp_raise_OSError(MP_ENODEV);
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    return mp_obj_new_int_from_uint(
        audiopump_cap_pull((uint8_t *)buf.buf, (uint32_t)buf.len));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_rx_read_obj, audiopump_rx_read);

// Stop recording. The channel closes only if no output holds it. Safe to
// call twice.
static mp_obj_t audiopump_rx_close(void) {
    if (!(audiopump_i2s_holds & AUDIOPUMP_I2S_HOLD_IN)) {
        return mp_const_none;
    }
    audiopump_cap_release();
    audiopump_i2s_holds &= (uint8_t)~AUDIOPUMP_I2S_HOLD_IN;
    if (audiopump_i2s_holds == 0) {
        audiopump_i2s_close();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_rx_close_obj, audiopump_rx_close);

// (frames captured, frames dropped because the ring was full, bytes waiting).
// `dropped` is the number a recorder watches: anything but 0 is a hole in
// the recording, and it means the reader fell further behind than `ring`.
static mp_obj_t audiopump_rx_stats(void) {
    audiopump_cap_t *c = &audiopump_cap;
    const uint32_t waiting = c->buf == NULL ? 0
        : __atomic_load_n(&c->written, __ATOMIC_ACQUIRE) - c->taken;
    mp_obj_t items[3] = {
        mp_obj_new_int_from_uint(c->frames),
        mp_obj_new_int_from_uint(c->dropped),
        mp_obj_new_int_from_uint(waiting),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_rx_stats_obj, audiopump_rx_stats);

// None when no channel is open, else (rate, channels, has_rx, holders):
// holders is 1 for the output, 2 for the recorder, 3 for both. A board asks
// this before it touches a clock pin the channel may already be driving.
static mp_obj_t audiopump_i2s_state(void) {
    if (audiopump_i2s_tx == NULL) {
        return mp_const_none;
    }
    mp_obj_t items[4] = {
        mp_obj_new_int(audiopump_i2s_live.rate),
        mp_obj_new_int(audiopump_i2s_live.channels),
        mp_obj_new_bool(audiopump_i2s_rx != NULL),
        mp_obj_new_int(audiopump_i2s_holds),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_i2s_state_obj, audiopump_i2s_state);

// --- audiopump.Input: the capture side, as an audiosample ------------------
//
// An audiosample whose get_buffer is an i2s_channel_read. That makes the live
// microphone a source like any other, so
//
//     audioeffects.create("Overdrive", audiopump.Input(...), rate)
//
// builds a real effect graph on it and the pump pulls the whole thing. The
// read blocks, which is the pacing: RX is the clock, and because RX and TX
// came out of one i2s_new_channel they are the SAME clock.
//
// Nothing in here calls into the MicroPython runtime, because everything in
// here runs on the pump task.

typedef struct {
    audiosample_base_t base;
    uint8_t *buf[2];            // ping-pong, so a node may hold the last block
    uint32_t len;               // bytes per block
    uint8_t which;
    uint32_t timeout_ms;
    volatile uint64_t blocks;
    volatile uint64_t timeouts;
    volatile uint64_t short_reads;
} audiopump_input_obj_t;

extern const mp_obj_type_t audiopump_input_type;

static void audiopump_input_reset_buffer(audiopump_input_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    self->which = 0;
}

static audioio_get_buffer_result_t audiopump_input_get_buffer(
    audiopump_input_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    uint8_t *dst = self->buf[self->which];
    self->which ^= 1;
    size_t got = 0;
    if (audiopump_i2s_rx != NULL) {
        esp_err_t err = i2s_channel_read(audiopump_i2s_rx, dst, self->len,
            &got, pdMS_TO_TICKS(self->timeout_ms));
        if (err != ESP_OK) {
            self->timeouts++;
        }
    }
    if (got < self->len) {
        // Silence rather than stale audio: a short read is a starve, and a
        // starve should sound like a hole, not like a stutter.
        memset(dst + got, 0, self->len - got);
        if (got != self->len) {
            self->short_reads++;
        }
    }
    self->blocks++;
    *buffer = dst;
    *buffer_length = self->len;
    return GET_BUFFER_MORE_DATA;
}

static mp_obj_t audiopump_input_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_rate, ARG_channels, ARG_frames, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sample_rate,  MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 48000 } },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 2 } },
        { MP_QSTR_frames,       MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 256 } },
        { MP_QSTR_timeout_ms,   MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 500 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    const uint32_t channels = (uint32_t)args[ARG_channels].u_int;
    const uint32_t frames = (uint32_t)args[ARG_frames].u_int;
    if (channels < 1 || channels > 2 || frames < 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad channel_count or frames"));
    }

    audiopump_input_obj_t *self = mp_obj_malloc(audiopump_input_obj_t,
        (const mp_obj_type_t *)&audiopump_input_type);
    self->len = frames * channels * 2;
    self->buf[0] = m_new(uint8_t, self->len);
    self->buf[1] = m_new(uint8_t, self->len);
    memset(self->buf[0], 0, self->len);
    memset(self->buf[1], 0, self->len);
    self->which = 0;
    self->timeout_ms = (uint32_t)args[ARG_timeout_ms].u_int;
    self->blocks = 0;
    self->timeouts = 0;
    self->short_reads = 0;

    self->base.sample_rate = (uint32_t)args[ARG_rate].u_int;
    self->base.max_buffer_length = self->len;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channels;
    self->base.samples_signed = true;
    // Two buffers, so a node downstream may keep the previous block.
    self->base.single_buffer = false;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiopump_input_stats(mp_obj_t self_in) {
    audiopump_input_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t items[4] = {
        mp_obj_new_int_from_uint((mp_uint_t)self->blocks),
        mp_obj_new_int_from_uint((mp_uint_t)self->timeouts),
        mp_obj_new_int_from_uint((mp_uint_t)self->short_reads),
        mp_obj_new_int_from_uint(self->len),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_input_stats_obj,
    audiopump_input_stats);

static const mp_rom_map_elem_t audiopump_input_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiopump_input_stats_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiopump_input_locals,
    audiopump_input_locals_table);

static const audiosample_p_t audiopump_input_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiopump_input_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiopump_input_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiopump_input_type,
    MP_QSTR_Input,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiopump_input_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiopump_input_locals,
    protocol, &audiopump_input_proto
    );

// --- the round-trip latency probe -----------------------------------------
//
// DAC to speaker to microphone to ADC, measured in frames rather than in host
// time. TX and RX share one frame clock, so RX frame N is sampled as TX frame
// N goes out; the only thing needed is a known origin, and
// i2s_channel_preload_data gives one -- the click is written into the DMA
// BEFORE the channels are enabled, so it leaves at a frame this code chose.
//
// Runs on the interpreter thread with no pump spawned; two owners of one
// channel is the failure that sounds like silence.
static mp_obj_t audiopump_rt_probe(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_capture, ARG_frames, ARG_lead, ARG_prime, ARG_level,
           ARG_channels, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_capture,    MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_frames,     MP_ARG_INT, { .u_int = 256 } },
        { MP_QSTR_lead,       MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_prime,      MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_level,      MP_ARG_INT, { .u_int = 24000 } },
        { MP_QSTR_channels,   MP_ARG_INT, { .u_int = 2 } },
        { MP_QSTR_timeout_ms, MP_ARG_INT, { .u_int = 2000 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    if (audiopump_i2s_tx == NULL || audiopump_i2s_rx == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("open i2s with din= first"));
    }
    if (audiopump_is_running()) {
        mp_raise_ValueError(MP_ERROR_TEXT("stop the pump first"));
    }

    mp_buffer_info_t cap;
    mp_get_buffer_raise(args[ARG_capture].u_obj, &cap, MP_BUFFER_WRITE);

    const uint32_t channels = (uint32_t)args[ARG_channels].u_int;
    const uint32_t frames = (uint32_t)args[ARG_frames].u_int;
    const uint32_t lead = (uint32_t)args[ARG_lead].u_int;
    const uint32_t frame_bytes = channels * 2;
    const uint32_t click_bytes = frames * frame_bytes;

    // Silence the whole path, then rebuild both DMAs from zero.
    i2s_channel_disable(audiopump_i2s_tx);
    i2s_channel_disable(audiopump_i2s_rx);

    // One block: `lead` frames of silence and then a hard step. The step is a
    // step, not an impulse -- a single frame gets swallowed by the codec's
    // reconstruction filter, and a step's leading edge is just as findable.
    uint8_t *click = m_new(uint8_t, click_bytes);
    memset(click, 0, click_bytes);
    int16_t *s = (int16_t *)click;
    const int16_t level = (int16_t)args[ARG_level].u_int;
    for (uint32_t f = lead; f < frames; f++) {
        for (uint32_t c = 0; c < channels; c++) {
            // A few hundred microseconds of square wave: loud, broadband and
            // unmistakable against a quiet room.
            s[f * channels + c] = ((f - lead) / 8) % 2 ? level : (int16_t)-level;
        }
    }

    size_t loaded = 0;
    // `prime` blocks of silence ahead of the click model a TX ring that is
    // already full -- which is what a live pump keeps it. prime=0 measures
    // the path with both rings empty.
    uint8_t *quiet = NULL;
    if (args[ARG_prime].u_int > 0) {
        quiet = m_new(uint8_t, click_bytes);
        memset(quiet, 0, click_bytes);
        for (int i = 0; i < args[ARG_prime].u_int; i++) {
            size_t n = 0;
            i2s_channel_preload_data(audiopump_i2s_tx, quiet, click_bytes, &n);
            loaded += n;
        }
    }
    size_t click_at = loaded;
    size_t n = 0;
    esp_err_t err = i2s_channel_preload_data(audiopump_i2s_tx, click,
        click_bytes, &n);
    loaded += n;

    const uint64_t t_rx = audiopump_now_us();
    if (err == ESP_OK) {
        err = i2s_channel_enable(audiopump_i2s_rx);
    }
    const uint64_t t_tx = audiopump_now_us();
    if (err == ESP_OK) {
        err = i2s_channel_enable(audiopump_i2s_tx);
    }
    size_t got = 0;
    if (err == ESP_OK) {
        err = i2s_channel_read(audiopump_i2s_rx, cap.buf, cap.len, &got,
            pdMS_TO_TICKS(args[ARG_timeout_ms].u_int));
    }

    mp_obj_t items[6] = {
        mp_obj_new_int_from_uint((mp_uint_t)got),
        // Frame index at which the step leaves the DAC, in the TX stream.
        mp_obj_new_int_from_uint((mp_uint_t)((click_at + lead * frame_bytes)
            / frame_bytes)),
        mp_obj_new_int_from_uint((mp_uint_t)(loaded / frame_bytes)),
        // Microseconds between enabling RX and enabling TX: the one piece of
        // host timing in the measurement, reported rather than assumed away.
        mp_obj_new_int_from_uint((mp_uint_t)(t_tx - t_rx)),
        mp_obj_new_int((mp_int_t)err),
        mp_obj_new_int_from_uint((mp_uint_t)frame_bytes),
    };
    m_del(uint8_t, click, click_bytes);
    if (quiet != NULL) {
        m_del(uint8_t, quiet, click_bytes);
    }
    return mp_obj_new_tuple(6, items);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiopump_rt_probe_obj, 1,
    audiopump_rt_probe);
#endif

// --- the module -----------------------------------------------------------

static mp_obj_t audiopump_driver_name(void) {
    return mp_obj_new_str_from_cstr(AUDIOIF_DRV_NAME);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_driver_name_obj,
    audiopump_driver_name);

// --- lock_probe: does FreeRTOS refuse a non-owner's give? -------------------
//
// audiodsp#107 fixed two nodes that released the pump lock without ever
// having taken it. On Windows that corrupted the CRITICAL_SECTION; glibc
// refuses a non-owner's unlock with EPERM, which was measured. That FreeRTOS
// refuses it too -- which is why neither ESP32 board ever showed the defect --
// was read out of its source and never run (audiodsp#117).
//
// This runs it. The interpreter takes the real pump mutex, a second task
// tries to give it, and the BaseType_t that comes back is the answer. It is a
// diagnostic and nothing in the audio path calls it; it is here rather than in
// a test script because the mutex is a static in this file and the question is
// about a task that is not the owner, which Python cannot arrange -- the
// esp32 port's own `_thread` lock is a binary semaphore, which has no owner at
// all and would answer a different question convincingly.
//
// Returns (held_by_other, free_for_all, owner_control): 1 where the give was
// ACCEPTED, 0 where FreeRTOS refused it. The third is a control that MUST
// read 1 -- a task giving back a mutex it took itself -- because 0 is what
// the FreeRTOS source predicts for the first two, so a probe that never ran
// would agree with the expected answer perfectly.

#if AUDIOIF_DRV_ESP

typedef struct {
    SemaphoreHandle_t done;
    volatile int result;
    bool take_first;
} audiopump_lock_probe_t;

static void audiopump_lock_probe_task(void *arg) {
    audiopump_lock_probe_t *ctx = (audiopump_lock_probe_t *)arg;
    // The CONTROL leg: this task takes the mutex itself, so the give that
    // follows is an owner's and MUST be accepted. Without it a probe that
    // never ran, or one whose give was broken for some unrelated reason,
    // returns the same zeros as the real answer -- and zero is the answer
    // the FreeRTOS source predicts, so the two would be indistinguishable.
    if (ctx->take_first) {
        xSemaphoreTakeRecursive(audiopump_mutex, portMAX_DELAY);
    }
    // xQueueGiveMutexRecursive compares the holder against the CURRENT task
    // and returns pdFAIL without asserting when they differ, so this is safe
    // to run: it is a question, not a corruption.
    ctx->result = (xSemaphoreGiveRecursive(audiopump_mutex) == pdTRUE) ? 1 : 0;
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static int audiopump_lock_probe_once_take(bool take_first) {
    audiopump_lock_probe_t ctx = { NULL, -1, take_first };
    ctx.done = xSemaphoreCreateBinary();
    if (ctx.done == NULL) {
        return -1;
    }
    TaskHandle_t task = NULL;
    if (xTaskCreate(audiopump_lock_probe_task, "lockprobe", 3072, &ctx, 5,
            &task) != pdPASS) {
        vSemaphoreDelete(ctx.done);
        return -1;
    }
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(2000));
    vSemaphoreDelete(ctx.done);
    return ctx.result;
}

static int audiopump_lock_probe_once(void) {
    return audiopump_lock_probe_once_take(false);
}

static mp_obj_t audiopump_lock_probe(void) {
    audiopump_lock_ensure();
    if (audiopump_mutex == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no mutex"));
    }
    // 1. The interpreter holds it; another task tries to give it.
    xSemaphoreTakeRecursive(audiopump_mutex, portMAX_DELAY);
    const int held = audiopump_lock_probe_once();
    xSemaphoreGiveRecursive(audiopump_mutex);
    // 2. Nobody holds it; another task tries to give it anyway. This is the
    //    shape audiodsp#107's nodes were actually in -- a release with no
    //    take anywhere, not a release of somebody else's take.
    const int freed = audiopump_lock_probe_once();
    // 3. The control, which must come back 1: a task that TOOK the mutex
    //    gives it back. A probe that cannot produce a 1 has not been shown
    //    to be able to answer anything, and 0 is what the source predicts.
    const int owner = audiopump_lock_probe_once_take(true);
    mp_obj_t items[3] = {
        MP_OBJ_NEW_SMALL_INT(held),
        MP_OBJ_NEW_SMALL_INT(freed),
        MP_OBJ_NEW_SMALL_INT(owner),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_lock_probe_obj,
    audiopump_lock_probe);

#endif


// _audioif.__revision__: which audioif this firmware was built from. The build
// passes AUDIOIF_REVISION (git describe) as a compile definition; a build with no
// git says "unknown" rather than guessing.
#ifndef AUDIOIF_REVISION
#define AUDIOIF_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(_audioif_revision_obj, AUDIOIF_REVISION);

static const mp_rom_map_elem_t audioif_driver_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__audioif) },
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&_audioif_revision_obj) },
    // What this driver says it is. audiopump.driver() asks the ENGINE the same
    // question, through the binding; the two agreeing is the proof the
    // binding took.
    { MP_ROM_QSTR(MP_QSTR_driver), MP_ROM_PTR(&audiopump_driver_name_obj) },
    #if AUDIOIF_DRV_ESP
    { MP_ROM_QSTR(MP_QSTR_i2s_start), MP_ROM_PTR(&audiopump_i2s_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2s_stop), MP_ROM_PTR(&audiopump_i2s_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2s_dma_bytes),
      MP_ROM_PTR(&audiopump_i2s_dma_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2s_rx_bytes),
      MP_ROM_PTR(&audiopump_i2s_rx_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2s_sink_bytes),
      MP_ROM_PTR(&audiopump_i2s_sink_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2s_starved_bytes),
      MP_ROM_PTR(&audiopump_i2s_starved_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2s_state), MP_ROM_PTR(&audiopump_i2s_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_rx_open), MP_ROM_PTR(&audiopump_rx_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_rx_read), MP_ROM_PTR(&audiopump_rx_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_rx_close), MP_ROM_PTR(&audiopump_rx_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_rx_stats), MP_ROM_PTR(&audiopump_rx_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_Input), MP_ROM_PTR(&audiopump_input_type) },
    { MP_ROM_QSTR(MP_QSTR_rt_probe), MP_ROM_PTR(&audiopump_rt_probe_obj) },
    { MP_ROM_QSTR(MP_QSTR_lock_probe),
      MP_ROM_PTR(&audiopump_lock_probe_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(audioif_driver_globals,
    audioif_driver_globals_table);

const mp_obj_module_t _audioif_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioif_driver_globals,
};
MP_REGISTER_MODULE(MP_QSTR__audioif, _audioif_module);

#endif  // !AUDIOIF_DRV_NONE

// The four the CircuitPython-shaped binding in audiobusio.c needs. On a
// build with no I2S they still exist and answer honestly, so that file needs
// no #if of its own.
bool audiopump_i2s_have(void) {
    #if AUDIOIF_DRV_ESP
    return true;
    #else
    return false;
    #endif
}

bool audiopump_i2s_is_open(void) {
    #if AUDIOIF_DRV_ESP
    return audiopump_i2s_tx != NULL;
    #else
    return false;
    #endif
}

void audiopump_i2s_shutdown(void) {
    #if AUDIOIF_DRV_ESP
    audiopump_i2s_holds &= (uint8_t)~AUDIOPUMP_I2S_HOLD_OUT;
    if (audiopump_i2s_holds == 0) {
        audiopump_i2s_close();
    }
    #endif
}

bool audiopump_i2s_input_held(void) {
    #if AUDIOIF_DRV_ESP
    return (audiopump_i2s_holds & AUDIOPUMP_I2S_HOLD_IN) != 0;
    #else
    return false;
    #endif
}

uint32_t audiopump_i2s_live_rate(void) {
    #if AUDIOIF_DRV_ESP
    return audiopump_i2s_tx != NULL ? (uint32_t)audiopump_i2s_live.rate : 0;
    #else
    return 0;
    #endif
}

// Silence on the wire: audio that should have played and did not, in bytes,
// since the performance began.
//
// What this replaces was the engine's SINK_TIMEOUTS -- the number of times
// the sink REFUSED a block. That can only move when the pump is running and
// the DMA is FULL, which is the one situation that is not an underrun at all,
// and it had never been seen to move on either chip (audioif#8).
//
// The two witnesses and why each is needed are written down beside their
// state, under "the underrun count". This only adds them up.
//
// **Silence that was asked for is not starvation.** A stopped output and a
// paused one both leave the channel clocking zeros on purpose. The caller
// says which: `audiopump_i2s_starved_reset()` where a performance begins
// (`play()`, the channel opening), `audiopump_i2s_starved_hold()` where a
// pause begins, and `audiopump_i2s_starved_resume()` where it ends. The count
// survives a pause, because starved audio cannot un-happen; only a new
// performance clears it.
//
// A file sink has no clock and cannot run ahead of what was written to it, so
// zero there is the true answer rather than a stub: a desktop's pacing
// failures are wall-clock drift, which is a different number and is already
// reported as one.
uint64_t audiopump_i2s_starved(void) {
    #if AUDIOIF_DRV_ESP
    const uint64_t dry = (uint64_t)audiopump_starved_descs
        * (uint64_t)audiopump_desc_bytes;
    uint64_t halted = (audiopump_halted_us * (uint64_t)audiopump_byte_rate)
        / 1000000ULL;
    // Whole frames, like everything else this driver counts in bytes.
    if (audiopump_frame_bytes > 1u) {
        halted -= halted % audiopump_frame_bytes;
    }
    return dry + halted;
    #else
    return 0;
    #endif
}

// A new performance starts here: forget the old one's silence, and count
// again from the pump's first write.
void audiopump_i2s_starved_reset(void) {
    #if AUDIOIF_DRV_ESP
    audiopump_sink_bytes = audiopump_dma_bytes;
    audiopump_starved_live = false;
    audiopump_starved_descs = 0;
    audiopump_halted_us = 0;
    audiopump_starved_pending = true;
    #endif
}

// A pause begins: the zeros it clocks were asked for. Keep what was counted.
void audiopump_i2s_starved_hold(void) {
    #if AUDIOIF_DRV_ESP
    audiopump_starved_pending = false;
    audiopump_starved_live = false;
    #endif
}

// The pause ends: count again from the pump's next write, keeping the total.
void audiopump_i2s_starved_resume(void) {
    #if AUDIOIF_DRV_ESP
    audiopump_starved_live = false;
    audiopump_starved_pending = true;
    #endif
}

#if !AUDIOIF_DRV_ESP
uint32_t audiopump_i2s_open(const audiopump_i2s_cfg_t *cfg) {
    (void)cfg;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("this port has no I2S"));
}
#endif
