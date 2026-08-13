#ifndef DTT_PLATFORM_H
#define DTT_PLATFORM_H

/* ============================================================================
 * dtt_platform.h
 *
 * Centralized platform / architecture detection plus small portability
 * shims (mutex, thread-local storage, millisecond clock) so the rest of
 * the agent never has to sprinkle raw #ifdefs around.
 *
 * Supported build targets:
 *   Windows  -> dtt_agent.dll   (MSVC or MinGW-w64)
 *   Linux    -> libdtt_agent.so   (gcc / clang)
 *   macOS    -> libdtt_agent.dylib (Apple clang)
 *
 * NOTE ON JNIEXPORT/JNICALL:
 * The JDK's own <jni.h>/<jni_md.h> (shipped per-platform with every JDK)
 * already define JNIEXPORT/JNICALL correctly for whichever OS you are
 * compiling on (e.g. __declspec(dllexport) on Windows, default visibility
 * attribute on Linux/macOS). Agent_OnLoad/Agent_OnUnload/Agent_OnAttach
 * and every JNI-callable bridge function in this project use those
 * macros directly - we do not redefine export semantics here, we only
 * add the extra portability primitives jni.h does not provide.
 * ============================================================================ */

#include <jni.h>
#include <jvmti.h>
#include <time.h>
#include <string.h>

/* ---------------------------- Platform identification ---------------------------- */
#if defined(_WIN32) || defined(_WIN64)
    #define DTT_OS_WINDOWS 1
    #define DTT_PLATFORM_NAME "Windows"
#elif defined(__APPLE__)
    #define DTT_OS_MACOS 1
    #define DTT_PLATFORM_NAME "macOS"
#elif defined(__linux__)
    #define DTT_OS_LINUX 1
    #define DTT_PLATFORM_NAME "Linux"
#else
    #define DTT_OS_UNKNOWN 1
    #define DTT_PLATFORM_NAME "Unknown"
#endif

/* ---------------------------- Architecture identification ------------------------ */
#if defined(_M_X64) || defined(__x86_64__)
    #define DTT_ARCH_NAME "x86_64"
#elif defined(_M_IX86) || defined(__i386__)
    #define DTT_ARCH_NAME "x86"
#elif defined(_M_ARM64) || defined(__aarch64__)
    #define DTT_ARCH_NAME "arm64"
#elif defined(__arm__) || defined(_M_ARM)
    #define DTT_ARCH_NAME "arm32"
#else
    #define DTT_ARCH_NAME "unknown"
#endif

/* ---------------------------- Mutex abstraction ----------------------------------
 * We deliberately avoid C11 <threads.h> (spotty compiler support, especially
 * on older Android NDK/macOS toolchains) and instead wrap the native
 * primitive for each OS behind five macros. */
#if defined(DTT_OS_WINDOWS)
    #include <windows.h>
    typedef CRITICAL_SECTION dtt_mutex_t;
    #define DTT_MUTEX_INIT(m)    InitializeCriticalSection(m)
    #define DTT_MUTEX_DESTROY(m) DeleteCriticalSection(m)
    #define DTT_MUTEX_LOCK(m)    EnterCriticalSection(m)
    #define DTT_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#else
    #include <pthread.h>
    typedef pthread_mutex_t dtt_mutex_t;
    #define DTT_MUTEX_INIT(m)    pthread_mutex_init((m), NULL)
    #define DTT_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
    #define DTT_MUTEX_LOCK(m)    pthread_mutex_lock(m)
    #define DTT_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------- Thread-local storage -------------------------------
 * Used for the "authorized transform" flag: the DTT Java runtime agent
 * marks its own thread as authorized immediately before it performs a
 * legitimate class redefinition, and clears it right after. Because
 * JVMTI's ClassFileLoadHook fires synchronously on the thread that
 * triggered the redefinition, a thread-local flag is sufficient (and
 * safer than a global flag, which could race with unrelated threads). */
#if defined(DTT_OS_WINDOWS) && defined(_MSC_VER)
    #define DTT_THREAD_LOCAL __declspec(thread)
#else
    /* MinGW-w64, gcc and clang all support __thread */
    #define DTT_THREAD_LOCAL __thread
#endif

/* ---------------------------- Millisecond wall clock ------------------------------
 * Used only for human-readable reason-log timestamps; never used for
 * timing-sensitive security decisions. */
#if defined(DTT_OS_WINDOWS)
static __inline jlong dtt_now_millis(void) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* FILETIME = 100ns ticks since 1601-01-01; convert to ms since 1970-01-01 */
    return (jlong)((uli.QuadPart / 10000ULL) - 11644473600000ULL);
}
#else
#include <sys/time.h>
static inline jlong dtt_now_millis(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (jlong)((jlong)tv.tv_sec * 1000LL + (jlong)tv.tv_usec / 1000LL);
}
#endif

#endif /* DTT_PLATFORM_H */
