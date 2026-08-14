package runtime;

/**
 * Java-side counterpart of dtt_bridge.c / dtt_bridge.h in the DTT native
 * integrity agent (C, JVMTI). Drop this class into the {@code runtime}
 * package of runtime-agent-NoNative alongside RuntimeAgent, ArmorLockGuard,
 * GodHelper, etc.
 *
 * <p>This class does NOT implement any logic itself - every method is a
 * native call into the compiled dtt_agent.{dll,so,dylib}. It is the ONLY
 * class that should call directly into the native layer; the rest of the
 * Java runtime agent should go through it rather than calling
 * System.loadLibrary/native methods directly, so the native library load
 * path stays centralized in one place.
 *
 * <p>Loading the native library is intentionally NOT done in a static
 * initializer here - if the native library fails to load (missing file,
 * wrong platform, etc.) a static initializer failure would poison this
 * class for the rest of the JVM's lifetime (ExceptionInInitializerError
 * on every subsequent use). Instead, call {@link #tryLoad(String)} once,
 * early, from RuntimeAgent's own startup path, and check its return value.
 * If it returns false, treat the native layer as simply unavailable and
 * keep running with Java-only protections - matching the native agent's
 * own fail-safe philosophy.
 *
 * <p>There is no integrity "score" anymore. The native agent's job is to
 * watch for anything trying to touch the mod's protected classes and then
 * <b>absorb</b> the offending thread (silently neutralize every attempt
 * it makes with known-good bytecode) or, for repeat offenders on the same
 * class, <b>reflect</b> the attempt back onto the offending thread itself.
 * It never kills, suspends, stops, or interrupts any thread.
 */
public final class NativeIntegrityBridge {

    private static volatile boolean loaded = false;

    private NativeIntegrityBridge() {
    }

    /**
     * Attempts to load the native library from an explicit path (recommended -
     * resolve the correct dtt_agent.dll / libdtt_agent.so / libdtt_agent.dylib
     * for the current OS/arch yourself, e.g. next to the mod jar). Returns
     * true on success. Never throws - any failure is caught and logged via
     * AgentLog, and the bridge simply stays "not loaded" so every method
     * below returns its fail-safe default.
     */
    public static synchronized boolean tryLoad(String nativeLibraryPath) {
        if (loaded) {
            return true;
        }
        try {
            System.load(nativeLibraryPath);
            loaded = true;
        } catch (Throwable t) {
            loaded = false;
        }
        return loaded;
    }

    public static boolean isLoaded() {
        return loaded;
    }

    // -------------------------------------------------------------------
    // Native methods - implemented in dtt_bridge.c
    // -------------------------------------------------------------------

    /** True once the native agent finished initialization successfully. */
    public static native boolean isNativeAgentActive();

    /**
     * Number of distinct threads the native agent has flagged ("absorbed")
     * because they tried to touch a protected DTT class. Every further
     * attempt from such a thread is silently neutralized with known-good
     * bytecode. 0 if the native layer isn't active.
     */
    public static native int getAbsorbedThreadCount();

    /**
     * Number of times a repeat offender (same thread, same protected class)
     * had its tampering attempt reflected back onto its own thread. 0 if
     * the native layer isn't active.
     */
    public static native int getReflectedThreadCount();

    /**
     * Call immediately before this JVM thread performs a legitimate
     * redefinition/retransformation of a protected DTT class (e.g. via
     * RuntimePatch), so the native layer doesn't flag its own runtime
     * agent's patches as tampering. Always pair with
     * {@link #endAuthorizedTransform()}, ideally in a try/finally.
     */
    public static native void beginAuthorizedTransform(String classNameHint);

    /** Closes the authorized-transform window opened by beginAuthorizedTransform. */
    public static native void endAuthorizedTransform();

    /** e.g. "Windows/x86_64" - which native binary actually got loaded. */
    public static native String getPlatformInfo();

    // -------------------------------------------------------------------
    // Convenience wrapper
    // -------------------------------------------------------------------

    /**
     * Runs {@code action} with the authorized-transform window open, and
     * guarantees the window is closed afterward even if action throws.
     * Prefer this over calling begin/end manually.
     */
    public static void runAuthorized(String classNameHint, Runnable action) {
        beginAuthorizedTransform(classNameHint);
        try {
            action.run();
        } finally {
            endAuthorizedTransform();
        }
    }
}
