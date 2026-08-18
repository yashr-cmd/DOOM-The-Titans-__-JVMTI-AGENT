package runtime;

public final class NativeIntegrityBridge {

    private static volatile boolean loaded = false;

    private NativeIntegrityBridge() {
    }
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

    public static native boolean isNativeAgentActive();
    public static native int getAbsorbedThreadCount();
    public static native int getReflectedThreadCount();
    public static native void beginAuthorizedTransform(String classNameHint);
    public static native void endAuthorizedTransform();
    public static native String getPlatformInfo();


    public static void runAuthorized(String classNameHint, Runnable action) {
        beginAuthorizedTransform(classNameHint);
        try {
            action.run();
        } finally {
            endAuthorizedTransform();
        }
    }
}