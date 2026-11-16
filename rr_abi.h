/* rr_abi.h - Wire contract shared by the RobRun driver and the overlay.
 *
 * Request numbers use ((type << 16) | (function << 2)), METHOD_BUFFERED.
 * Keep this header as the single definition of the request layout; both
 * sides are built from it.
 */
#ifndef RR_ABI_H
#define RR_ABI_H

#if defined(RR_KERNEL)
#include <ntddk.h>
#else
#include <windows.h>
#endif

#define RR_ABI_TYPE     0x3BUL
#define RR_IOCTL(fn)    ((ULONG)((RR_ABI_TYPE << 16) | (((ULONG)(fn)) << 2)))

#define RR_IO_READ      RR_IOCTL(0x901)   /* read target memory            */
#define RR_IO_BASE      RR_IOCTL(0x902)   /* image base of the target      */
#define RR_IO_DETACH    RR_IOCTL(0x903)   /* remove the DOS symbolic link  */
#define RR_IO_WRITE     RR_IOCTL(0x904)   /* write target memory           */
#define RR_IO_PEB       RR_IOCTL(0x905)   /* PEB of the target             */
#define RR_IO_REGION    RR_IOCTL(0x906)   /* one region descriptor         */
#define RR_IO_BATCH     RR_IOCTL(0x907)   /* up to 64 reads in one call    */

#define RR_MAX_TRANSFER 0x10000
#define RR_BATCH_MAX    64

#define RR_DEVICE_NAME   L"\\Device\\RobRun"
#define RR_SYMLINK_NAME  L"\\??\\RobRun"
#define RR_WIN32_NAME    L"\\\\.\\RobRun"
#define RR_DRIVER_NAME   L"\\Driver\\RobRun"
#define RR_REGISTRY_PATH L"\\Registry\\Machine\\SYSTEM\\RobRun"
#define RR_REGISTRY_KEY  L"SYSTEM\\RobRun"
#define RR_REGISTRY_TOKEN L"RingToken"

#pragma pack(push, 1)
typedef struct _RR_CMD {
    ULONG     magic_key;
    ULONG     process_id;
    ULONG_PTR address;
    SIZE_T    size;
} RR_CMD, *PRR_CMD;

typedef struct _RR_BATCH_ITEM {
    ULONGLONG address;
    ULONG     size;
    ULONG     offset;
} RR_BATCH_ITEM, *PRR_BATCH_ITEM;

typedef struct _RR_BATCH {
    ULONG          magic_key;
    ULONG          process_id;
    ULONG          count;
    ULONG          total;          /* reply data bytes from offset 0 */
    RR_BATCH_ITEM  items[RR_BATCH_MAX];
    /* The reply contains `total` data bytes followed by `count` status
     * bytes. The reply overwrites the request buffer. */
} RR_BATCH, *PRR_BATCH;

typedef struct _RR_REGION {
    ULONG_PTR base;
    ULONG_PTR size;
    ULONG     state;
    ULONG     protect;
    ULONG     type;
    ULONG     pad;
} RR_REGION, *PRR_REGION;
#pragma pack(pop)

#endif /* RR_ABI_H */
