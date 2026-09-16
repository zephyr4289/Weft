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
#include "fanout.h"

// FFI struct budget guard (referenced by packages/flutter_weft/lib/src/
// bindings.dart): the Dart side reserves 512 bytes for weft_t storage.
// If a kernel change grows the struct past this reservation, fail the
// build here rather than corrupting memory silently on the Dart side.
_Static_assert(sizeof(weft_t) <= 512, "weft_t exceeds the Dart FFI 512-byte WeftStruct reservation");

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

// ===========================================================================
// Fan-out ring (RFC 0004) — JNI surface over core/c/fanout.{h,c}
// ===========================================================================
// WHY EXISTS: The Triad kernel is 1-writer/1-reader by design; RFC 0004 adds
// the driver-layer multi-consumer ring. The C ring is BYTE-COMPATIBLE with
// core/ts/fanout.ts (layout contract in fanout.h), so one ring serves TS
// (SharedArrayBuffer), Android (this JNI surface), and Flutter (Dart FFI).
// Until this section, Android could not fan out at all.
//
// THREAD DISCIPLINE: none of these entries ever calls back into the JVM
// (no NewStringUTF, no exceptions, no monitor enter), so no
// AttachCurrentThread is ever required — any Java thread may call any
// entry. The PROTOCOL contracts still apply: one writer thread at a time
// (begin/fill/publish from the writer thread only — same contract as the
// kernel's writer), N independent readers from any threads.
//
// DIRECT-BYTEBUFFER LIFETIME (the caller-visible contract):
//   - fanoutBegin() returns a direct ByteBuffer VIEW over the live slot.
//     It is valid until the broadcaster's NEXT begin() (the cursor moves)
//     and dangles after fanoutDestroy() freed the ring — the same stance
//     as weftWriterBuffer() above for the kernel.
//   - fanoutViewBuffer() returns the reader-OWNED copy buffer; stable
//     identity for the reader's lifetime, contents replaced by each claim.
//   - The ByteBuffer objects themselves are ordinary JVM locals/references;
//     the C side never retains one.
//
// Handles are jlong (0 = invalid / allocation failure — panic-shielded,
// never an exception). Lifecycle: fanoutCreate -> fanoutDestroy is the
// broadcaster pair; fanoutReaderCreate* -> fanoutReaderDestroy the reader
// pair; all destroys are idempotent (NULL-safe in C).

// --- Broadcaster lifecycle ---

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutCreate(JNIEnv *env, jobject thiz, jint payloadBytes, jint slotCount) {
    (void)env; (void)thiz;
    if (payloadBytes <= 0 || slotCount <= 0) return 0;
    weft_fanout_t *f = weft_fanout_new((size_t)payloadBytes, (unsigned)slotCount);
    return (jlong)(uintptr_t)f;
}

// Foreign-ring broadcaster: calloc an EMPTY broadcaster and attach it to
// byte-compatible memory produced by another port (e.g. a TS
// SharedArrayBuffer session shared via a direct ByteBuffer). Frame
// numbering continues from the ring's latestSeq (producer handoff).
JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutCreateForeign(JNIEnv *env, jobject thiz, jobject ringBuf,
                                              jint payloadBytes, jint slotCount) {
    (void)thiz;
    if (!ringBuf || payloadBytes <= 0 || slotCount <= 0) return 0;
    void *ring = (*env)->GetDirectBufferAddress(env, ringBuf);
    jlong cap = (*env)->GetDirectBufferCapacity(env, ringBuf);
    if (!ring || cap <= 0) return 0;
    weft_fanout_t *f = (weft_fanout_t*)calloc(1, sizeof(weft_fanout_t));
    if (!f) return 0;
    if (weft_fanout_attach_writer(f, ring, (size_t)cap,
                                  (size_t)payloadBytes, (unsigned)slotCount) != 0) {
        free(f); // geometry mismatch — fails fast instead of tearing
        return 0;
    }
    return (jlong)(uintptr_t)f;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutRingBytes(JNIEnv *env, jobject thiz, jint payloadBytes, jint slotCount) {
    (void)env; (void)thiz;
    if (payloadBytes <= 0 || slotCount <= 0) return 0;
    return (jlong)weft_fanout_ring_bytes((size_t)payloadBytes, (unsigned)slotCount);
}

JNIEXPORT void JNICALL
Java_dev_weft_TriadNative_fanoutDestroy(JNIEnv *env, jobject thiz, jlong fanoutHandle) {
    (void)env; (void)thiz;
    weft_fanout_free((weft_fanout_t*)(uintptr_t)fanoutHandle); // NULL-safe, frees ring + struct
}

// --- Broadcaster hot path (writer thread only) ---

JNIEXPORT jobject JNICALL
Java_dev_weft_TriadNative_fanoutBegin(JNIEnv *env, jobject thiz, jlong fanoutHandle) {
    (void)thiz;
    weft_fanout_t *f = (weft_fanout_t*)(uintptr_t)fanoutHandle;
    if (!f) return NULL;
    uint8_t *slot = weft_fanout_begin(f); // invalidates the slot stamp FIRST (FI1 bracket)
    if (!slot) return NULL;
    return (*env)->NewDirectByteBuffer(env, slot, (jlong)f->payload_bytes);
}

JNIEXPORT jint JNICALL
Java_dev_weft_TriadNative_fanoutFill(JNIEnv *env, jobject thiz, jlong fanoutHandle,
                                     jobject srcBuf, jint len) {
    (void)thiz;
    weft_fanout_t *f = (weft_fanout_t*)(uintptr_t)fanoutHandle;
    if (!f || !srcBuf || len < 0) return -1;
    void *src = (*env)->GetDirectBufferAddress(env, srcBuf);
    jlong cap = (*env)->GetDirectBufferCapacity(env, srcBuf);
    if (!src || cap <= 0 || (jlong)len > cap) return -1;
    return weft_fanout_fill(f, src, (size_t)len); // words written, or -1
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutPublish(JNIEnv *env, jobject thiz, jlong fanoutHandle) {
    (void)env; (void)thiz;
    weft_fanout_t *f = (weft_fanout_t*)(uintptr_t)fanoutHandle;
    if (!f) return 0;
    return (jlong)weft_fanout_publish(f); // frame seq (0 if no begin — detectable no-op)
}

// --- Broadcaster advisory state (AXIOM T: advisory, never correctness) ---

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutLatestSeq(JNIEnv *env, jobject thiz, jlong fanoutHandle) {
    (void)env; (void)thiz;
    weft_fanout_t *f = (weft_fanout_t*)(uintptr_t)fanoutHandle;
    if (!f) return 0;
    weft_fanout_debug_t d;
    weft_fanout_debug_stats(f, &d);
    return (jlong)d.latest_seq;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutPublishes(JNIEnv *env, jobject thiz, jlong fanoutHandle) {
    (void)env; (void)thiz;
    weft_fanout_t *f = (weft_fanout_t*)(uintptr_t)fanoutHandle;
    if (!f) return 0;
    weft_fanout_debug_t d;
    weft_fanout_debug_stats(f, &d);
    return (jlong)d.publishes;
}

// --- Reader lifecycle ---

// In-process fan-out: attach a reader to the broadcaster's own ring (the
// RFC-0004 motivating call — one writer, N consumers across Android threads).
JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutReaderCreate(JNIEnv *env, jobject thiz, jlong fanoutHandle) {
    (void)env; (void)thiz;
    weft_fanout_t *f = (weft_fanout_t*)(uintptr_t)fanoutHandle;
    if (!f || !f->ring) return 0;
    const size_t ring_bytes = weft_fanout_ring_bytes(f->payload_bytes, f->slot_count);
    weft_fanout_reader_t *r =
        weft_fanout_reader_new(f->ring, ring_bytes, f->payload_bytes, f->slot_count);
    return (jlong)(uintptr_t)r;
}

// Foreign ring: attach to byte-compatible memory produced by another port
// (e.g. a TS SharedArrayBuffer session shared via a direct ByteBuffer).
JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutReaderCreateForeign(JNIEnv *env, jobject thiz, jobject ringBuf,
                                                    jint payloadBytes, jint slotCount) {
    (void)thiz;
    if (!ringBuf || payloadBytes <= 0 || slotCount <= 0) return 0;
    void *ring = (*env)->GetDirectBufferAddress(env, ringBuf);
    jlong cap = (*env)->GetDirectBufferCapacity(env, ringBuf);
    if (!ring || cap <= 0) return 0;
    weft_fanout_reader_t *r =
        weft_fanout_reader_new(ring, (size_t)cap, (size_t)payloadBytes, (unsigned)slotCount);
    return (jlong)(uintptr_t)r;
}

JNIEXPORT void JNICALL
Java_dev_weft_TriadNative_fanoutReaderDestroy(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_free((weft_fanout_reader_t*)(uintptr_t)readerHandle); // NULL-safe
}

// --- Reader hot path (any thread; one reader per thread by discipline) ---
// The claim record is READER-OWNED and identity-stable until that reader's
// next claim, so claim()/claimFresh()/claimDropped() read back one
// consistent claim across three JNI transitions.

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutClaim(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    if (!r) return 0;
    return (jlong)weft_fanout_claim(r)->seq;
}

JNIEXPORT jboolean JNICALL
Java_dev_weft_TriadNative_fanoutClaimFresh(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    if (!r) return JNI_FALSE;
    return r->rec.fresh ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutClaimDropped(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    if (!r) return 0;
    return (jlong)r->rec.dropped;
}

// Zero-copy view of the reader's copy buffer (payload_bytes capacity): the
// draw-phase read path — claim(), then read this buffer live.
JNIEXPORT jobject JNICALL
Java_dev_weft_TriadNative_fanoutViewBuffer(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    if (!r || !r->target) return NULL;
    return (*env)->NewDirectByteBuffer(env, r->target, (jlong)r->payload_bytes);
}

// --- Reader advisory statistics (cold path; AXIOM T) ---

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutReaderStatsReads(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    weft_fanout_stats_t s;
    if (!r) return 0;
    weft_fanout_reader_stats(r, &s);
    return (jlong)s.reads;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutReaderStatsFresh(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    weft_fanout_stats_t s;
    if (!r) return 0;
    weft_fanout_reader_stats(r, &s);
    return (jlong)s.fresh;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutReaderStatsDrops(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    weft_fanout_stats_t s;
    if (!r) return 0;
    weft_fanout_reader_stats(r, &s);
    return (jlong)s.drops;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutReaderStatsSkipped(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    weft_fanout_stats_t s;
    if (!r) return 0;
    weft_fanout_reader_stats(r, &s);
    return (jlong)s.skipped_mid_overwrite;
}

JNIEXPORT jlong JNICALL
Java_dev_weft_TriadNative_fanoutReaderStatsExhausted(JNIEnv *env, jobject thiz, jlong readerHandle) {
    (void)env; (void)thiz;
    weft_fanout_reader_t *r = (weft_fanout_reader_t*)(uintptr_t)readerHandle;
    weft_fanout_stats_t s;
    if (!r) return 0;
    weft_fanout_reader_stats(r, &s);
    return (jlong)s.torn_exhausted;
}
