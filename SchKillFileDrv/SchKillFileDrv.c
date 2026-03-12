#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include "../common.h"

//
// Definitons for System Handle Information
//
#define SystemExtendedHandleInformation 64

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, * PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX, * PSYSTEM_HANDLE_INFORMATION_EX;

// Context for the system worker thread
typedef struct _WORK_ITEM_CTX {
    PIO_WORKITEM   WorkItem;
    PIRP           Irp;
    ULONG          IoControlCode;
    ULONG          PathLength; // Length in bytes
    WCHAR          PathBuffer[1]; // Variable length
} WORK_ITEM_CTX, * PWORK_ITEM_CTX;

// Context for iterative directory traversal
typedef struct _DIR_NODE {
    LIST_ENTRY     ListEntry;
    UNICODE_STRING Path;
} DIR_NODE, * PDIR_NODE;

NTSTATUS ZwQuerySystemInformation(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

NTSTATUS ZwQueryInformationProcess(
    _In_      HANDLE           ProcessHandle,
    _In_      PROCESSINFOCLASS ProcessInformationClass,
    _Out_     PVOID            ProcessInformation,
    _In_      ULONG            ProcessInformationLength,
    _Out_opt_ PULONG           ReturnLength
);

// ---------------------------------------------------------------------------
// Check if a process is critical (BSOD if killed) or System/Idle
// ---------------------------------------------------------------------------
BOOLEAN SchIsCriticalProcess(PEPROCESS Process)
{
    NTSTATUS status;
    HANDLE hProcess = NULL;
    ULONG breakOnTermination = 0;

    // Check "BreakOnTermination" flag.

    status = ZwQueryInformationProcess(
        ZwCurrentProcess(),
        ProcessBreakOnTermination,
        &breakOnTermination,
        sizeof(ULONG),
        NULL
    );

    if (NT_SUCCESS(status) && breakOnTermination != 0) {
        return TRUE; // Would rather not delete the files than kill the critical process (would trigger BSoD).
    }

    return FALSE;
}

VOID SchUnloadDriver(
    IN PDRIVER_OBJECT DriverObject)
{
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    UNICODE_STRING uniSymLink;

    RtlInitUnicodeString(&uniSymLink, DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&uniSymLink);
    IoDeleteDevice(deviceObject);
}

HANDLE
SchOpenFile(
    IN PCWSTR FileName,
    IN ACCESS_MASK DesiredAccess,
    IN ULONG ShareAccess)
{
    NTSTATUS ntStatus;
    UNICODE_STRING uniFileName;
    OBJECT_ATTRIBUTES objectAttributes;
    HANDLE ntFileHandle;
    IO_STATUS_BLOCK ioStatus;

    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        DbgPrint("[-] Should not be at PASSIVE_LEVEL!\n");
        return 0;
    }

    RtlInitUnicodeString(&uniFileName, FileName);

    InitializeObjectAttributes(&objectAttributes, &uniFileName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    ntStatus = IoCreateFile(&ntFileHandle,
        DesiredAccess,
        &objectAttributes,
        &ioStatus,
        0,
        FILE_ATTRIBUTE_NORMAL,
        ShareAccess,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0,
        0,
        NULL,
        IO_NO_PARAMETER_CHECKING);

    if (!NT_SUCCESS(ntStatus))
    {
        DbgPrint("[-] IoCreateFile Error\n");
        return 0;
    }

    return ntFileHandle;
}

NTSTATUS
SchSetInformationCompletion(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    Irp->UserIosb->Status = Irp->IoStatus.Status;
    Irp->UserIosb->Information = Irp->IoStatus.Information;

    KeSetEvent(Irp->UserEvent, IO_NO_INCREMENT, FALSE);

    IoFreeIrp(Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// ---------------------------------------------------------------------------
// HELPER: Manually duplicate a UNICODE_STRING to avoid RTL API dependency issues
// ---------------------------------------------------------------------------
NTSTATUS SchDuplicateString(
    IN PCUNICODE_STRING Source,
    OUT PUNICODE_STRING Destination
)
{
    if (!Source || !Destination) return STATUS_INVALID_PARAMETER;

    Destination->MaximumLength = Source->Length + sizeof(WCHAR);
    Destination->Buffer = ExAllocatePoolWithTag(PagedPool, Destination->MaximumLength, 'DupS');

    if (!Destination->Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(Destination, Source);

    // Ensure NULL termination manually
    if (Destination->Length < Destination->MaximumLength) {
        Destination->Buffer[Destination->Length / sizeof(WCHAR)] = L'\0';
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// HELPER: Convert DOS Path (\??\C:\Folder\File) to Device Path (\Device\HarddiskVolumeX\Folder\File)
// ---------------------------------------------------------------------------
NTSTATUS SchConvertDosPathToDevicePath(IN PCWSTR DosPath, OUT PUNICODE_STRING DevicePath)
{
    NTSTATUS status;
    UNICODE_STRING uniDosPath;
    UNICODE_STRING uniSymLink;
    UNICODE_STRING uniDevicePrefix;
    OBJECT_ATTRIBUTES objAttr;
    HANDLE hSymLink;
    ULONG resLen;
    USHORT prefixLen = 0;
    USHORT i = 0;
    BOOLEAN foundColon = FALSE;

    RtlInitUnicodeString(&uniDosPath, DosPath);

    // 1. Find the drive letter colon manually to avoid CRT wcschr dependencies
    // Path format expected: \??\C:\...
    // We scan the first 10 chars which is enough for drive letters
    for (i = 0; i < (uniDosPath.Length / sizeof(WCHAR)) && i < 10; i++) {
        if (uniDosPath.Buffer[i] == L':') {
            prefixLen = (i + 1) * sizeof(WCHAR); // Include the colon
            foundColon = TRUE;
            break;
        }
    }

    // If no colon found, or it's not a \??\ path, just duplicate input and hope for best
    if (!foundColon) {
        return SchDuplicateString(&uniDosPath, DevicePath);
    }

    // 2. Open the Symbolic Link (e.g. \??\C:)
    uniSymLink.Buffer = uniDosPath.Buffer;
    uniSymLink.Length = prefixLen;
    uniSymLink.MaximumLength = prefixLen;

    InitializeObjectAttributes(&objAttr, &uniSymLink, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwOpenSymbolicLinkObject(&hSymLink, GENERIC_READ, &objAttr);
    if (!NT_SUCCESS(status)) {
        // Fallback: If we can't open symlink (e.g. invalid name), use original
        return SchDuplicateString(&uniDosPath, DevicePath);
    }

    // 3. Query the target (e.g. \Device\HarddiskVolume1)
    // Allocate a temporary buffer for the device name
    uniDevicePrefix.MaximumLength = 512;
    uniDevicePrefix.Length = 0;
    uniDevicePrefix.Buffer = ExAllocatePoolWithTag(PagedPool, 512, 'PthC');

    if (!uniDevicePrefix.Buffer) {
        ZwClose(hSymLink);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQuerySymbolicLinkObject(hSymLink, &uniDevicePrefix, &resLen);
    ZwClose(hSymLink);

    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(uniDevicePrefix.Buffer, 'PthC');
        return SchDuplicateString(&uniDosPath, DevicePath);
    }

    // 4. Construct Final Path: \Device\HarddiskVolume1 + \Folder\File
    // Calculate size: DevicePrefix Length + (Original Length - Prefix Length) + Null
    USHORT remainingLen = uniDosPath.Length - prefixLen;
    DevicePath->MaximumLength = uniDevicePrefix.Length + remainingLen + sizeof(WCHAR);

    DevicePath->Buffer = ExAllocatePoolWithTag(PagedPool, DevicePath->MaximumLength, 'PthF');
    DevicePath->Length = 0;

    if (!DevicePath->Buffer) {
        ExFreePoolWithTag(uniDevicePrefix.Buffer, 'PthC');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Copy Device Prefix
    RtlCopyUnicodeString(DevicePath, &uniDevicePrefix);

    // Append the rest of the path (the part after \??\C:)
    // Manual append to be safe
    if (remainingLen > 0) {
        RtlAppendUnicodeToString(DevicePath, DosPath + (prefixLen / sizeof(WCHAR)));
    }

    ExFreePoolWithTag(uniDevicePrefix.Buffer, 'PthC');
    return STATUS_SUCCESS;
}

//
// Traverse ALL system handles (including kernel ones) and close those matching the target path
//
NTSTATUS SchForceCloseHandles(IN PCWSTR TargetPath)
{
    NTSTATUS status;
    PSYSTEM_HANDLE_INFORMATION_EX pHandleInfo = NULL;
    ULONG ulSize = 0x10000; // Start with a larger buffer for EX info
    ULONG ulRet = 0;
    UNICODE_STRING targetDevicePath = { 0 };

    // Convert input path to DOS Device Path for correct comparison
    status = SchConvertDosPathToDevicePath(TargetPath, &targetDevicePath);
    if (!NT_SUCCESS(status)) return status;

    // Loop to get extended handle snapshot
    while (TRUE) {
        pHandleInfo = (PSYSTEM_HANDLE_INFORMATION_EX)ExAllocatePoolWithTag(PagedPool, ulSize, 'Hndl');
        if (!pHandleInfo) {
            RtlFreeUnicodeString(&targetDevicePath);
            return STATUS_MEMORY_NOT_ALLOCATED;
        }

        status = ZwQuerySystemInformation(SystemExtendedHandleInformation, pHandleInfo, ulSize, &ulRet);

        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            ExFreePoolWithTag(pHandleInfo, 'Hndl');
            ulSize = ulRet + 0x5000; // Add an extra buffer margin
        }
        else {
            break;
        }
    }

    if (!NT_SUCCESS(status) || !pHandleInfo) {
        if (pHandleInfo) ExFreePoolWithTag(pHandleInfo, 'Hndl');
        RtlFreeUnicodeString(&targetDevicePath);
        return status;
    }

    // Iterate handles using the EX structure
    for (ULONG_PTR i = 0; i < pHandleInfo->NumberOfHandles; i++) {
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX handleEntry = pHandleInfo->Handles[i];

        PEPROCESS pProcess;
        status = PsLookupProcessByProcessId((HANDLE)handleEntry.UniqueProcessId, &pProcess);
        if (!NT_SUCCESS(status)) continue;

        KAPC_STATE apcState;
        KeStackAttachProcess(pProcess, &apcState);

        // Now we are in the context of the process holding the handle.
        PVOID object = NULL;
        status = ObReferenceObjectByHandle((HANDLE)handleEntry.HandleValue,
            0,
            *IoFileObjectType,
            KernelMode, // Must use KernelMode to resolve kernel handles safely
            &object,
            NULL);

        if (NT_SUCCESS(status)) {
            ULONG returnLength;
            POBJECT_NAME_INFORMATION pNameInfo = ExAllocatePoolWithTag(NonPagedPool, 1024, 'NmeB');

            if (pNameInfo) {
                status = ObQueryNameString(object, pNameInfo, 1024, &returnLength);

                if (NT_SUCCESS(status) && pNameInfo->Name.Buffer) {
                    if (RtlEqualUnicodeString(&pNameInfo->Name, &targetDevicePath, TRUE)) {
                        
                        // Close the handle forcefully
                        status = ZwClose((HANDLE)handleEntry.HandleValue);
                        if (!NT_SUCCESS(status)) {
                            DbgPrint("[-] Status = 0x%X: Failed to close handle: PID %Id, Val 0x%IX, Name %wZ\n", 
                                status, handleEntry.UniqueProcessId, handleEntry.HandleValue, &pNameInfo->Name);
                        } else {
                            DbgPrint("[+] Force closed handle: PID %Id, Val 0x%IX, Name %wZ\n", 
                                handleEntry.UniqueProcessId, handleEntry.HandleValue, &pNameInfo->Name);
                        }

                        // DO NOT terminate System Process (PID 4) under ANY circumstances.
                        if (handleEntry.UniqueProcessId != 4) {
                            PEPROCESS pProc = PsGetCurrentProcess();
                            if (!SchIsCriticalProcess(pProc)) {
                                status = ZwTerminateProcess(ZwCurrentProcess(), STATUS_FILE_FORCED_CLOSED);
                                if (!NT_SUCCESS(status)) {
                                    DbgPrint("[-] Failed to Terminate Process %Id. Status: 0x%X\n", handleEntry.UniqueProcessId, status);
                                }
                            }
                            else {
                                DbgPrint("[*] Process %Id is critical, skipping termination.\n", handleEntry.UniqueProcessId);
                            }
                        }
                    }
                }
                ExFreePoolWithTag(pNameInfo, 'NmeB');
            }
            ObDereferenceObject(object);
        }

        KeUnstackDetachProcess(&apcState);
        ObDereferenceObject(pProcess);
    }

    ExFreePoolWithTag(pHandleInfo, 'Hndl');
    RtlFreeUnicodeString(&targetDevicePath);
    return STATUS_SUCCESS;
}

//
// Robust Single File Deletion
//
NTSTATUS
SchKillSingleFile(
    IN PCWSTR FilePath // Accept Path directly to allow retry logic 
)
{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    HANDLE FileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    PDEVICE_OBJECT DeviceObject;
    PIRP Irp;
    KEVENT event;
    FILE_DISPOSITION_INFORMATION FileInformation;
    FILE_BASIC_INFORMATION BasicInformation;
    IO_STATUS_BLOCK ioStatus;
    PIO_STACK_LOCATION irpSp;
    OBJECT_ATTRIBUTES objAttr;
    UNICODE_STRING uniName;

    RtlInitUnicodeString(&uniName, FilePath);
    InitializeObjectAttributes(&objAttr, &uniName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    // Attempt to open for the first time with FILE_WRITE_ATTRIBUTES to allow ReadOnly removal
    ntStatus = IoCreateFile(&FileHandle, DELETE | FILE_WRITE_ATTRIBUTES, &objAttr, &ioStatus, 0, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0, CreateFileTypeNone, NULL, IO_NO_PARAMETER_CHECKING);

    // If opening fails, it indicates there might be an exclusive lock; unlock it first.
    if (!NT_SUCCESS(ntStatus)) {
        DbgPrint("[-] First Open Failed (0x%X). Creating Force Unlock for %S...\n", ntStatus, FilePath);
        ntStatus = SchForceCloseHandles(FilePath);
        if (!NT_SUCCESS(ntStatus)) return ntStatus;

        // Give the process some time to exit and release resources (100ms)
        LARGE_INTEGER interval;
        interval.QuadPart = -10000 * 100;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);

        // Reattempt to open
        ntStatus = IoCreateFile(&FileHandle, DELETE | FILE_WRITE_ATTRIBUTES, &objAttr, &ioStatus, 0, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0, CreateFileTypeNone, NULL, IO_NO_PARAMETER_CHECKING);

        if (!NT_SUCCESS(ntStatus)) {
            DbgPrint("[-] Failed to open the same file again (0x%X).\n", ntStatus);
            return ntStatus;
        }
    }

    // Get File Object
    ntStatus = ObReferenceObjectByHandle(FileHandle, DELETE | FILE_WRITE_ATTRIBUTES, *IoFileObjectType, KernelMode, (PVOID*)&fileObject, NULL);
    if (!NT_SUCCESS(ntStatus)) {
        ZwClose(FileHandle);
        return ntStatus;
    }

    // Check if Section/Cache clearing was successful.
    if (fileObject->SectionObjectPointer) {
        BOOLEAN purged = CcPurgeCacheSection(fileObject->SectionObjectPointer, NULL, 0, FALSE);
        // If MmFlushForDelete fails, another process may still be mapping this file.
        BOOLEAN flushed = MmFlushImageSection(fileObject->SectionObjectPointer, MmFlushForDelete);

        if (!purged || !flushed) {
            DbgPrint("[-] Flush Failed (Mapped?) for %S. Attempting to Kill Owners and Retry...\n", FilePath);

            // Release the current object and prepare to kill the process.
            ObDereferenceObject(fileObject);
            ZwClose(FileHandle);

            // Perform force unlocking
            SchForceCloseHandles(FilePath);

            // Waiting for kernel cleanup of Section Object (200ms)
            LARGE_INTEGER interval;
            interval.QuadPart = -10000 * 200;
            KeDelayExecutionThread(KernelMode, FALSE, &interval);

            // Reattempt to open
            ntStatus = IoCreateFile(&FileHandle, DELETE | FILE_WRITE_ATTRIBUTES, &objAttr, &ioStatus, 0, FILE_ATTRIBUTE_NORMAL,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0, CreateFileTypeNone, NULL, IO_NO_PARAMETER_CHECKING);

            if (!NT_SUCCESS(ntStatus)) return ntStatus;

            ntStatus = ObReferenceObjectByHandle(FileHandle, DELETE | FILE_WRITE_ATTRIBUTES, *IoFileObjectType, KernelMode, (PVOID*)&fileObject, NULL);
            if (!NT_SUCCESS(ntStatus)) {
                ZwClose(FileHandle);
                return ntStatus;
            }

            // Reattempt to flush
            if (fileObject->SectionObjectPointer) {
                purged = CcPurgeCacheSection(fileObject->SectionObjectPointer, NULL, 0, FALSE);
                flushed = MmFlushImageSection(fileObject->SectionObjectPointer, MmFlushForDelete);
                if (!purged || !flushed)
                    DbgPrint("[-] %S failed for file: %S", ((!purged && !flushed) ? (L"CcPurgeCacheSection() && MmFlushImageSection()")
                        : (!purged ? L"CcPurgeCacheSection()" : L"MmFlushImageSection()")), FilePath);
                else
                    DbgPrint("[+] Succeeded for file: %S", FilePath);
            }
        }
    }

    /*
    // If you'd like to unsafely delete stubbornly occupied files anyway, you can use the following snippet.
    // But remember, it is highly prone to trigger BSoD since you corrupted the refcount and introduced null pointers.
    if (fileObject->SectionObjectPointer != NULL) {
        fileObject->SectionObjectPointer->ImageSectionObject = NULL;
        fileObject->SectionObjectPointer->DataSectionObject = NULL;
    }
    */

    // Construct IRP
    DeviceObject = IoGetRelatedDeviceObject(fileObject);

    // Strip FILE_ATTRIBUTE_READONLY
    RtlZeroMemory(&BasicInformation, sizeof(FILE_BASIC_INFORMATION));
    BasicInformation.FileAttributes = FILE_ATTRIBUTE_NORMAL;

    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (Irp != NULL) {
        KeInitializeEvent(&event, SynchronizationEvent, FALSE);
        Irp->AssociatedIrp.SystemBuffer = &BasicInformation;
        Irp->UserEvent = &event;
        Irp->UserIosb = &ioStatus;
        Irp->Tail.Overlay.OriginalFileObject = fileObject;
        Irp->Tail.Overlay.Thread = (PETHREAD)KeGetCurrentThread();
        Irp->RequestorMode = KernelMode;

        irpSp = IoGetNextIrpStackLocation(Irp);
        irpSp->MajorFunction = IRP_MJ_SET_INFORMATION;
        irpSp->DeviceObject = DeviceObject;
        irpSp->FileObject = fileObject;
        irpSp->Parameters.SetFile.Length = sizeof(FILE_BASIC_INFORMATION);
        irpSp->Parameters.SetFile.FileInformationClass = FileBasicInformation;
        irpSp->Parameters.SetFile.FileObject = fileObject;

        IoSetCompletionRoutine(Irp, SchSetInformationCompletion, &event, TRUE, TRUE, TRUE);
        IoCallDriver(DeviceObject, Irp);
        KeWaitForSingleObject(&event, Executive, KernelMode, TRUE, NULL);
    }

    // Set Delete Disposition
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (Irp == NULL) {
        ObDereferenceObject(fileObject);
        ZwClose(FileHandle);
        return ntStatus;
    }

    KeInitializeEvent(&event, SynchronizationEvent, FALSE);
    FileInformation.DeleteFile = TRUE;

    Irp->AssociatedIrp.SystemBuffer = &FileInformation;
    Irp->UserEvent = &event;
    Irp->UserIosb = &ioStatus;
    Irp->Tail.Overlay.OriginalFileObject = fileObject;
    Irp->Tail.Overlay.Thread = (PETHREAD)KeGetCurrentThread();
    Irp->RequestorMode = KernelMode;

    irpSp = IoGetNextIrpStackLocation(Irp);
    irpSp->MajorFunction = IRP_MJ_SET_INFORMATION;
    irpSp->DeviceObject = DeviceObject;
    irpSp->FileObject = fileObject;
    irpSp->Parameters.SetFile.Length = sizeof(FILE_DISPOSITION_INFORMATION);
    irpSp->Parameters.SetFile.FileInformationClass = FileDispositionInformation;
    irpSp->Parameters.SetFile.FileObject = fileObject;

    IoSetCompletionRoutine(Irp, SchSetInformationCompletion, &event, TRUE, TRUE, TRUE);

    IoCallDriver(DeviceObject, Irp);
    KeWaitForSingleObject(&event, Executive, KernelMode, TRUE, NULL);

    ObDereferenceObject(fileObject);
    ZwClose(FileHandle);

    return ioStatus.Status;
}

//
// Robust Recursive Directory Deletion
// BFS for discovery -> Add to List -> Reverse List Traversal (DFS effect) -> Force Kill Each
//
NTSTATUS
SchDeleteFolderIterative(
    IN PCWSTR RootPath
)
{
    NTSTATUS status;
    LIST_ENTRY dirListHead;
    PDIR_NODE rootNode = NULL;
    NTSTATUS finalStatus = STATUS_SUCCESS;

    InitializeListHead(&dirListHead);

    // --- Phase 1: Discovery (BFS) ---
    // Initialize root node
    rootNode = (PDIR_NODE)ExAllocatePoolWithTag(PagedPool, sizeof(DIR_NODE), 'doND');
    if (!rootNode) return STATUS_DEVICE_INSUFFICIENT_RESOURCES;

    status = RtlCreateUnicodeString(&rootNode->Path, RootPath);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(rootNode, 'doND');
        return status;
    }
    InsertTailList(&dirListHead, &rootNode->ListEntry);

    PLIST_ENTRY pEntry = dirListHead.Flink;

    while (pEntry != &dirListHead) {
        PDIR_NODE currentNode = CONTAINING_RECORD(pEntry, DIR_NODE, ListEntry);

        HANDLE hDir = NULL;
        OBJECT_ATTRIBUTES objAttr;
        IO_STATUS_BLOCK ioSt;
        PFILE_FULL_DIR_INFORMATION pInfo = NULL;
        ULONG bufLen = 4096;

        InitializeObjectAttributes(&objAttr, &currentNode->Path,
            OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

        status = ZwOpenFile(&hDir, FILE_LIST_DIRECTORY | SYNCHRONIZE, &objAttr, &ioSt,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);

        if (NT_SUCCESS(status)) {
            pInfo = ExAllocatePoolWithTag(NonPagedPool, bufLen, 'DfMz');
            if (pInfo) {
                BOOLEAN restartScan = TRUE;
                while (TRUE) {
                    status = ZwQueryDirectoryFile(hDir, NULL, NULL, NULL, &ioSt,
                        pInfo, bufLen, FileFullDirectoryInformation,
                        FALSE, NULL, restartScan);
                    restartScan = FALSE;

                    if (!NT_SUCCESS(status) || ioSt.Information == 0) break;

                    PFILE_FULL_DIR_INFORMATION pCur = pInfo;
                    do {
                        BOOLEAN isDot = (pCur->FileNameLength == 2 && pCur->FileName[0] == L'.') ||
                            (pCur->FileNameLength == 4 && pCur->FileName[0] == L'.' && pCur->FileName[1] == L'.');

                        if (!isDot) {
                            UNICODE_STRING subPath;
                            USHORT nameLen = (USHORT)pCur->FileNameLength;
                            USHORT newLen = currentNode->Path.Length + sizeof(WCHAR) + nameLen;

                            subPath.MaximumLength = newLen + sizeof(WCHAR);
                            subPath.Buffer = ExAllocatePoolWithTag(PagedPool, subPath.MaximumLength, 'htPS');

                            if (subPath.Buffer) {
                                subPath.Length = 0;
                                RtlCopyUnicodeString(&subPath, &currentNode->Path);
                                if (subPath.Length > 0 && subPath.Buffer[(subPath.Length / sizeof(WCHAR)) - 1] != L'\\') {
                                    RtlAppendUnicodeToString(&subPath, L"\\");
                                }
                                RtlCopyMemory((PUCHAR)subPath.Buffer + subPath.Length, pCur->FileName, nameLen);
                                subPath.Length += nameLen;
                                subPath.Buffer[subPath.Length / sizeof(WCHAR)] = L'\0';

                                PDIR_NODE newNode = ExAllocatePoolWithTag(PagedPool, sizeof(DIR_NODE), 'doND');
                                if (newNode) {
                                    newNode->Path = subPath; // Transfer ownership

                                    // Important: Add to tail. Later we traverse from Tail to Head.
                                    InsertTailList(&dirListHead, &newNode->ListEntry);
                                }
                                else {
                                    RtlFreeUnicodeString(&subPath);
                                }
                            }
                        }
                        if (pCur->NextEntryOffset == 0) break;
                        pCur = (PFILE_FULL_DIR_INFORMATION)((PUCHAR)pCur + pCur->NextEntryOffset);
                    } while (TRUE);
                }
                ExFreePoolWithTag(pInfo, 'DfMz');
            }
            ZwClose(hDir);
        }
        pEntry = pEntry->Flink;
    }

    // --- Phase 2: Converging Deletion Loop ---
    // Repeatedly scan the list from Tail to Head (Children before Parents).
    // If we delete SOMETHING in a pass, we scan again.
	
    BOOLEAN bProgressMade;
    int passCount = 0;
    int retryStuckCount = 0; 

    do {
        bProgressMade = FALSE;
        passCount++;

        // Start from the tail (deepest items)
        PLIST_ENTRY pCurrEntry = dirListHead.Blink;

        while (pCurrEntry != &dirListHead) {
            PDIR_NODE node = CONTAINING_RECORD(pCurrEntry, DIR_NODE, ListEntry);

            // Save prev now because we might remove the node
            PLIST_ENTRY pPrevEntry = pCurrEntry->Blink;

            // Attempt to kill/delete
            // Note: SchKillSingleFile should handle both files and empty folders (via IoCreateFile with DELETE)
            // But for folders, we might fail if children are still there. That's fine, we'll catch them next pass.
            NTSTATUS killStatus = SchKillSingleFile(node->Path.Buffer);

            if (NT_SUCCESS(killStatus)) {
                // Success: Remove from list and free memory
                RemoveEntryList(&node->ListEntry);
                RtlFreeUnicodeString(&node->Path);
                ExFreePoolWithTag(node, 'doND');

                bProgressMade = TRUE;
            }
            else {
                finalStatus = killStatus; // Record the reason of failure
            }

            pCurrEntry = pPrevEntry;
        }

        if (bProgressMade) {
            retryStuckCount = 0; 
            DbgPrint("[SDKrn] Pass %d made progress. Rescanning remaining items...\n", passCount);

            LARGE_INTEGER interval;
            interval.QuadPart = -10000 * 50; 
            KeDelayExecutionThread(KernelMode, FALSE, &interval);
        }
        // This indicates that the release of the underlying asynchronous object has not yet been completed, 
        // causing the directory to temporarily return STATUS_DIRECTORY_NOT_EMPTY, thus the deletion failed and the list is empty. 
        // In this case, a grace period should be given before retrying.
        else if (!IsListEmpty(&dirListHead)) {
            retryStuckCount++;
            if (retryStuckCount <= 10) {
                DbgPrint("[SDKrn] Pass %d stalled. Awaiting kernel async teardown. Retry %d/10...\n", passCount, retryStuckCount);
                
                LARGE_INTEGER interval;
                interval.QuadPart = -10000 * 200; 
                KeDelayExecutionThread(KernelMode, FALSE, &interval);
                
                bProgressMade = TRUE; 
            }
        }

    } while (bProgressMade && !IsListEmpty(&dirListHead));

    // --- Phase 3: Cleanup Remnants ---
    if (!IsListEmpty(&dirListHead)) {
        while (!IsListEmpty(&dirListHead)) {
            PLIST_ENTRY pTail = RemoveTailList(&dirListHead);
            PDIR_NODE node = CONTAINING_RECORD(pTail, DIR_NODE, ListEntry);

            DbgPrint("[-] Failed to delete residue: %wZ\n", &node->Path);

            RtlFreeUnicodeString(&node->Path);
            ExFreePoolWithTag(node, 'doND');
        }
        // Return STATUS_SUCCESS if list is empty, otherwise return the last failure status
        return finalStatus != STATUS_SUCCESS ? finalStatus : STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

VOID
SchWorkItemRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID Context
)
{
    PWORK_ITEM_CTX ctx = (PWORK_ITEM_CTX)Context;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    DbgPrint("[SDKrn] Worker executing for: %S\n", ctx->PathBuffer);

    if (ctx->IoControlCode == IOCTL_DELETEFILE)
        status = SchKillSingleFile(ctx->PathBuffer);
    else if (ctx->IoControlCode == IOCTL_DELETEFOLDER)
        status = SchDeleteFolderIterative(ctx->PathBuffer);

    // Complete the IRP with the exact status code
    ctx->Irp->IoStatus.Status = status;
    ctx->Irp->IoStatus.Information = 0;
    IoCompleteRequest(ctx->Irp, IO_NO_INCREMENT);

    // Cleanup
    IoFreeWorkItem(ctx->WorkItem);
    ExFreePoolWithTag(ctx, 'kWrW');
}

NTSTATUS
DispatchCreate(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    pIrp->IoStatus.Status = STATUS_SUCCESS;
    pIrp->IoStatus.Information = 0;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

NTSTATUS
DispatchClose(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    pIrp->IoStatus.Status = STATUS_SUCCESS;
    pIrp->IoStatus.Information = 0;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

NTSTATUS
DispatchDeviceControl(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PIO_STACK_LOCATION pIrpStack = IoGetCurrentIrpStackLocation(pIrp);
    ULONG uIoControlCode = pIrpStack->Parameters.DeviceIoControl.IoControlCode;
    PVOID pIoBuffer = pIrp->AssociatedIrp.SystemBuffer;
    ULONG uInSize = pIrpStack->Parameters.DeviceIoControl.InputBufferLength;

    switch (uIoControlCode)
    {
    case IOCTL_DELETEFILE:
    case IOCTL_DELETEFOLDER:
    {
        // Basic validation
        if (uInSize < sizeof(WCHAR) || pIoBuffer == NULL) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        // Dynamic Allocation for Context
        // Prefix "\??\" length is 4 WCHARs (8 bytes). 
        // We allocate enough for Prefix + Input + Null Terminator.
        ULONG prefixLen = 4 * sizeof(WCHAR);
        ULONG totalSize = sizeof(WORK_ITEM_CTX) + prefixLen + uInSize + sizeof(WCHAR);

        PWORK_ITEM_CTX pCtx = ExAllocatePoolWithTag(NonPagedPool, totalSize, 'kWrW');
        if (!pCtx) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        // 3. Prepare Context
        pCtx->IoControlCode = uIoControlCode;
        pCtx->Irp = pIrp;
        pCtx->WorkItem = IoAllocateWorkItem(pDevObj);

        if (!pCtx->WorkItem) {
            ExFreePoolWithTag(pCtx, 'kWrW');
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        // 4. Copy Path safely and Prepend Prefix
        RtlZeroMemory(pCtx->PathBuffer, prefixLen + uInSize + sizeof(WCHAR));
        RtlStringCbCopyW(pCtx->PathBuffer, totalSize, L"\\??\\");

        // Append user buffer (be careful of missing null in user buffer)
        RtlStringCbCatNW(pCtx->PathBuffer, totalSize, (PCWSTR)pIoBuffer, uInSize);

        // 5. Queue Work Item
        IoMarkIrpPending(pIrp);
        IoQueueWorkItem(pCtx->WorkItem, SchWorkItemRoutine, DelayedWorkQueue, pCtx);

        return STATUS_PENDING;
    }
    default:
        // Keep existing default logic...
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    if (status != STATUS_PENDING) {
        pIrp->IoStatus.Status = status;
        pIrp->IoStatus.Information = 0;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    }

    return status;
}

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING uniDeviceName;
    UNICODE_STRING uniSymLink;
    NTSTATUS ntStatus;
    PDEVICE_OBJECT deviceObject = NULL;
    HANDLE hFileHandle;

    RtlInitUnicodeString(&uniDeviceName, NT_DEVICE_NAME);
    RtlInitUnicodeString(&uniSymLink, DOS_DEVICE_NAME);

    ntStatus = IoCreateDevice(
        DriverObject,
        0,
        &uniDeviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject);

    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    ntStatus = IoCreateSymbolicLink(&uniSymLink, &uniDeviceName);

    if (!NT_SUCCESS(ntStatus))
    {
        UNICODE_STRING uniSymLink;

        RtlInitUnicodeString(&uniSymLink, DOS_DEVICE_NAME);
        IoDeleteSymbolicLink(&uniSymLink);

        ntStatus = IoCreateSymbolicLink(&uniSymLink, &uniDeviceName);
        if (!NT_SUCCESS(ntStatus))
        {
            IoDeleteDevice(deviceObject);
            return ntStatus;
        }
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;

    DriverObject->DriverUnload = SchUnloadDriver;

    return STATUS_SUCCESS;
}