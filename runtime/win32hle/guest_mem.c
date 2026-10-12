/*
 * guest_mem.c - the guest's address space, and a heap inside it.
 *
 * Lifted code holds 32-bit guest addresses (VAs). On a 32-bit flat host a VA
 * is a host address (g_mem_base is 0). On a 64-bit host the guest's 4 GB is
 * one reservation, g_mem_base is its host address, and recomp_types.h's ADDR
 * adds the two; no host pointer is ever handed to the guest, and nothing the
 * guest can see lives outside the reservation. This file is the one place
 * that knows which of the two the host is.
 *
 * Everything the guest can address comes through here: the image (pe_loader),
 * VirtualAlloc regions, guest thread stacks and TIBs (host_lite), the heap
 * (kernel32's HeapAlloc and the host's own hle_alloc), COM objects and their
 * vtables (ole32), surfaces (ddraw), sound buffers (dsound), and the few
 * constants a shim hands the guest (hle_guest_static).
 *
 * Layout of the guest space, as Windows lays a 32-bit process out:
 *   0x00000000  64 KB never mapped (a null pointer faults, atoms live here)
 *   0x00400000  the image, at its own base
 *   0x10000000  heap chunks, stacks, DLLs, VirtualAlloc(NULL): upwards
 *   0x7E000000  pseudo-modules and the shim VAs (host_lite): never memory
 * Nothing goes above 2 GB, so a game that treats pointers as signed is safe.
 *
 * The heap is a size-class allocator: small blocks from per-class free
 * lists carved out of 1 MB chunks, large ones as their own page-rounded
 * chunks. Each block has a 16-byte header (size, 'HEAP', class), so
 * HeapSize and _msize answer from it, and a free of something that is not a
 * block is caught. One mutex: guest code runs under the machine lock, but a
 * host thread (a mixer, a recorder) may allocate too.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include "win32hle.h"

#define GUEST_LOW   0x00010000u          /* below: never mapped */
#define GUEST_DYN   0x10000000u          /* where anything without an address goes */
#define GUEST_TOP   0x7E000000u          /* the pseudo-modules and shim VAs start here */
#define GRAN        0x10000u             /* allocation granularity, as Windows' */
#define CHUNK       (1u << 20)           /* a heap chunk */
#define HDR         16u
#define MAGIC       0x48454150u          /* 'HEAP' */

/* One lock, recursive: the heap holds it while it asks the region layer for
 * a chunk, and a free hands a large block back through the same layer. */
static pthread_mutex_t g_lock;
static pthread_once_t  g_lock_once = PTHREAD_ONCE_INIT;
static void lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
    pthread_mutexattr_destroy(&a);
}
static void lock(void)   { pthread_once(&g_lock_once, lock_init); pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }
static int g_inited;
static size_t g_page;

/* ---- the reservation ------------------------------------------------- */

static int is64(void) { return sizeof(void *) > 4; }

static void init_locked(void) {
    if (g_inited) return;
    g_inited = 1;
    g_page = (size_t)sysconf(_SC_PAGESIZE);
    if (!is64()) { g_mem_base = 0; return; }             /* flat: a VA is a host address */
    /* The whole 4 GB, read-write but untouched (so unbacked), except the
     * first 64 KB, which stays inaccessible so a guest null dereference
     * faults as it would on Windows. */
    void *m = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { fprintf(stderr, "[guest] cannot reserve 4 GB of address space\n"); abort(); }
    if (mprotect((uint8_t *)m + GUEST_LOW, 0x100000000ull - GUEST_LOW, PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "[guest] cannot make the guest space writable\n"); abort();
    }
    g_mem_base = (ptrdiff_t)(uintptr_t)m;
}
static void init(void) { lock(); init_locked(); unlock(); }
int hle_guest_init(void) { init(); return 0; }

void *hle_g2h(uint32_t va) {
    /* Small values pass through: 0 stays NULL and a Win32 atom (a class name
     * below 0x10000) stays an atom, so `if (!p)` and `(uintptr_t)p < 0x10000`
     * mean the same on both hosts. */
    if (va < GUEST_LOW) return (void *)(uintptr_t)va;
    if (!g_inited) init();
    return (void *)ADDR(va);
}
uint32_t hle_h2g(const void *p) {
    uintptr_t h = (uintptr_t)p;
    if (h < GUEST_LOW) return (uint32_t)h;
    if (!g_inited) init();
    uintptr_t off = h - (uintptr_t)g_mem_base;
    if (off >= 0x100000000ull || off < GUEST_LOW) {
        fprintf(stderr, "[guest] host pointer %p is not in guest memory (in sub_%08X, import %s)\n", p, g_cur_func,
                g_cur_import ? g_cur_import : "(none)");
        abort();
    }
    return (uint32_t)off;
}

/* ---- regions: who has which part of the space ------------------------ */

#define MAX_REGIONS 4096
static struct { uint32_t va, size; } g_region[MAX_REGIONS];
static int g_nregion;
static uint32_t g_cursor = GUEST_DYN;

static int overlaps(uint32_t va, uint32_t size) {
    for (int i = 0; i < g_nregion; i++)
        if (va < g_region[i].va + g_region[i].size && g_region[i].va < va + size) return 1;
    return 0;
}
static int region_add(uint32_t va, uint32_t size) {
    if (g_nregion >= MAX_REGIONS) return 0;
    g_region[g_nregion].va = va, g_region[g_nregion].size = size;
    g_nregion++;
    return 1;
}
static uint32_t round_gran(uint32_t v) { return (v + GRAN - 1) & ~(GRAN - 1); }

/* Memory at a fixed VA (an image at its base, VirtualAlloc with an address):
 * the VA, or 0 if that range is taken. */
uint32_t hle_guest_map(uint32_t va, uint32_t size) {
    init();
    uint32_t lo = va & ~(GRAN - 1), hi = round_gran(va + size);
    if (lo < GUEST_LOW || hi > GUEST_TOP || hi <= lo) return 0;
    lock();
    if (overlaps(lo, hi - lo) || !region_add(lo, hi - lo)) { unlock(); return 0; }
    unlock();
    if (!is64()) {
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
        void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED || (uint32_t)(uintptr_t)p != lo) { if (p != MAP_FAILED) munmap(p, hi - lo); hle_guest_unmap(lo, hi - lo); return 0; }
    }
    return va;
}
/* Memory anywhere: a 64 KB-aligned VA, zero-filled. */
uint32_t hle_guest_map_any(uint32_t size) {
    init();
    size = round_gran(size);
    if (!size) return 0;
    if (!is64()) {
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return 0;
        lock();
        region_add((uint32_t)(uintptr_t)p, size);
        unlock();
        return (uint32_t)(uintptr_t)p;
    }
    lock();
    uint32_t va = g_cursor;
    for (int tries = 0; tries < MAX_REGIONS; tries++) {  /* first fit from the cursor, wrapping once */
        if (va + size > GUEST_TOP || va + size < va) { va = GUEST_DYN; }
        if (!overlaps(va, size)) break;
        /* step past the region in the way */
        uint32_t next = 0xFFFFFFFFu;
        for (int i = 0; i < g_nregion; i++)
            if (va < g_region[i].va + g_region[i].size && g_region[i].va < va + size) {
                uint32_t e = g_region[i].va + g_region[i].size;
                if (e < next) next = e;
            }
        va = round_gran(next);
    }
    if (overlaps(va, size) || va + size > GUEST_TOP || !region_add(va, size)) {
        unlock();
        fprintf(stderr, "[guest] out of guest address space (%u bytes)\n", size);
        return 0;
    }
    g_cursor = va + size;
    unlock();
    memset((void *)ADDR(va), 0, size);                   /* a reused range may hold old bytes */
    return va;
}
void hle_guest_unmap(uint32_t va, uint32_t size) {
    uint32_t lo = va & ~(GRAN - 1), hi = round_gran(va + size);
    lock();
    for (int i = 0; i < g_nregion; i++)
        if (g_region[i].va == lo) { g_region[i] = g_region[--g_nregion]; break; }
    unlock();
    if (!is64()) munmap((void *)(uintptr_t)lo, hi - lo);
    else hle_guest_decommit(lo, hi - lo);
}
/* Give the pages back; they read as zero when touched again. */
void hle_guest_decommit(uint32_t va, uint32_t size) {
    if (!g_inited) return;
    uintptr_t a = ADDR(va), e = ADDR(va) + size;
    a = (a + g_page - 1) & ~(g_page - 1);
    e &= ~(g_page - 1);
    if (e > a) madvise((void *)a, e - a, MADV_DONTNEED);
}
int hle_guest_region_of(uint32_t va, uint32_t *base, uint32_t *size) {
    int found = 0;
    lock();
    for (int i = 0; i < g_nregion && !found; i++)
        if (va >= g_region[i].va && va - g_region[i].va < g_region[i].size) {
            if (base) *base = g_region[i].va;
            if (size) *size = g_region[i].size;
            found = 1;
        }
    unlock();
    return found;
}

/* ---- the heap -------------------------------------------------------- */

/* Size classes: 16-byte steps to 256, then a 3/4-geometric ladder to 256 KB. */
static const uint32_t g_class_size[] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024,
    1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384,
    20480, 24576, 28672, 32768, 40960, 49152, 57344, 65536, 81920, 98304, 114688, 131072, 163840, 196608,
    229376, 262144 };
#define NCLASS ((int)(sizeof g_class_size / sizeof *g_class_size))
#define LARGE  0xFFFFu

static uint32_t g_free[NCLASS];         /* free lists: the VA of the first free block's user area */
static uint32_t g_chunk_va, g_chunk_left;
static struct { uint32_t va, size; } g_large_free[256];  /* freed large blocks, whole */
static int g_nlarge_free;

static int class_of(uint32_t n) {
    int lo = 0, hi = NCLASS;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_class_size[mid] < n) lo = mid + 1; else hi = mid; }
    return lo < NCLASS ? lo : -1;
}
static uint32_t hdr_size(uint32_t user)  { return MEM32(user - HDR); }
static uint32_t hdr_magic(uint32_t user) { return MEM32(user - HDR + 4); }
static uint32_t hdr_class(uint32_t user) { return MEM32(user - HDR + 8); }
static void     hdr_set(uint32_t user, uint32_t size, uint32_t cls) { MEM32(user - HDR) = size; MEM32(user - HDR + 4) = MAGIC; MEM32(user - HDR + 8) = cls; MEM32(user - HDR + 12) = 0; }

/* Carve `bytes` (16-aligned, header included) out of the current chunk. */
static uint32_t carve(uint32_t bytes) {
    if (g_chunk_left < bytes) {
        uint32_t want = bytes > CHUNK ? round_gran(bytes) : CHUNK;
        uint32_t va = hle_guest_map_any(want);
        if (!va) return 0;
        g_chunk_va = va, g_chunk_left = want;
    }
    uint32_t va = g_chunk_va;
    g_chunk_va += bytes, g_chunk_left -= bytes;
    return va;
}

static uint32_t alloc_locked(uint32_t n, int zero) {
    int c = class_of(n);
    if (c >= 0) {
        uint32_t user = g_free[c];
        if (user) {
            g_free[c] = MEM32(user);                     /* the next free block, kept in the first word */
            hdr_set(user, n, (uint32_t)c);
            if (zero) memset((void *)ADDR(user), 0, g_class_size[c]);
            return user;
        }
        uint32_t blk = carve(HDR + g_class_size[c]);
        if (!blk) return 0;
        hdr_set(blk + HDR, n, (uint32_t)c);              /* fresh pages are zero already */
        return blk + HDR;
    }
    /* large: a mapping of its own, reused whole when one of a close size was freed */
    uint32_t need = round_gran(HDR + n);
    for (int i = 0; i < g_nlarge_free; i++)
        if (g_large_free[i].size >= need && g_large_free[i].size <= need + need / 2) {
            uint32_t va = g_large_free[i].va;
            g_large_free[i] = g_large_free[--g_nlarge_free];
            if (zero) memset((void *)ADDR(va), 0, need);
            hdr_set(va + HDR, n, LARGE);
            return va + HDR;
        }
    uint32_t va = hle_guest_map_any(need);
    if (!va) return 0;
    hdr_set(va + HDR, n, LARGE);
    return va + HDR;
}

uint32_t hle_guest_alloc(uint32_t n) { return hle_guest_alloc2(n, 1); }
uint32_t hle_guest_alloc2(uint32_t n, int zero) {
    init();
    lock();
    uint32_t va = alloc_locked(n ? n : 1, zero);
    unlock();
    if (!va) fprintf(stderr, "[guest] heap: cannot allocate %u bytes\n", n);
    return va;
}
int hle_guest_block_ok(uint32_t user) {
    return user >= GUEST_LOW + HDR && hdr_magic(user) == MAGIC;
}
uint32_t hle_guest_size(uint32_t user) { return hle_guest_block_ok(user) ? hdr_size(user) : 0; }
uint32_t hle_guest_capacity(uint32_t user) {
    if (!hle_guest_block_ok(user)) return 0;
    uint32_t c = hdr_class(user), size = 0;
    if (c != LARGE) return g_class_size[c];
    hle_guest_region_of(user - HDR, NULL, &size);
    return size ? size - HDR : hdr_size(user);
}

static void free_locked(uint32_t user) {
    uint32_t c = hdr_class(user);
    MEM32(user - HDR + 4) = 0;                           /* no longer a block */
    if (c == LARGE) {
        uint32_t base = user - HDR, size = 0;
        hle_guest_region_of(base, NULL, &size);
        if (g_nlarge_free < 256 && size) { g_large_free[g_nlarge_free].va = base, g_large_free[g_nlarge_free].size = size; g_nlarge_free++; hle_guest_decommit(base + g_page, size > g_page ? size - g_page : 0); }
        else hle_guest_unmap(base, size);
        return;
    }
    MEM32(user) = g_free[c];
    g_free[c] = user;
}
void hle_guest_free(uint32_t user) {
    if (!user) return;
    if (!hle_guest_block_ok(user)) {
        fprintf(stderr, "[guest] free of 0x%08X, which this heap did not hand out (in sub_%08X)\n", user, g_cur_func);
        return;
    }
    lock();
    free_locked(user);
    unlock();
}
uint32_t hle_guest_realloc(uint32_t user, uint32_t n, int zero) {
    if (!user) return hle_guest_alloc2(n, zero);
    if (!hle_guest_block_ok(user)) return 0;
    uint32_t old = hdr_size(user), c = hdr_class(user);
    uint32_t cap = c == LARGE ? 0 : g_class_size[c];
    if (c == LARGE) { uint32_t size = 0; hle_guest_region_of(user - HDR, NULL, &size); cap = size ? size - HDR : old; }
    if (n <= cap) {                                      /* fits where it is */
        if (zero && n > old) memset((void *)ADDR(user + old), 0, n - old);
        lock();
        hdr_set(user, n, c);
        unlock();
        return user;
    }
    uint32_t nu = hle_guest_alloc2(n, 0);
    if (!nu) return 0;
    memcpy((void *)ADDR(nu), (void *)ADDR(user), old);
    if (zero) memset((void *)ADDR(nu + old), 0, n - old);
    hle_guest_free(user);
    return nu;
}

/* A constant the guest reads (a driver's name, an IID): copied into guest
 * memory once, for the run. The same host pointer gives the same VA. */
uint32_t hle_guest_static(const void *data, uint32_t n) {
    static struct { const void *host; uint32_t va; } memo[256];
    static int nmemo;
    lock();
    for (int i = 0; i < nmemo; i++)
        if (memo[i].host == data) { uint32_t va = memo[i].va; unlock(); return va; }
    unlock();
    uint32_t va = hle_guest_alloc2(n, 0);
    if (!va) return 0;
    memcpy((void *)ADDR(va), data, n);
    lock();
    if (nmemo < 256) memo[nmemo].host = data, memo[nmemo++].va = va;
    unlock();
    return va;
}
uint32_t hle_guest_strdup(const char *s) { return hle_guest_static(s, (uint32_t)strlen(s) + 1); }

/* Host-pointer faces of the heap, for the shims that hold a block as a
 * pointer (a surface's pixels, a sound buffer's samples). */
void *hle_alloc(uint32_t n) { uint32_t va = hle_guest_alloc2(n, 1); return va ? (void *)ADDR(va) : NULL; }
void *hle_alloc_raw(uint32_t n) { uint32_t va = hle_guest_alloc2(n, 0); return va ? (void *)ADDR(va) : NULL; }
void  hle_free(void *p) { if (p) hle_guest_free(hle_h2g(p)); }
