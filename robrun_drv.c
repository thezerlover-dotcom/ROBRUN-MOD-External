/* robrun_drv.c - RobRun kernel transport.
 *
 * A single private device serves read, write, image-base, PEB, batched-read
 * and detach requests for one target process. DriverEntry starts a system
 * thread so object and registry work runs at PASSIVE_LEVEL in a clean
 * context, independent of the mapper that supplied the entry point.
 */
#define RR_KERNEL 1
#include <ntddk.h>

#include "rr_abi.h"

NTKERNELAPI PPEB PsGetProcessPeb(PEPROCESS Process);

NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId, PEPROCESS* Process);

NTKERNELAPI NTSTATUS MmCopyVirtualMemory(
    PEPROCESS SourceProcess,
    PVOID SourceAddress,
    PEPROCESS TargetProcess,
    PVOID TargetAddress,
    SIZE_T BufferSize,
    KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize
);

NTKERNELAPI NTSTATUS IoCreateDriver(
    PUNICODE_STRING DriverName,
    PDRIVER_INITIALIZE InitializationFunction
);

NTKERNELAPI NTSTATUS ObOpenObjectByPointer(
    PVOID Object,
    ULONG HandleAttributes,
    PVOID PassedAccessState,
    ACCESS_MASK DesiredAccess,
    POBJECT_TYPE ObjectType,
    KPROCESSOR_MODE AccessMode,
    PHANDLE Handle
);

NTKERNELAPI NTSTATUS ZwQueryVirtualMemory(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    ULONG MemoryInformationClass,
    PVOID MemoryInformation,
    SIZE_T MemoryInformationLength,
    PSIZE_T ReturnLength
);

#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION 0x0400
#endif

typedef struct _RR_MBI {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    ULONG  AllocationProtect;
    USHORT PartitionId;
    USHORT Reserved0;
    SIZE_T RegionSize;
    ULONG  State;
    ULONG  Protect;
    ULONG  Type;
    ULONG  Reserved1;
} RR_MBI;

#define RR_KEY_SALT 0x8F2A9C4BUL

static ULONG g_token = 0;

/* Session key. The client reads the same value from the registry, so a
 * request without the current boot's key is rejected. */
static ULONG XorShift(ULONG key) {
    key ^= key << 13;
    key ^= key >> 17;
    key ^= key << 5;
    return key;
}

static NTSTATUS ReadBootId(PULONG bootId) {
    UNICODE_STRING keyPath, valueName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    ULONG result = 0;
    NTSTATUS status;

    *bootId = 0;
    RtlInitUnicodeString(&keyPath,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager"
        L"\\Memory Management\\PrefetchParameters");
    RtlInitUnicodeString(&valueName, L"BootId");
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&keyHandle, KEY_READ, &attributes);
    if (!NT_SUCCESS(status)) return status;

    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = { 0 };
    ULONG length = 0;
    status = ZwQueryValueKey(keyHandle, &valueName, KeyValuePartialInformation,
                             buffer, sizeof(buffer), &length);
    if (NT_SUCCESS(status)) {
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
            result = *(ULONG*)info->Data;
        else
            status = STATUS_OBJECT_TYPE_MISMATCH;
    }

    ZwClose(keyHandle);
    *bootId = result;
    return status;
}

static ULONG ComputeRingToken(void) {
    ULONG bootId = 0;
    if (!NT_SUCCESS(ReadBootId(&bootId)) || bootId == 0) {
        LARGE_INTEGER systemTime;
        KeQuerySystemTime(&systemTime);
        bootId = systemTime.LowPart ^ systemTime.HighPart;
    }

    ULONG key = XorShift(bootId ^ RR_KEY_SALT);
    return (key == 0) ? 0xA5A5A5A5UL : key;
}

static void PublishRingToken(ULONG token) {
    UNICODE_STRING keyPath, valueName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;

    RtlInitUnicodeString(&keyPath, RR_REGISTRY_PATH);
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&keyHandle, KEY_SET_VALUE, &attributes, 0,
                                NULL, REG_OPTION_VOLATILE, NULL)))
        return;

    RtlInitUnicodeString(&valueName, RR_REGISTRY_TOKEN);
    ZwSetValueKey(keyHandle, &valueName, 0, REG_DWORD, &token, sizeof(token));
    ZwClose(keyHandle);
}

/* Memory operations. */
static NTSTATUS CopyCrossProcess(PEPROCESS sourceProcess, ULONG_PTR sourceAddress,
                                 PEPROCESS targetProcess, ULONG_PTR targetAddress,
                                 SIZE_T size, PSIZE_T transferred) {
    return MmCopyVirtualMemory(sourceProcess, (PVOID)sourceAddress,
                               targetProcess, (PVOID)targetAddress,
                               size, KernelMode, transferred);
}

static NTSTATUS ReadTargetMemory(ULONG processId, ULONG_PTR address,
                                 PVOID kernelBuffer, SIZE_T size) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status)) return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, address, PsGetCurrentProcess(),
                              (ULONG_PTR)kernelBuffer, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size) return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS WriteTargetMemory(ULONG processId, ULONG_PTR address,
                                  PVOID kernelBuffer, SIZE_T size) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status)) return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(PsGetCurrentProcess(), (ULONG_PTR)kernelBuffer,
                              process, address, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size) return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS GetTargetImageBase(ULONG processId, PULONG_PTR imageBase) {
    PEPROCESS process = NULL;
    NTSTATUS status;
    *imageBase = 0;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status)) return status;

    PPEB peb = PsGetProcessPeb(process);
    if (!peb) {
        ObDereferenceObject(process);
        return STATUS_UNSUCCESSFUL;
    }

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, (ULONG_PTR)peb + 0x10,
                              PsGetCurrentProcess(), (ULONG_PTR)imageBase,
                              sizeof(ULONG_PTR), &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != sizeof(ULONG_PTR))
        return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS GetTargetRegion(ULONG processId, ULONG_PTR address,
                                PRR_REGION out) {
    PEPROCESS process = NULL;
    HANDLE handle = NULL;
    NTSTATUS status;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status)) return status;

    status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, NULL,
                                   PROCESS_QUERY_INFORMATION, *PsProcessType,
                                   KernelMode, &handle);
    if (NT_SUCCESS(status)) {
        RR_MBI mbi;
        SIZE_T returned = 0;
        RtlZeroMemory(&mbi, sizeof(mbi));
        status = ZwQueryVirtualMemory(handle, (PVOID)address, 0,
                                      &mbi, sizeof(mbi), &returned);
        if (NT_SUCCESS(status)) {
            out->base = (ULONG_PTR)mbi.BaseAddress;
            out->size = mbi.RegionSize;
            out->state = mbi.State;
            out->protect = mbi.Protect;
            out->type = mbi.Type;
            out->pad = 0;
        }
        ZwClose(handle);
    }

    ObDereferenceObject(process);
    return status;
}

/* Dispatch. */
static NTSTATUS RobRunCreateClose(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS RobRunDetach(void) {
    UNICODE_STRING symlink;
    RtlInitUnicodeString(&symlink, RR_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlink);
    return STATUS_SUCCESS;
}

static NTSTATUS RobRunDispatch(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONG information = 0;

    __try {
        if (inputLength >= sizeof(RR_CMD) && systemBuffer) {
            /* Snapshot before any transfer: METHOD_BUFFERED reuses this
             * storage for the reply. */
            RR_CMD command = *(PRR_CMD)systemBuffer;

            if (command.magic_key != g_token) {
                status = STATUS_ACCESS_DENIED;
            } else if (controlCode == RR_IO_READ) {
                if (command.size == 0 || command.size > RR_MAX_TRANSFER ||
                    command.size > outputLength) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = ReadTargetMemory(command.process_id, command.address,
                                              systemBuffer, (SIZE_T)command.size);
                    if (NT_SUCCESS(status)) information = (ULONG)command.size;
                }
            } else if (controlCode == RR_IO_WRITE) {
                if (command.size == 0 || command.size > RR_MAX_TRANSFER ||
                    command.size > inputLength - sizeof(RR_CMD)) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = WriteTargetMemory(command.process_id, command.address,
                                               (PUCHAR)systemBuffer + sizeof(RR_CMD),
                                               (SIZE_T)command.size);
                }
            } else if (controlCode == RR_IO_BASE) {
                ULONG_PTR base = 0;
                if (outputLength < sizeof(ULONG_PTR)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = GetTargetImageBase(command.process_id, &base);
                    if (NT_SUCCESS(status)) {
                        *(ULONG_PTR*)systemBuffer = base;
                        information = sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == RR_IO_PEB) {
                PEPROCESS process = NULL;
                status = PsLookupProcessByProcessId(
                    (HANDLE)(ULONG_PTR)command.process_id, &process);
                if (NT_SUCCESS(status)) {
                    PPEB peb = PsGetProcessPeb(process);
                    ObDereferenceObject(process);
                    if (!peb) {
                        status = STATUS_UNSUCCESSFUL;
                    } else if (outputLength < sizeof(ULONG_PTR)) {
                        status = STATUS_BUFFER_TOO_SMALL;
                    } else {
                        *(ULONG_PTR*)systemBuffer = (ULONG_PTR)peb;
                        information = sizeof(ULONG_PTR);
                    }
                }
            } else if (controlCode == RR_IO_REGION) {
                if (outputLength < sizeof(RR_REGION)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = GetTargetRegion(command.process_id, command.address,
                                             (PRR_REGION)systemBuffer);
                    if (NT_SUCCESS(status)) information = sizeof(RR_REGION);
                }
            } else if (controlCode == RR_IO_BATCH) {
                RR_BATCH request;
                if (inputLength >= sizeof(RR_BATCH))
                    RtlCopyMemory(&request, systemBuffer, sizeof(RR_BATCH));
                else
                    RtlZeroMemory(&request, sizeof(RR_BATCH));

                if (request.count == 0 || request.count > RR_BATCH_MAX ||
                    inputLength < sizeof(RR_BATCH) ||
                    request.count > outputLength ||
                    request.total > outputLength - request.count ||
                    request.total > RR_MAX_TRANSFER ||
                    request.count > RR_MAX_TRANSFER - request.total) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    PEPROCESS process = NULL;
                    status = PsLookupProcessByProcessId(
                        (HANDLE)(ULONG_PTR)request.process_id, &process);
                    if (NT_SUCCESS(status)) {
                        PUCHAR base = (PUCHAR)systemBuffer;
                        PUCHAR flags = base + request.total;

                        /* Clear the whole reply so gaps and failed copies do
                         * not disclose previous buffer contents. */
                        RtlZeroMemory(base, (SIZE_T)request.total + (SIZE_T)request.count);

                        for (ULONG i = 0; i < request.count; i++) {
                            PRR_BATCH_ITEM item = &request.items[i];
                            flags[i] = 0;
                            if (item->size == 0 ||
                                item->offset > request.total ||
                                item->size > request.total - item->offset)
                                continue;

                            SIZE_T transferred = 0;
                            NTSTATUS copyStatus = CopyCrossProcess(
                                process, item->address, PsGetCurrentProcess(),
                                (ULONG_PTR)(base + item->offset),
                                (SIZE_T)item->size, &transferred);
                            if (NT_SUCCESS(copyStatus) && transferred == item->size)
                                flags[i] = 1;
                            else
                                RtlZeroMemory(base + item->offset, item->size);
                        }
                        ObDereferenceObject(process);
                        information = request.total + request.count;
                    }
                }
            } else if (controlCode == RR_IO_DETACH) {
                status = RobRunDetach();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        information = 0;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

/* Driver init. */
static NTSTATUS RobRunInit(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);

    UNICODE_STRING deviceName, symlink;
    PDEVICE_OBJECT device = NULL;
    WCHAR nameBuffer[64];
    NTSTATUS status = STATUS_OBJECT_NAME_COLLISION;

    static const WCHAR baseName[] = RR_DEVICE_NAME;
    USHORT baseLength = (USHORT)(sizeof(baseName) / sizeof(WCHAR) - 1);

    /* A previous mapping in this boot may still own the base name. */
    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        USHORT position = 0;
        for (USHORT i = 0; i < baseLength && position + 1 < 64; i++)
            nameBuffer[position++] = baseName[i];

        WCHAR digits[12];
        int digitCount = 0;
        ULONG value = attempt;
        do {
            digits[digitCount++] = (WCHAR)(L'0' + (value % 10));
            value /= 10;
        } while (value);
        while (digitCount > 0 && position + 1 < 64)
            nameBuffer[position++] = digits[--digitCount];
        nameBuffer[position] = L'\0';

        RtlInitUnicodeString(&deviceName, nameBuffer);
        status = IoCreateDevice(driverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
                                FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    }
    if (!NT_SUCCESS(status)) return status;

    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;

    driverObject->MajorFunction[IRP_MJ_CREATE] = RobRunCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = RobRunCreateClose;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RobRunDispatch;

    RtlInitUnicodeString(&symlink, RR_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlink);
    status = IoCreateSymbolicLink(&symlink, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }

    return STATUS_SUCCESS;
}

/* Entry. The mapper calls DriverEntry from a user-mode thread; defer object
 * and registry work to a system thread with a normal kernel context. */
static void RobRunBootstrap(PVOID context) {
    UNREFERENCED_PARAMETER(context);

    g_token = ComputeRingToken();
    PublishRingToken(g_token);

    static const WCHAR baseName[] = RR_DRIVER_NAME;
    USHORT baseLength = (USHORT)(sizeof(baseName) / sizeof(WCHAR) - 1);
    NTSTATUS status = STATUS_OBJECT_NAME_COLLISION;

    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        WCHAR nameBuffer[64];
        USHORT position = 0;
        for (USHORT i = 0; i < baseLength && position + 1 < 64; i++)
            nameBuffer[position++] = baseName[i];

        WCHAR digits[12];
        int digitCount = 0;
        ULONG value = attempt;
        do {
            digits[digitCount++] = (WCHAR)(L'0' + (value % 10));
            value /= 10;
        } while (value);
        while (digitCount > 0 && position + 1 < 64)
            nameBuffer[position++] = digits[--digitCount];
        nameBuffer[position] = L'\0';

        UNICODE_STRING driverName;
        RtlInitUnicodeString(&driverName, nameBuffer);
        status = IoCreateDriver(&driverName, RobRunInit);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PVOID parameterOne, PVOID parameterTwo) {
    UNREFERENCED_PARAMETER(parameterOne);
    UNREFERENCED_PARAMETER(parameterTwo);

    HANDLE thread = NULL;
    NTSTATUS status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS,
                                           NULL, NULL, NULL, RobRunBootstrap, NULL);
    if (!NT_SUCCESS(status)) return status;

    ZwClose(thread);
    return STATUS_SUCCESS;
}
