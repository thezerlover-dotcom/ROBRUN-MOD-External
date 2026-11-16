/* rr_unity.h - Managed-object helpers for the supported Unity IL2CPP layout.
 *
 * Object: class pointer at +0x00, monitor at +0x08, instance fields at +0x10.
 * String: length at +0x10, UTF-16 characters at +0x14.
 * Array: length at +0x18, elements at +0x20.
 * Dictionary: entries at +0x18, used count at +0x20.
 *
 * Class-layout constants below describe the runtime used for this snapshot.
 * They are not a compatibility guarantee for other Unity builds. Managed
 * game-field offsets are maintained separately in the generated headers.
 */
#ifndef RR_UNITY_H
#define RR_UNITY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_CLASS_NAME        0x10u
#define U_STR_LEN           0x10u
#define U_STR_CHARS         0x14u
#define U_ARR_LEN           0x18u
#define U_ARR_DATA          0x20u
#define U_DICT_ENTRIES      0x18u
#define U_DICT_COUNT        0x20u

#define PL(v) ((v) >= 0x10000ULL && (v) < 0x00007FFFFFFFFFFFULL)

/* Il2CppClass* of an object. */
uint64_t u_klass(uint64_t obj);

/* "Namespace.Type" (or just "Type" when the namespace is empty). 0 on a bad
 * read. The result is a C string in out. */
int u_class_name(uint64_t klass, char* out, size_t n);

/* Compares an object's class against "Namespace.Type" (namespace optional). */
int u_is(uint64_t obj, const char* qualified);

/* Convert an IL2CPP UTF-16 string to UTF-8 and NUL-terminate out. Return
 * the bytes written, or zero for an empty, invalid or unreadable string.
 */
int u_string(uint64_t str, char* out, size_t n);

/* Read a dictionary used-slot count and the first-entry address.
 * stride_hint receives 0x18 for the supported int/reference layout. Other key
 * or value types require their own verified stride and field offsets.
 */
int u_dict(uint64_t dict, int* count, uint64_t* entries, int* stride_hint);

#ifdef __cplusplus
}
#endif

#endif /* RR_UNITY_H */
