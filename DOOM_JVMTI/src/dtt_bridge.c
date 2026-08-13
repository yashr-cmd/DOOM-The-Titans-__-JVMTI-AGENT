/* ============================================================================
 * dtt_bridge.c - see dtt_bridge.h for design notes.
 * ============================================================================ */

#include "../include/dtt_bridge.h"
#include "../include/dtt_agent.h"
#include "../include/dtt_score.h"
#include "../include/dtt_auth.h"
#include "../include/dtt_config.h"
#include "../include/dtt_log.h"

#include <string.h>

JNIEXPORT jboolean JNICALL Java_runtime_NativeIntegrityBridge_isNativeAgentActive(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return dtt_agent_is_active() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_runtime_NativeIntegrityBridge_getIntegrityScore(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    if (!dtt_agent_is_active()) {
        return 0; /* fail-safe default when the native layer never came up */
    }
    return (jint)dtt_score_get();
}

JNIEXPORT jstring JNICALL Java_runtime_NativeIntegrityBridge_getIntegrityStatus(JNIEnv *env, jclass clazz) {
    (void)clazz;
    if (!dtt_agent_is_active()) {
        return (*env)->NewStringUTF(env, "NORMAL");
    }
    return (*env)->NewStringUTF(env, dtt_status_name(dtt_score_get_status()));
}

JNIEXPORT jobjectArray JNICALL Java_runtime_NativeIntegrityBridge_getReasonCodes(JNIEnv *env, jclass clazz, jint maxEntries) {
    jclass string_class;
    jobjectArray result;
    char lines[DTT_REASON_LOG_CAPACITY][256];
    int count;
    int i;
    int requested;

    (void)clazz;

    string_class = (*env)->FindClass(env, "java/lang/String");
    if (string_class == NULL) {
        return NULL; /* should never happen; JNI will have an exception pending */
    }

    if (!dtt_agent_is_active()) {
        return (*env)->NewObjectArray(env, 0, string_class, NULL);
    }

    requested = (int)maxEntries;
    if (requested <= 0) {
        requested = DTT_REASON_LOG_CAPACITY;
    }
    if (requested > DTT_REASON_LOG_CAPACITY) {
        requested = DTT_REASON_LOG_CAPACITY;
    }

    count = dtt_score_snapshot_reasons(lines, requested);

    result = (*env)->NewObjectArray(env, count, string_class, NULL);
    if (result == NULL) {
        return NULL;
    }

    for (i = 0; i < count; i++) {
        jstring line = (*env)->NewStringUTF(env, lines[i]);
        (*env)->SetObjectArrayElement(env, result, i, line);
        (*env)->DeleteLocalRef(env, line);
    }

    return result;
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

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_configureThresholds(JNIEnv *env, jclass clazz, jint suspicious, jint highRisk, jint critical) {
    (void)env;
    (void)clazz;

    if (!dtt_agent_is_active()) {
        return;
    }

    dtt_score_configure_thresholds((int)suspicious, (int)highRisk, (int)critical);
}

JNIEXPORT jstring JNICALL Java_runtime_NativeIntegrityBridge_getPlatformInfo(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return (*env)->NewStringUTF(env, dtt_agent_platform_string());
}
