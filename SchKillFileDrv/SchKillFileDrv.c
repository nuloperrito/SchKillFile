#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include "../common.h"

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
        KdPrint(("Do Not At PASSIVE_LEVEL!\n"));
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
                            0,
                            NULL,
                            0,
                            0,
                            NULL,
                            IO_NO_PARAMETER_CHECKING);

    if (!NT_SUCCESS(ntStatus))
    {
        KdPrint(("IoCreateFile Error\n"));
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

BOOLEAN
SchKillFileApp(
    IN HANDLE FileHandle)
{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    PFILE_OBJECT fileObject;
    PDEVICE_OBJECT DeviceObject;
    PIRP Irp;
    KEVENT event;
    FILE_DISPOSITION_INFORMATION FileInformation;
    IO_STATUS_BLOCK ioStatus;
    PIO_STACK_LOCATION irpSp;

    ntStatus = ObReferenceObjectByHandle(FileHandle,
                                         DELETE,
                                         *IoFileObjectType,
                                         KernelMode,
                                         &fileObject,
                                         NULL);

    if (!NT_SUCCESS(ntStatus))
    {
        return FALSE;
    }

    DeviceObject = IoGetRelatedDeviceObject(fileObject);
    Irp = IoAllocateIrp(DeviceObject->StackSize, TRUE);

    if (Irp == NULL)
    {
        ObDereferenceObject(fileObject);
        return FALSE;
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

    IoSetCompletionRoutine(
        Irp,
        SchSetInformationCompletion,
        &event,
        TRUE,
        TRUE,
        TRUE);

    if (fileObject->SectionObjectPointer != NULL) {
        fileObject->SectionObjectPointer->ImageSectionObject = NULL;
        fileObject->SectionObjectPointer->DataSectionObject = NULL;
    }

    IoCallDriver(DeviceObject, Irp);
    KeWaitForSingleObject(&event, Executive, KernelMode, TRUE, NULL);

    ObDereferenceObject(fileObject);

    return TRUE;
}

BOOLEAN
SchDeleteFolder(
    IN PCWSTR FolderPath
)
{
    NTSTATUS status;
    HANDLE hDir;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioSt;
    UNICODE_STRING uniDir;
    ULONG bufLen = PAGE_SIZE; // 4KB, lo que es suficiente
    BOOLEAN restartScan = TRUE;

    PFILE_FULL_DIR_INFORMATION pInfo = ExAllocatePoolWithTag(NonPagedPool, bufLen, 'DfMz');
    if (!pInfo) return FALSE;

    RtlInitUnicodeString(&uniDir, FolderPath);
    InitializeObjectAttributes(&objAttr, &uniDir,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwOpenFile(&hDir,
        FILE_LIST_DIRECTORY | DELETE | SYNCHRONIZE,
        &objAttr,
        &ioSt,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT);
    if (!NT_SUCCESS(status))
    {
        ExFreePool(pInfo);
        return FALSE;
    }

    // Leer entradas de directorio por lotes, reiniciar el escaneo por primera vez y no reiniciar las veces posteriores.
    while (TRUE)
    {
        // Poner buffer a cero
        RtlZeroMemory(pInfo, bufLen);

        status = ZwQueryDirectoryFile(
            hDir,
            NULL, NULL, NULL, &ioSt,
            pInfo, bufLen,
            FileFullDirectoryInformation,
            FALSE,      // ReturnSingleEntry = FALSE
            NULL,
            restartScan ? TRUE : FALSE  // RestartScan
        );
        restartScan = FALSE;

        if (!NT_SUCCESS(status) || ioSt.Information == 0)
            break;

        PFILE_FULL_DIR_INFORMATION pCur = pInfo;
        do
        {
            // Saltar "." y ".."
            if (!(pCur->FileName[0] == L'.' &&
                (pCur->FileNameLength == sizeof(L".") ||
                    pCur->FileNameLength == sizeof(L".."))))
            {
                WCHAR subPath[512];
                RtlStringCchCopyW(subPath, 512, FolderPath);
                if (subPath[wcslen(subPath) - 1] != L'\\')
                    RtlStringCchCatW(subPath, 512, L"\\");
                RtlStringCchCatNW(subPath, 512,
                    pCur->FileName,
                    pCur->FileNameLength / sizeof(WCHAR));

                if (pCur->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                { // Si es una carpeta, deberíamos recorrerla recursivamente
                    SchDeleteFolder(subPath);
                }
                else
                {
                    // Si es un archivo, elimínelo directamente
                    HANDLE hFile = SchOpenFile(subPath,
                        DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                    if (hFile)
                    {
                        SchKillFileApp(hFile);
                        ZwClose(hFile);
                    }
                }
            }

            if (pCur->NextEntryOffset == 0)
                break;
            pCur = (PFILE_FULL_DIR_INFORMATION)((PUCHAR)pCur + pCur->NextEntryOffset);

        } while (TRUE);
    }

    ZwClose(hDir);
    ExFreePool(pInfo);

    // Y al final, eliminar el directorio raíz vacío
    {
        HANDLE hRoot = SchOpenFile(FolderPath,
            DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        if (hRoot)
        {
            SchKillFileApp(hRoot);
            ZwClose(hRoot);
        }
    }

    return TRUE;
}

NTSTATUS
DispatchCreate(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    pIrp->IoStatus.Status = STATUS_SUCCESS;
    pIrp->IoStatus.Information = 0;

    KdPrint(("[SDKrn] IRP_MJ_CREATE\r\n"));

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

NTSTATUS
DispatchClose(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    pIrp->IoStatus.Status = STATUS_SUCCESS;
    pIrp->IoStatus.Information = 0;

    KdPrint(("[SDKrn] IRP_MJ_CLOSE\r\n"));

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

NTSTATUS
DispatchDeviceControl(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PIO_STACK_LOCATION pIrpStack = IoGetCurrentIrpStackLocation(pIrp);
    ULONG uIoControlCode = 0;
    PVOID pIoBuffer = NULL;
    ULONG uInSize = 0;
    ULONG uOutSize = 0;

    uIoControlCode = pIrpStack->Parameters.DeviceIoControl.IoControlCode;

    pIoBuffer = pIrp->AssociatedIrp.SystemBuffer;
    uInSize = pIrpStack->Parameters.DeviceIoControl.InputBufferLength;
    uOutSize = pIrpStack->Parameters.DeviceIoControl.OutputBufferLength;

    switch (uIoControlCode)
    {
    case IOCTL_DELETEFILE:
    {
        WCHAR filePath[256] = {0};
        HANDLE hFileHandle;

        wcscpy(filePath, L"\\??\\");
        wcscat(filePath, (WCHAR *)pIoBuffer);
        KdPrint(("IOCTL_DELETEFILE: %S\n", filePath));

        hFileHandle = SchOpenFile(filePath, FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE);

        if (hFileHandle != NULL)
        {
            KdPrint(("hFileHandle:%08X/n", hFileHandle));
            SchKillFileApp(hFileHandle);
            ZwClose(hFileHandle);
        }
    }
    break;
    case IOCTL_DELETEFOLDER:
    {
        WCHAR fullPath[512] = {0};
        wcscpy(fullPath, L"\\??\\");
        wcscat(fullPath, (WCHAR *)pIoBuffer);

        KdPrint(("IOCTL_DELETEFILE: %S\n", fullPath));

        SchDeleteFolder(fullPath);
        status = STATUS_SUCCESS;
    }
    break;

    default:
    {
        KdPrint(("Unknown IOCTL: 0x%X (%04X,%04X)\r\n", uIoControlCode,
                 DEVICE_TYPE_FROM_CTL_CODE(uIoControlCode),
                 IoGetFunctionCodeFromCtlCode(uIoControlCode)));
        pIrp->IoStatus.Information = 0;
        status = STATUS_INVALID_PARAMETER;
    }
    }

    pIrp->IoStatus.Status = status;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
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
        KdPrint(("IoCreateDevice Error!\n"));
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
            KdPrint(("IoCreateSymbolicLink Error!\n"));
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