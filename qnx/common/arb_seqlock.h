/*
 * A sequence lock for one ring slot in shared memory (review M2-J8): one writer
 * that never waits, readers that never block it and never accept a torn copy,
 * on any memory model C11 allows, not only x86's.
 *
 * A slot is a sequence word and the record as 64-bit words, all C11 atomics, so
 * no access is a data race. The writer makes the sequence odd, fences, stores
 * the words, then publishes the next even value (release). A reader loads the
 * sequence (acquire; odd means being written), loads the words, fences
 * (acquire), loads the sequence again: the copy is whole only if both loads saw
 * the same even value. This is the C11 sequence lock of H.-J. Boehm, "Can
 * seqlocks get along with programming language memory models?" (MSPC 2012): the
 * writer's release fence before the word stores pairs with the reader's acquire
 * fence after the word loads, so a reader that saw any new word also sees the
 * odd (or newer) sequence on its second load.
 *
 * The QNX 8.0 shared-memory pages state no cross-process ordering rule, so this
 * relies on C11 atomics on lock-free 64-bit words in memory shared through
 * shm_open()/mmap() (checked: ATOMIC_LLONG_LOCK_FREE == 2 on x86_64 and aarch64).
 * Bounded: one pass over at most ARB_SEQ_MAX_WORDS words.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
 */
#ifndef ARB_SEQLOCK_H
#define ARB_SEQLOCK_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ARB_SEQ_MAX_WORDS 16u          /* the largest record: 128 bytes */

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics must be lock-free for shared memory");
_Static_assert(sizeof(_Atomic uint64_t) == 8, "an atomic word is 8 bytes");

/* The writer (one per ring): store n words of src into the slot. */
static inline void arb_seq_write(_Atomic uint64_t *seq, _Atomic uint64_t *w, const void *src, size_t n)
{
    uint64_t tmp[ARB_SEQ_MAX_WORDS];
    const uint64_t s = atomic_load_explicit(seq, memory_order_relaxed);

    memcpy(tmp, src, n * sizeof tmp[0]);
    atomic_store_explicit(seq, s + 1u, memory_order_relaxed);          /* odd: being written */
    atomic_thread_fence(memory_order_release);
    for (size_t i = 0; i < n; i++) {                                    /* bound: n <= ARB_SEQ_MAX_WORDS */
        atomic_store_explicit(&w[i], tmp[i], memory_order_relaxed);
    }
    atomic_store_explicit(seq, (s | 1u) + 1u, memory_order_release);   /* even: complete */
}

/* A reader: copy n words of the slot into dst. 1 if the copy is whole, 0 if the
 * slot was being written meanwhile (dst is then left unchanged). The slot may sit
 * in a read-only mapping: only loads are made (the casts only drop const). */
static inline int arb_seq_read(const _Atomic uint64_t *seq, const _Atomic uint64_t *w, void *dst, size_t n)
{
    uint64_t tmp[ARB_SEQ_MAX_WORDS];
    uint64_t s1, s2;

    s1 = atomic_load_explicit((_Atomic uint64_t *)seq, memory_order_acquire);
    if (s1 & 1u) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {                                    /* bound: n <= ARB_SEQ_MAX_WORDS */
        tmp[i] = atomic_load_explicit((_Atomic uint64_t *)&w[i], memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_acquire);
    s2 = atomic_load_explicit((_Atomic uint64_t *)seq, memory_order_relaxed);
    if (s1 != s2) {
        return 0;
    }
    memcpy(dst, tmp, n * sizeof tmp[0]);
    return 1;
}

#endif /* ARB_SEQLOCK_H */
