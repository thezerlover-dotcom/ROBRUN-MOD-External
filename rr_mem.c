/* rr_mem.c - RobRun IOCTL transport and target-module lookup.
 *
 * Open the device, load its session key and send bounded memory requests.
 * The module walker reads the target PEB and loader entries so callers can
 * locate DLLs independently of the main executable image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdlib.h>

#include "rr_mem.h"
#include "rr_abi.h"

typedef NTSTATUS (WINAPI* fNtCF)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                 PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (WINAPI* fNtDIOC)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
                                   ULONG, PVOID, ULONG, PVOID, ULONG);

static HANDLE   g_dev    = NULL;
static ULONG    g_drvkey = 0;
static fNtDIOC  g_pdioc  = NULL;
static uint64_t g_peb    = 0;
/* Small-read status belongs to the calling thread: roster scanning and frame
 * sampling run concurrently and must not overwrite one another's result. */
static __declspec(thread) int g_ok = 0;

uint32_t g_rr_pid = 0;

/* Device. */

static int open_device(void) {
    HMODULE nd = GetModuleHandleW(L"ntdll.dll");
    fNtCF pCF = (fNtCF)GetProcAddress(nd, "NtCreateFile");
    if (!pCF) return 0;
    /* Try higher-numbered device names first because repeated mappings can
     * create suffixed names. */
    for (int n = 7; n >= 0 && !g_dev; n--) {
        wchar_t nt[64];
        const wchar_t base[] = RR_DEVICE_NAME;
        int i = 0;
        for (; base[i]; i++) nt[i] = base[i];
        nt[i++] = (wchar_t)(L'0' + n);
        nt[i] = 0;

        UNICODE_STRING un; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK ios;
        un.Buffer = (PWSTR)nt;
        un.Length = (USHORT)(wcslen(nt) * 2);
        un.MaximumLength = (USHORT)(un.Length + 2);
        InitializeObjectAttributes(&oa, &un, 0, NULL, NULL);
        HANDLE h = NULL;
        NTSTATUS st = pCF(&h, GENERIC_READ | GENERIC_WRITE, &oa, &ios, NULL, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
        if (st >= 0 && h) g_dev = h;
    }
    if (g_dev) return 1;
    /* Fall back to the Win32 name in case the symlink is present but the
     * \Device\ path was shadowed by a previous mapping. */
    g_dev = CreateFileW(RR_WIN32_NAME, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    return g_dev != NULL && g_dev != INVALID_HANDLE_VALUE;
}

static int load_key(void) {
    HKEY hk = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RR_REGISTRY_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS || !hk)
        return 0;
    DWORD v = 0, sz = sizeof(v), ty = 0;
    LONG r = RegQueryValueExW(hk, RR_REGISTRY_TOKEN, NULL, &ty, (LPBYTE)&v, &sz);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS || !v) return 0;
    g_drvkey = v;
    return 1;
}

int rr_drv_open(void) {
    if (g_dev && g_pdioc && g_drvkey) return 1;  /* already open */
    if (!open_device()) return 0;
    g_pdioc = (fNtDIOC)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile");
    if (!g_pdioc) { rr_drv_close(); return 0; }
    if (!load_key()) { rr_drv_close(); return 0; }
    return 1;
}

void rr_drv_close(void) {
    if (g_dev) { CloseHandle(g_dev); g_dev = NULL; }
    g_pdioc = NULL;
    g_peb = 0;
    g_ok = 0;
}

/* Batch. */

int rr_read_batch(RR_RD_REQ* items, int n) {
    size_t total = 0;
    int head = 0;
    size_t acc = 0;
    int a, b;

    if (!g_dev || !g_pdioc || !items || n <= 0 || n > RR_BATCH_MAX) return 0;

    for (int i = 0; i < n; i++) total += items[i].size;
    if (total == 0) return 0;

    /* Split batches that exceed the driver reply budget. Each returned item
     * status must still be checked before its bytes are used.
     */
    if (total > 0xF000u) {
        while (head < n && acc + items[head].size <= 0xF000u) acc += items[head].size, head++;
        if (head == 0) return 0;                 /* one item is over budget */
        if (head == n) return 0;                 /* no progress possible */
        a = rr_read_batch(items, head);
        b = rr_read_batch(items + head, n - head);
        if (a < 0 && b < 0) return -1;
        return (a > 0 ? a : 0) + (b > 0 ? b : 0);
    }

    size_t need = total + (size_t)n;
    if (need < sizeof(RR_BATCH)) need = sizeof(RR_BATCH);
    uint8_t* buf = (uint8_t*)malloc(need);
    if (!buf) return 0;
    memset(buf, 0, need);

    RR_BATCH* req = (RR_BATCH*)buf;
    req->magic_key = g_drvkey;
    req->process_id = g_rr_pid;
    req->count = (uint32_t)n;
    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        req->items[i].address = items[i].addr;
        req->items[i].size = items[i].size;
        req->items[i].offset = off;
        off += items[i].size;
    }
    req->total = off;

    IO_STATUS_BLOCK ios;
    memset(&ios, 0, sizeof(ios));
    NTSTATUS st = g_pdioc(g_dev, NULL, NULL, NULL, &ios, RR_IO_BATCH,
                          buf, (ULONG)sizeof(RR_BATCH),
                          buf, (ULONG)need);

    /* Expect total data bytes followed by count status bytes. Treat another
     * reply size as incompatible and let the caller use individual reads.
     */
    if (st < 0 || ios.Information != off + (ULONG)n) {
        free(buf);
        return -1;
    }

    const uint8_t* flags = buf + off;
    uint32_t data_off = 0;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (flags[i]) {
            memcpy(items[i].dst, buf + data_off, items[i].size);
            ok++;
        }
        data_off += items[i].size;
    }
    free(buf);
    return ok > 0 ? ok : -1;
}

/* Read. */

int rr_read(uint64_t addr, void* out, size_t n) {
    if (!g_dev || !g_pdioc || !out || n == 0 || n > RR_MAX_TRANSFER) return 0;
    RR_CMD c;
    c.magic_key = g_drvkey;
    c.process_id = g_rr_pid;
    c.address = (ULONG_PTR)addr;
    c.size = n;
    IO_STATUS_BLOCK ios;
    memset(&ios, 0, sizeof(ios));
    NTSTATUS st = g_pdioc(g_dev, NULL, NULL, NULL, &ios, RR_IO_READ,
                          &c, (ULONG)sizeof(c), out, (ULONG)n);
    if (st >= 0 && ios.Information == n) { g_ok = 1; return 1; }
    return 0;
}

int rr_write(uint64_t addr, const void* in, size_t n) {
    if (!g_dev || !g_pdioc || !in || n == 0 || n > RR_MAX_TRANSFER) return 0;
    size_t total = sizeof(RR_CMD) + n;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return 0;
    RR_CMD* c = (RR_CMD*)buf;
    c->magic_key = g_drvkey;
    c->process_id = g_rr_pid;
    c->address = (ULONG_PTR)addr;
    c->size = n;
    memcpy(buf + sizeof(RR_CMD), in, n);
    uint64_t dummy = 0;
    IO_STATUS_BLOCK ios;
    memset(&ios, 0, sizeof(ios));
    NTSTATUS st = g_pdioc(g_dev, NULL, NULL, NULL, &ios, RR_IO_WRITE,
                          buf, (ULONG)total, &dummy, (ULONG)sizeof(dummy));
    free(buf);
    return st >= 0;
}

int rr_ok(void) { return g_ok; }

uint64_t rr_u64(uint64_t a) { uint64_t v = 0; g_ok = 0; rr_read(a, &v, 8); return v; }
uint32_t rr_u32(uint64_t a) { uint32_t v = 0; g_ok = 0; rr_read(a, &v, 4); return v; }
int32_t  rr_i32(uint64_t a) { int32_t  v = 0; g_ok = 0; rr_read(a, &v, 4); return v; }
float    rr_f32(uint64_t a) { float    v = 0; g_ok = 0; rr_read(a, &v, 4); return v; }

/* Process. */

typedef struct _RR_PE32 { DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags;
    CHAR szExeFile[MAX_PATH]; } RR_PE32;

int rr_process_visible(uint32_t pid) {
    if (!pid) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    RR_PE32 e;
    e.dwSize = sizeof(e);
    typedef BOOL (WINAPI* F)(HANDLE, RR_PE32*);
    F first = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32First");
    F next  = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32Next");
    int found = 0;
    if (first && first(snap, &e)) {
        do {
            if (e.th32ProcessID == pid) { found = 1; break; }
        } while (next && next(snap, &e));
    }
    CloseHandle(snap);
    return found;
}

int rr_attach(const char* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    RR_PE32 e;
    e.dwSize = sizeof(e);
    typedef BOOL (WINAPI* F)(HANDLE, RR_PE32*);
    F first = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32First");
    F next  = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32Next");
    int found = 0;
    if (first && first(snap, &e)) {
        do {
            if (!_stricmp(e.szExeFile, exe_name)) { g_rr_pid = e.th32ProcessID; found = 1; break; }
        } while (next && next(snap, &e));
    }
    CloseHandle(snap);
    return found;
}

int rr_query_region(uint64_t address, RR_REGION* out) {
    if (!g_dev || !g_pdioc || !out) return 0;
    RR_CMD c;
    c.magic_key = g_drvkey;
    c.process_id = g_rr_pid;
    c.address = (ULONG_PTR)address;
    c.size = 0;
    IO_STATUS_BLOCK ios;
    memset(&ios, 0, sizeof(ios));
    NTSTATUS st = g_pdioc(g_dev, NULL, NULL, NULL, &ios, RR_IO_REGION,
                          &c, (ULONG)sizeof(c), out, (ULONG)sizeof(*out));
    return st >= 0 && ios.Information == sizeof(*out);
}

static int ioctl_ptr(ULONG code, uint64_t* out) {
    if (!g_dev || !g_pdioc) return 0;
    RR_CMD c;
    c.magic_key = g_drvkey;
    c.process_id = g_rr_pid;
    c.address = 0;
    c.size = 0;
    IO_STATUS_BLOCK ios;
    memset(&ios, 0, sizeof(ios));
    *out = 0;
    NTSTATUS st = g_pdioc(g_dev, NULL, NULL, NULL, &ios, code, &c, (ULONG)sizeof(c),
                          out, (ULONG)sizeof(uint64_t));
    return st >= 0 && ios.Information == sizeof(uint64_t) && *out != 0;
}

uint64_t rr_image_base(void) {
    uint64_t b = 0;
    return ioctl_ptr(RR_IO_BASE, &b) ? b : 0;
}

static uint64_t rr_peb(void) {
    uint64_t b = 0;
    if (g_peb) return g_peb;
    if (ioctl_ptr(RR_IO_PEB, &b)) g_peb = b;
    return g_peb;
}

/* Windows x64 PEB/LDR offsets used by this reader. Verify on other builds:
 *   PEB +0x10: ImageBaseAddress; +0x18: Ldr
 *   LDR +0x10: InLoadOrderModuleList head
 *   Entry +0x00: Flink; +0x30: DllBase; +0x40: SizeOfImage
 *   Entry +0x58: BaseDllName (counted UNICODE_STRING).
 */
#define PL(v) ((v) >= 0x10000ULL && (v) < 0x00007FFFFFFFFFFFULL)

int rr_module(const char* name, uint64_t* base, uint32_t* size) {
    *base = 0;
    if (size) *size = 0;
    uint64_t peb = rr_peb();
    if (!peb) return 0;

    uint64_t ldr = rr_u64(peb + 0x18);
    if (!PL(ldr)) return 0;
    uint64_t head = ldr + 0x10;
    uint64_t cur  = rr_u64(head);
    if (!PL(cur)) return 0;

    wchar_t want[64];
    MultiByteToWideChar(CP_ACP, 0, name, -1, want, 64);

    for (int i = 0; i < 512; i++) {
        if (cur == head || !PL(cur)) break;

        /* BaseDllName is a UNICODE_STRING at entry+0x58 */
        USHORT blen = (USHORT)rr_u32(cur + 0x58);
        uint64_t bbuf = rr_u64(cur + 0x60);
        if (blen > 0 && blen < 512 && PL(bbuf)) {
            wchar_t got[256];
            size_t chars = blen / 2;
            if (chars > 255) chars = 255;
            if (rr_read(bbuf, got, chars * 2)) {
                got[chars] = 0;
                if (_wcsicmp(got, want) == 0) {
                    *base = rr_u64(cur + 0x30);
                    if (size) *size = rr_u32(cur + 0x40);
                    return PL(*base) ? 1 : 0;
                }
            }
        }
        cur = rr_u64(cur);
    }
    return 0;
}
