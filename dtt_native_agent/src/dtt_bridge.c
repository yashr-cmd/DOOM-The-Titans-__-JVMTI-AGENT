/* ============================================================================
 * dtt_bridge.c - see dtt_bridge.h for design notes.
 * ============================================================================ */

#include "../include/dtt_bridge.h"
#include "../include/dtt_agent.h"
#include "../include/dtt_threads.h"
#include "../include/dtt_auth.h"
#include "../include/dtt_log.h"

#include <string.h>

JNIEXPORT jboolean JNICALL Java_runtime_NativeIntegrityBridge_isNativeAgentActive(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return dtt_agent_is_active() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_runtime_NativeIntegrityBridge_getAbsorbedThreadCount(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    if (!dtt_agent_is_active()) {
        return 0; /* fail-safe default when the native layer never came up */
    }
    return (jint)dtt_threads_absorbed_thread_count();
}

JNIEXPORT jint JNICALL Java_runtime_NativeIntegrityBridge_getReflectedThreadCount(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    if (!dtt_agent_is_active()) {
        return 0; /* fail-safe default when the native layer never came up */
    }
    return (jint)dtt_threads_reflected_event_count();
}

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_beginAuthorizedTransform(JNIEnv *env, jclass clazz, jstring classNameHint) {
    const char *hint;

    (void)clazz;

    if (!dtt_agent_is_active()) {
        return;
    }

    dtt_auth_begin();

    if (classNameHint != NULL) {
        hint = (*env)->GetStringUTFChars(env, classNameHint, NULL);
        if (hint != NULL) {
            dtt_log(DTT_LOG_DEBUG, "authorized-transform window opened for '%s'", hint);
            (*env)->ReleaseStringUTFChars(env, classNameHint, hint);
        }
    } else {
        dtt_log(DTT_LOG_DEBUG, "authorized-transform window opened (no class hint provided)");
    }
}

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_endAuthorizedTransform(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;

    if (!dtt_agent_is_active()) {
        return;
    }

    dtt_auth_end();
    dtt_log(DTT_LOG_DEBUG, "authorized-transform window closed");
}

JNIEXPORT jstring JNICALL Java_runtime_NativeIntegrityBridge_getPlatformInfo(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return (*env)->NewStringUTF(env, dtt_agent_platform_string());
}
