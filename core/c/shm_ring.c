// shm_ring.c — inter-process shared-memory rings, POSIX + Windows.
//
// POSix layout: one shm object, ftruncate'd to HEADER + ring_bytes, mapped
// MAP_SHARED. The header is assembled byte-wise little-endian (no packed
// structs — the same discipline as the kernel's envelope encode). Anonymous
// sessions skip the object entirely (MAP_ANONYMOUS|MAP_SHARED, fork-
// inherited). Attach validates everything the header claims against the
// object's real size before any ring pointer escapes.
//
// Windows layout (compile-gated _WIN32): a named pagefile-backed file
// mapping (CreateFileMappingA(INVALID_HANDLE_VALUE, ...)); the "mapping
// size" check reads the header's ring_bytes and validates the mapping's
// claimed size was honored (MapViewOfFile exposes the full mapping; the
// creator sized it, attachers trust the header + the view length via
// VirtualQuery). Compiled by the windows CI leg; not executed in this
// sandbox — declared.
//
// Layer discipline: driver layer; weft.c/weft.h untouched; no allocation
// beyond the mapping itself; no new library dependencies (RTLD for
// shm_open on Linux — the Makefile adds -lrt where needed).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // clock_gettime, shm_open under strict C11
#endif

#include "shm_ring.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Header encode/decode — byte-wise, little-endian, no packed structs
// ---------------------------------------------------------------------------

static void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16le(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u64le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t unix_ns_now(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000ull) * 100ull;  // 100ns ticks since 1970
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static int header_write(uint8_t* base, size_t payload_bytes, unsigned slot_count) {
    memset(base, 0, WEFT_SHM_HEADER_BYTES);
    put_u32le(base + 0, WEFT_SHM_MAGIC);
    put_u16le(base + 4, (uint16_t)WEFT_SHM_VERSION);
    put_u16le(base + 6, (uint16_t)WEFT_SHM_HEADER_BYTES);
    put_u32le(base + 8, 0);  // flags
    put_u32le(base + 12, (uint32_t)payload_bytes);
    put_u32le(base + 16, (uint32_t)slot_count);
    put_u64le(base + 20, (uint64_t)weft_fanout_ring_bytes(payload_bytes, slot_count));
#if defined(_WIN32)
    put_u32le(base + 28, (uint32_t)GetCurrentProcessId());
#else
    put_u32le(base + 28, (uint32_t)getpid());
#endif
    put_u64le(base + 32, unix_ns_now());
    return 0;
}

/// Validate a mapped session header against the real mapping size.
/// Returns 0/-1; never guesses — a mismatched object is refused.
static int header_validate(const uint8_t* base, size_t mapping_bytes) {
    if (mapping_bytes < WEFT_SHM_HEADER_BYTES) return -1;
    if (get_u32le(base + 0) != WEFT_SHM_MAGIC) return -1;
    if (get_u16le(base + 4) != WEFT_SHM_VERSION) return -1;
    if (get_u16le(base + 6) != WEFT_SHM_HEADER_BYTES) return -1;
    if (get_u32le(base + 8) != 0) return -1;  // unknown flag bits = reject
    const uint32_t payload_bytes = get_u32le(base + 12);
    const uint32_t slot_count = get_u32le(base + 16);
    const uint64_t ring_bytes = get_u64le(base + 20);
    if (ring_bytes != weft_fanout_ring_bytes(payload_bytes, slot_count)) return -1;
    // Reserved bytes [40, 64) must be zero (version discipline).
    for (size_t i = 40; i < WEFT_SHM_HEADER_BYTES; i++) {
        if (base[i] != 0) return -1;
    }
    if ((uint64_t)mapping_bytes != (uint64_t)WEFT_SHM_HEADER_BYTES + ring_bytes) return -1;
    return 0;
}

static int name_valid(const char* name) {
    if (name == NULL) return 0;
    size_t n = 0;
    while (name[n] != '\0') {
        const char c = name[n];
        const int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
        n++;
        if (n > 80) return 0;
    }
    if (n == 0) return 0;
    if (name[0] == '-') return 0;
    return 1;
}

static void map_init(weft_shm_map_t* m, uint8_t* base, size_t mapping_bytes,
                     int fd, int creator, const char* name) {
    m->base = base;
    m->ring = base + WEFT_SHM_HEADER_BYTES;
    m->mapping_bytes = mapping_bytes;
    m->fd = fd;
    m->creator = creator;
    memset(m->name, 0, sizeof(m->name));
    if (name != NULL) {
        strncpy(m->name, name, sizeof(m->name) - 1);
    }
}

// ---------------------------------------------------------------------------
// POSIX implementation
// ---------------------------------------------------------------------------

#if !defined(_WIN32)

/// shm_open wants a leading '/'; our public names never carry one.
static int shm_open_named(const char* name, int oflag, mode_t mode) {
    char path[96];
    snprintf(path, sizeof(path), "/%s", name);
    return shm_open(path, oflag, mode);
}

int weft_shm_create_named(const char* name, size_t payload_bytes,
                          unsigned slot_count, weft_shm_map_t* out) {
    if (!name_valid(name)) return -1;
    // Geometry is validated by the ring-bytes identity; an invalid pair
    // yields ring_bytes == 0 and is refused below.
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0 || payload_bytes > 0xFFFFFFFFu || slot_count > 0xFFFFFFFFu) return -1;
    const size_t mapping_bytes = (size_t)WEFT_SHM_HEADER_BYTES + rb;

    const int fd = shm_open_named(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)mapping_bytes) != 0) {
        close(fd);
        return -1;
    }
    void* p = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return -1;
    }
    uint8_t* base = (uint8_t*)p;
    // Zero the whole region (ftruncate already gives zeros; this keeps the
    // invariant explicit for the fd-passing road) then write the header.
    memset(base, 0, mapping_bytes);
    header_write(base, payload_bytes, slot_count);
    map_init(out, base, mapping_bytes, fd, 1, name);
    return 0;
}

int weft_shm_attach_named(const char* name, weft_shm_map_t* out, int read_only) {
    if (!name_valid(name)) return -1;
    const int fd = shm_open_named(name, read_only ? O_RDONLY : O_RDWR, 0);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)WEFT_SHM_HEADER_BYTES) {
        close(fd);
        return -1;
    }
    const size_t mapping_bytes = (size_t)st.st_size;
    void* p = mmap(NULL, mapping_bytes, read_only ? PROT_READ : (PROT_READ | PROT_WRITE),
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return -1;
    }
    if (header_validate((const uint8_t*)p, mapping_bytes) != 0) {
        munmap(p, mapping_bytes);
        close(fd);
        return -1;
    }
    map_init(out, (uint8_t*)p, mapping_bytes, fd, 0, name);
    return 0;
}

int weft_shm_create_anon(size_t payload_bytes, unsigned slot_count,
                         weft_shm_map_t* out) {
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0) return -1;
    const size_t mapping_bytes = (size_t)WEFT_SHM_HEADER_BYTES + rb;
    void* p = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    uint8_t* base = (uint8_t*)p;
    memset(base, 0, mapping_bytes);
    header_write(base, payload_bytes, slot_count);
    map_init(out, base, mapping_bytes, -1, 1, "");
    return 0;
}

int weft_shm_attach_fd(int fd, weft_shm_map_t* out, int read_only) {
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)WEFT_SHM_HEADER_BYTES) return -1;
    const size_t mapping_bytes = (size_t)st.st_size;
    void* p = mmap(NULL, mapping_bytes, read_only ? PROT_READ : (PROT_READ | PROT_WRITE),
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return -1;
    if (header_validate((const uint8_t*)p, mapping_bytes) != 0) {
        munmap(p, mapping_bytes);
        return -1;
    }
    map_init(out, (uint8_t*)p, mapping_bytes, fd, 0, "");
    return 0;
}

int weft_shm_unlink(const char* name) {
    if (!name_valid(name)) return -1;
    char path[96];
    snprintf(path, sizeof(path), "/%s", name);
    const int rc = shm_unlink(path);
    return rc == 0 ? 0 : (errno == ENOENT ? 0 : -1);
}

void weft_shm_destroy(weft_shm_map_t* m) {
    if (m == NULL || m->base == NULL) return;
    munmap(m->base, m->mapping_bytes);
    if (m->fd >= 0) close(m->fd);
    if (m->creator && m->name[0] != '\0') {
        weft_shm_unlink(m->name);  // crash leaves the object: declared
    }
    memset(m, 0, sizeof(*m));
}

// ---------------------------------------------------------------------------
// Windows implementation (compile-gated; windows CI compiles it)
// ---------------------------------------------------------------------------

#else  // _WIN32

int weft_shm_create_named(const char* name, size_t payload_bytes,
                          unsigned slot_count, weft_shm_map_t* out) {
    if (!name_valid(name)) return -1;
    const size_t rb = weft_fanout_ring_bytes(payload_bytes, slot_count);
    if (rb == 0 || payload_bytes > 0xFFFFFFFFu || slot_count > 0xFFFFFFFFu) return -1;
    const size_t mapping_bytes = (size_t)WEFT_SHM_HEADER_BYTES + rb;

    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, (DWORD)mapping_bytes, name);
    if (h == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (h != NULL) CloseHandle(h);
        return -1;
    }
    uint8_t* base = (uint8_t*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, mapping_bytes);
    if (base == NULL) {
        CloseHandle(h);
        return -1;
    }
    memset(base, 0, mapping_bytes);
    header_write(base, payload_bytes, slot_count);
    map_init(out, base, mapping_bytes, (int)(intptr_t)h, 1, name);
    return 0;
}

int weft_shm_attach_named(const char* name, weft_shm_map_t* out, int read_only) {
    if (!name_valid(name)) return -1;
    HANDLE h = OpenFileMappingA(read_only ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS,
                                FALSE, name);
    if (h == NULL) return -1;
    // Map the full mapping; the view length is what the creator sized.
    uint8_t* base = (uint8_t*)MapViewOfFile(
        h, read_only ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (base == NULL) {
        CloseHandle(h);
        return -1;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(base, &mbi, sizeof(mbi)) != sizeof(mbi)) {
        UnmapViewOfFile(base);
        CloseHandle(h);
        return -1;
    }
    if (header_validate(base, (size_t)mbi.RegionSize) != 0) {
        UnmapViewOfFile(base);
        CloseHandle(h);
        return -1;
    }
    map_init(out, base, (size_t)mbi.RegionSize, (int)(intptr_t)h, 0, name);
    return 0;
}

int weft_shm_create_anon(size_t payload_bytes, unsigned slot_count,
                         weft_shm_map_t* out) {
    (void)payload_bytes;
    (void)slot_count;
    (void)out;
    return -1;  // no fork to inherit it — declared in the header
}

int weft_shm_attach_fd(int fd, weft_shm_map_t* out, int read_only) {
    (void)fd;
    (void)out;
    (void)read_only;
    return -1;  // POSIX fd-passing road; use named mappings on Windows
}

int weft_shm_unlink(const char* name) {
    // Named file mappings vanish with the last handle — nothing to unlink.
    (void)name;
    return 0;
}

void weft_shm_destroy(weft_shm_map_t* m) {
    if (m == NULL || m->base == NULL) return;
    UnmapViewOfFile(m->base);
    if (m->fd >= 0) CloseHandle((HANDLE)(intptr_t)m->fd);
    memset(m, 0, sizeof(*m));
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Geometry accessors + fan-out bindings
// ---------------------------------------------------------------------------

size_t weft_shm_payload_bytes(const weft_shm_map_t* m) {
    return m && m->base ? get_u32le(m->base + 12) : 0;
}

unsigned weft_shm_slot_count(const weft_shm_map_t* m) {
    return m && m->base ? get_u32le(m->base + 16) : 0;
}

size_t weft_shm_ring_bytes(const weft_shm_map_t* m) {
    return m && m->base ? (size_t)get_u64le(m->base + 20) : 0;
}

const void* weft_shm_ring(const weft_shm_map_t* m) {
    return m ? (const void*)m->ring : NULL;
}

int weft_fanout_shm_create(const char* name, size_t payload_bytes,
                           unsigned slot_count, weft_fanout_t* f, weft_shm_map_t* m) {
    memset(f, 0, sizeof(*f));
    if (weft_shm_create_named(name, payload_bytes, slot_count, m) != 0) return -1;
    if (weft_fanout_attach_writer(f, m->ring, m->mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                  payload_bytes, slot_count) != 0) {
        weft_shm_destroy(m);
        return -1;
    }
    return 0;
}

int weft_fanout_shm_attach_writer(const char* name, weft_fanout_t* f,
                                  weft_shm_map_t* m) {
    memset(f, 0, sizeof(*f));
    if (weft_shm_attach_named(name, m, 0) != 0) return -1;
    const size_t pb = weft_shm_payload_bytes(m);
    const unsigned slots = weft_shm_slot_count(m);
    if (weft_fanout_attach_writer(f, m->ring, m->mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                  pb, slots) != 0) {
        weft_shm_destroy(m);
        return -1;
    }
    return 0;
}

int weft_fanout_shm_attach_reader(const char* name, weft_fanout_reader_t* r,
                                  weft_shm_map_t* m, int read_only) {
    memset(r, 0, sizeof(*r));
    if (weft_shm_attach_named(name, m, read_only) != 0) return -1;
    const size_t pb = weft_shm_payload_bytes(m);
    const unsigned slots = weft_shm_slot_count(m);
    if (weft_fanout_reader_init(r, m->ring, m->mapping_bytes - WEFT_SHM_HEADER_BYTES,
                                pb, slots) != 0) {
        weft_shm_destroy(m);
        return -1;
    }
    return 0;
}
