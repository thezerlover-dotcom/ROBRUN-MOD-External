/* rr_mem.h - Driver transport and target-memory access.
 *
 * The client opens the RobRun device, obtains its session key and exchanges
 * METHOD_BUFFERED IOCTL requests. Module lookup walks the target PEB loader
 * list, including GameAssembly.dll. The PEB offsets are layout assumptions
 * for the supported x64 build; verify them when changing Windows versions.
 */
#ifndef RR_MEM_H
#define RR_MEM_H

#include <stdint.h>
#include <stddef.h>

#include "rr_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle. */
int  rr_drv_open(void);          /* 1 = device open and session key loaded   */
void rr_drv_close(void);

/* Raw access: each individual request is limited to 64 KiB. */
int  rr_read (uint64_t addr, void* out, size_t n);
int  rr_write(uint64_t addr, const void* in, size_t n);

/* Read up to 64 address/size/destination items per call. Requests exceeding
 * the reply budget are split. Return the successful-item count, zero for an
 * invalid/empty request, or -1 for an incompatible or unsuccessful batch reply.
 * Only successful items are copied; callers can retry with rr_read().
 */
typedef struct RR_RD_REQ {
    uint64_t addr;
    uint32_t size;
    void*    dst;
} RR_RD_REQ;
int  rr_read_batch(RR_RD_REQ* items, int n);

/* Small-read helpers return zero on failure. Check rr_ok() to distinguish
 * a failed read from a successfully read zero.
 */
uint64_t rr_u64(uint64_t addr);
uint32_t rr_u32(uint64_t addr);
int32_t  rr_i32(uint64_t addr);
float    rr_f32(uint64_t addr);
int      rr_ok(void);            /* 1 = the last small read succeeded        */

/* Target process. */
extern uint32_t g_rr_pid;
int      rr_attach(const char* exe_name);   /* 0 = not running              */
/* One MemoryBasicInformation descriptor for the target process. Return 1
 * when the driver reports a region, 0 on failure. */
int      rr_query_region(uint64_t address, RR_REGION* out);
/* Return 1 if the PID appears in the process snapshot, 0 if absent, or
 * -1 if enumeration fails. A failed snapshot is not evidence of absence.
 */
int      rr_process_visible(uint32_t pid);
uint64_t rr_image_base(void);               /* PEB->ImageBaseAddress        */

/* Module lookup through the target PEB. name is case-insensitive, e.g.
 * "GameAssembly.dll". On success *base is the loaded base and *size the
 * SizeOfImage from the LDR entry. */
int rr_module(const char* name, uint64_t* base, uint32_t* size);

#ifdef __cplusplus
}
#endif

#endif /* RR_MEM_H */
