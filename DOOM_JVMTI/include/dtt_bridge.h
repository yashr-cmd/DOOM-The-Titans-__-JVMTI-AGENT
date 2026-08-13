#ifndef DTT_BRIDGE_H
#define DTT_BRIDGE_H

#include <jni.h>

/* ============================================================================
 * dtt_bridge.h
 *
 * The small native-to-Java interface that lets the existing DTT Java
 * runtime agent (runtime-agent-NoNative) query this native layer's
 * current integrity score/status/reasons, and coordinate authorized
 * transforms so its own legitimate patches are never mistaken for an
 * attack.
 *
 * These functions are plain JNI native methods, meant to back a Java
 * class such as:
 *
 *   package runtime;
 *   public final class NativeIntegrityBridge {
 *       public static native boolean isNativeAgentActive();
 *       public static native int getIntegrityScore();
 *       public static native String getIntegrityStatus();
 *       public static native String[] getReasonCodes(int maxEntries);
 *       public static native void beginAuthorizedTransform(String classNameHint);
 *       public static native void endAuthorizedTransform();
 *       public static native void configureThresholds(int suspicious, int highRisk, int critical);
 *       public static native String getPlatformInfo();
 *   }
 *
 * (see java/runtime/NativeIntegrityBridge.java in this project for a
 * ready-to-drop-in copy). Java loads this native library itself
 * (System.load/loadLibrary) - these functions do not need to be
 * called manually from C.
 *
 * Every function here is fail-safe: if the native agent failed to
 * initialize (see dtt_agent.c), isNativeAgentActive() simply reports
 * false and the getters return harmless defaults (score 0, status
 * "NORMAL", empty reason array) rather than crashing or throwing,
 * so the Java-side agent can keep running Minecraft with reduced
 * (Java-only) protection instead of failing outright.
 * ============================================================================ */

JNIEXPORT jboolean JNICALL Java_runtime_NativeIntegrityBridge_isNativeAgentActive(JNIEnv *env, jclass clazz);

JNIEXPORT jint JNICALL Java_runtime_NativeIntegrityBridge_getIntegrityScore(JNIEnv *env, jclass clazz);

JNIEXPORT jstring JNICALL Java_runtime_NativeIntegrityBridge_getIntegrityStatus(JNIEnv *env, jclass clazz);

JNIEXPORT jobjectArray JNICALL Java_runtime_NativeIntegrityBridge_getReasonCodes(JNIEnv *env, jclass clazz, jint maxEntries);

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_beginAuthorizedTransform(JNIEnv *env, jclass clazz, jstring classNameHint);

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_endAuthorizedTransform(JNIEnv *env, jclass clazz);

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_configureThresholds(JNIEnv *env, jclass clazz, jint suspicious, jint highRisk, jint critical);

JNIEXPORT jstring JNICALL Java_runtime_NativeIntegrityBridge_getPlatformInfo(JNIEnv *env, jclass clazz);

#endif /* DTT_BRIDGE_H */
