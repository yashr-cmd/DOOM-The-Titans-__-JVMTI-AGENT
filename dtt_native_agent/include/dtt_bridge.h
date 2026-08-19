#ifndef DTT_BRIDGE_H
#define DTT_BRIDGE_H

#include <jni.h>

JNIEXPORT jboolean JNICALL Java_runtime_NativeIntegrityBridge_isNativeAgentActive(JNIEnv *env, jclass clazz);

JNIEXPORT jint JNICALL Java_runtime_NativeIntegrityBridge_getAbsorbedThreadCount(JNIEnv *env, jclass clazz);

JNIEXPORT jint JNICALL Java_runtime_NativeIntegrityBridge_getReflectedThreadCount(JNIEnv *env, jclass clazz);

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_beginAuthorizedTransform(JNIEnv *env, jclass clazz, jstring classNameHint);

JNIEXPORT void JNICALL Java_runtime_NativeIntegrityBridge_endAuthorizedTransform(JNIEnv *env, jclass clazz);

JNIEXPORT jstring JNICALL Java_runtime_NativeIntegrityBridge_getPlatformInfo(JNIEnv *env, jclass clazz);

#endif /* DTT_BRIDGE_H */