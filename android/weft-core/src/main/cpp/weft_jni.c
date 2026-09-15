// weft_jni.c — JNI bridge implementation for dev.weft.TriadNative
//
// WHY EXISTS: Bridges Kotlin/JVM callers to the frozen C kernel (core/c/weft.c)
// per 02-KERNEL §4 and WHITEPAPER §8.6.
//
// SAFETY & PANIC SHIELDING:
// Every JNI entry checks pointers / bounds before dereference.
// No uncaught C exceptions cross into the JVM.

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "weft.h"

typedef struct {
    weft_t** items;
    size_t count;
    size_t capacity;
} steward_native_t;

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_stewardCreate(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    steward_native_t *s = (steward_native_t*)calloc(1, sizeof(steward_native_t));
    return (jlong)(uintptr_t)s;
}

JNIEXPORT void JNICALL
Java_dev_weft_TriadNative_stewardDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    steward_native_t *s = (steward_native_t*)(uintptr_t)handle;
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) {
        if (s->items[i]) {
            weft_destroy(s->items[i]);
            free(s->items[i]);
        }
    }
    free(s->items);
    free(s);
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_stewardWeft(JNIEnv *env, jobject thiz, jlong stewardHandle, jint elemSize, jint capacity, jint align) {
    (void)env; (void)thiz; (void)align;
    steward_native_t *s = (steward_native_t*)(uintptr_t)stewardHandle;
    size_t payload_max = (size_t)(elemSize * capacity);
    if (payload_max == 0) payload_max = 64;

    weft_t *w = (weft_t*)calloc(1, sizeof(weft_t));
    if (!w) return 0;

    if (weft_init(w, payload_max) != 0) {
        free(w);
        return 0;
    }

    if (s) {
        if (s->count == s->capacity) {
            size_t new_cap = s->capacity == 0 ? 4 : s->capacity * 2;
            weft_t **new_items = (weft_t**)realloc(s->items, new_cap * sizeof(weft_t*));
            if (new_items) {
                s->items = new_items;
                s->capacity = new_cap;
            }
        }
        if (s->count < s->capacity) {
            s->items[s->count++] = w;
        }
    }

    return (jlong)(uintptr_t)w;
}

JNIEXPORT void JNICALL
Java_dev_weft_TriadNative_weftRelease(JNIEnv *env, jobject thiz, jlong weftHandle) {
    (void)env; (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (!w) return;
    weft_destroy(w);
    free(w);
}

JNIEXPORT jobject JNICALL
Java_dev_weft_TriadNative_weftWriterBuffer(JNIEnv *env, jobject thiz, jlong weftHandle) {
    (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (!w) return NULL;
    uint8_t *buf = weft_w_begin(w);
    if (!buf) return NULL;
    return (*env)->NewDirectByteBuffer(env, buf, (jlong)w->payload_max);
}

JNIEXPORT jint JNICALL
Java_dev_weft_TriadNative_weftPublish(JNIEnv *env, jobject thiz, jlong weftHandle, jobject data) {
    (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (!w) return -1;
    jlong capacity = 0;
    if (data) {
        capacity = (*env)->GetDirectBufferCapacity(env, data);
    }
    uint32_t payload_len = (uint32_t)(capacity > 0 ? capacity : w->payload_max);
    if (payload_len > w->payload_max) payload_len = (uint32_t)w->payload_max;

    uint32_t seq = (uint32_t)(weft_t_publish(w) + 1);
    weft_pub_result_t res = weft_publish(w, seq, payload_len);
    return (jint)res;
}

JNIEXPORT jboolean JNICALL
Java_dev_weft_TriadNative_weftRead(JNIEnv *env, jobject thiz, jlong weftHandle, jobject out) {
    (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (!w) return JNI_FALSE;
    uint32_t slot = weft_r_claim(w);
    (void)slot;

    if (out) {
        void *out_ptr = (*env)->GetDirectBufferAddress(env, out);
        jlong out_cap = (*env)->GetDirectBufferCapacity(env, out);
        if (out_ptr && out_cap > 0) {
            size_t copy_len = (size_t)out_cap;
            if (copy_len > w->payload_max) copy_len = w->payload_max;
            weft_r_read_slice(w, (uint8_t*)out_ptr, 16, copy_len);
        }
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_dev_weft_TriadNative_weftRevoke(JNIEnv *env, jobject thiz, jlong weftHandle) {
    (void)env; (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (w) weft_revoke(w);
}

JNIEXPORT jboolean JNICALL
Java_dev_weft_TriadNative_weftReclaim(JNIEnv *env, jobject thiz, jlong weftHandle, jint preRevokeEpoch, jint timeoutMs) {
    (void)env; (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (!w) return JNI_FALSE;
    int res = weft_reclaim(w, (uint32_t)preRevokeEpoch, (uint32_t)(timeoutMs > 0 ? timeoutMs : 100));
    return res == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_weftPublishCount(JNIEnv *env, jobject thiz, jlong weftHandle) {
    (void)env; (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    return w ? (jlong)weft_t_publish(w) : 0;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_weftReadCount(JNIEnv *env, jobject thiz, jlong weftHandle) {
    (void)env; (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    return w ? (jlong)weft_t_claim(w) : 0;
}
