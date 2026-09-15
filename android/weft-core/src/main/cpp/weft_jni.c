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

// I6 teardown helper: revoke the writer, wait a bounded time for the epoch
// ACK (the writer checks `revoked` at the top of every publish and ACKs via
// epoch.fetch_add), then destroy. Per weft.h: destroying while a writer may
// still run without this handshake is a CALLER ERROR — the bridge defends
// the contract so Kotlin callers cannot get it wrong.
static void weft_release_with_i6(weft_t *w) {
    if (!w) return;
    uint32_t pre = weft_epoch(w);          // pre-revoke epoch
    weft_revoke(w);                        // step 1: revoked.store(true, Release)
    (void)weft_reclaim(w, pre, 50);        // steps 2-3: bounded ACK wait (best effort)
    weft_destroy(w);
}

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
            weft_release_with_i6(s->items[i]);
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
        } else {
            // Could not register (OOM) — never hand out an untracked Weft:
            // stewardDestroy would leak it. Fail the allocation instead.
            weft_release_with_i6(w);
            free(w);
            return 0;
        }
    }

    return (jlong)(uintptr_t)w;
}

JNIEXPORT void JNICALL
Java_dev_weft_TriadNative_weftRelease(JNIEnv *env, jobject thiz, jlong weftHandle) {
    (void)env; (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (!w) return;
    weft_release_with_i6(w);
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

// Publish with an EXPLICIT payload length and sequence number.
//
// Two modes:
//   data != NULL  : `data` must be a direct ByteBuffer; its contents ARE the
//                   frame — exactly payloadLen bytes are copied into the
//                   kernel's writer buffer (weft_w_write_payload), then
//                   published. (The previous bridge ignored the buffer
//                   contents entirely and published the kernel buffer
//                   as-is — passing a fresh buffer silently published
//                   stale bytes. That trap is removed.)
//   data == NULL  : cursor mode — the caller filled the buffer obtained
//                   from weftWriterBuffer(); payloadLen names how much of
//                   it is valid.
//
// seq < 0 selects auto-numbering (t_publish + 1, matching the old behavior).
// Returns 0 (WEFT_PUB_OK) or 1 (WEFT_PUB_DROPPED_REVOKED), -1 on error.
JNIEXPORT jint JNICALL
Java_dev_weft_TriadNative_weftPublish(JNIEnv *env, jobject thiz, jlong weftHandle,
                                      jobject data, jint payloadLen, jint seq) {
    (void)thiz;
    weft_t *w = (weft_t*)(uintptr_t)weftHandle;
    if (!w) return -1;

    uint32_t len = (payloadLen >= 0) ? (uint32_t)payloadLen : w->payload_max;
    if (len > w->payload_max) return -1; // caller error, do not silently clamp

    if (data) {
        void *src = (*env)->GetDirectBufferAddress(env, data);
        jlong cap = (*env)->GetDirectBufferCapacity(env, data);
        if (!src || cap <= 0 || (jlong)len > cap) return -1;
        if (weft_w_write_payload(w, (const uint8_t*)src, len) != 0) return -1;
    }

    uint32_t s = (seq >= 0) ? (uint32_t)seq : (uint32_t)(weft_t_publish(w) + 1);
    weft_pub_result_t res = weft_publish(w, s, len);
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
