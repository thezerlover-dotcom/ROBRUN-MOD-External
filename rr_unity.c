/* rr_unity.c - IL2CPP object helpers (see rr_unity.h for the layout map). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "rr_mem.h"
#include "rr_unity.h"

uint64_t u_klass(uint64_t obj) {
    if (!PL(obj)) return 0;
    uint64_t k = rr_u64(obj);
    return PL(k) ? k : 0;
}

int u_class_name(uint64_t klass, char* out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!PL(klass)) return 0;
    uint64_t namep = rr_u64(klass + U_CLASS_NAME);
    if (!PL(namep)) return 0;
    /* Class names are NUL-terminated ASCII in the metadata, but a short name
     * can sit at the very end of a committed page: a 160-byte read then fails
     * with STATUS_PARTIAL_COPY even though the string is fine. Retry with
     * shorter windows instead of pretending the name is unreadable. */
    static const size_t tries[] = { 160, 96, 64, 32, 16, 8 };
    char tmp[160];
    size_t got = 0;
    for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
        size_t want = tries[t];
        if (want > sizeof(tmp)) want = sizeof(tmp);
        if (rr_read(namep, tmp, want)) { got = 0; break; }
        got = 1;
    }
    if (got) return 0;                 /* every window failed */
    tmp[sizeof(tmp) - 1] = 0;
    size_t i = 0;
    while (i + 1 < sizeof(tmp) && i + 1 < n && tmp[i]) { out[i] = tmp[i]; i++; }
    out[i] = 0;
    return i > 0;
}

int u_is(uint64_t obj, const char* qualified) {
    uint64_t k = u_klass(obj);
    if (!k || !qualified) return 0;
    char name[160];
    if (!u_class_name(k, name, sizeof(name))) return 0;

    /* Accept both "Type" and "Namespace.Type"; the runtime does not always
     * report the namespace the dump uses for nested or global types. */
    const char* dot = strrchr(qualified, '.');
    if (dot && !strcmp(name, dot + 1)) return 1;
    return !strcmp(name, qualified);
}

int u_string(uint64_t str, char* out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!PL(str)) return 0;
    int32_t len = rr_i32(str + U_STR_LEN);
    if (len <= 0 || len > 4096) return 0;
    wchar_t wbuf[1025];
    if (len > 1024) len = 1024;
    if (!rr_read(str + U_STR_CHARS, wbuf, (size_t)len * 2)) return 0;
    wbuf[len] = 0;
    int written = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, out, (int)n - 1, NULL, NULL);
    if (written <= 0) { out[0] = 0; return 0; }
    out[written < (int)n - 1 ? written : (int)n - 1] = 0;
    return written;
}

int u_dict(uint64_t dict, int* count, uint64_t* entries, int* stride_hint) {
    if (count) *count = 0;
    if (entries) *entries = 0;
    if (!PL(dict)) return 0;
    int32_t n = rr_i32(dict + U_DICT_COUNT);
    uint64_t ent = rr_u64(dict + U_DICT_ENTRIES);
    if (n <= 0 || n > 100000 || !PL(ent)) return 0;
    if (count) *count = n;
    if (entries) *entries = ent + U_ARR_DATA;
    if (stride_hint) *stride_hint = 0x18;   /* Dictionary<int, TRef> in IL2CPP */
    return n;
}
