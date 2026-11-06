// chimera_guard.c — Chimera Kernel Guard
// Process protection ONLY via ObRegisterCallbacks.
// NO keyboard filter — kept simple and stable.

#include <ntddk.h>
#include "chimera_guard.h"

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE         ((ACCESS_MASK)0x0001U)
#define PROCESS_SUSPEND_RESUME    ((ACCESS_MASK)0x0800U)
#endif

static PDEVICE_OBJECT g_Device   = NULL;
static HANDLE         g_ObHandle = NULL;
static volatile LONG  g_ProtPid1 = 0;
static volatile LONG  g_ProtPid2 = 0;

static OB_PREOP_CALLBACK_STATUS
ChimeraProcessPre(PVOID Ctx, POB_PRE_OPERATION_INFORMATION Info)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (Info->ObjectType != *PsProcessType) return OB_PREOP_SUCCESS;
    if (Info->KernelHandle) return OB_PREOP_SUCCESS;

    ULONG target = (ULONG)(ULONG_PTR)PsGetProcessId((PEPROCESS)Info->Object);
    ULONG p1 = (ULONG)InterlockedCompareExchange(&g_ProtPid1, 0, 0);
    ULONG p2 = (ULONG)InterlockedCompareExchange(&g_ProtPid2, 0, 0);
    if (target == 0 || (target != p1 && target != p2)) return OB_PREOP_SUCCESS;

    const ACCESS_MASK STRIP = PROCESS_TERMINATE | PROCESS_SUSPEND_RESUME;
    if (Info->Operation == OB_OPERATION_HANDLE_CREATE)
        Info->Parameters->CreateHandleInformation.DesiredAccess &= ~STRIP;
    else
        Info->Parameters->DuplicateHandleInformation.DesiredAccess &= ~STRIP;
    return OB_PREOP_SUCCESS;
}

static NTSTATUS RegisterCallbacks(void)
{
    OB_OPERATION_REGISTRATION opReg;
    RtlZeroMemory(&opReg, sizeof(opReg));
    opReg.ObjectType   = PsProcessType;
    opReg.Operations   = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    opReg.PreOperation = ChimeraProcessPre;
    OB_CALLBACK_REGISTRATION cbReg;
    RtlZeroMemory(&cbReg, sizeof(cbReg));
    cbReg.Version = OB_FLT_REGISTRATION_VERSION;
    cbReg.OperationRegistrationCount = 1;
    cbReg.OperationRegistration = &opReg;
    UNICODE_STRING alt;
    RtlInitUnicodeString(&alt, L"389001");
    cbReg.Altitude = alt;
    return ObRegisterCallbacks(&cbReg, &g_ObHandle);
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT D, PIRP Irp)
{
    UNREFERENCED_PARAMETER(D);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchIoctl(PDEVICE_OBJECT D, PIRP Irp)
{
    UNREFERENCED_PARAMETER(D);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    if (s->Parameters.DeviceIoControl.IoControlCode == IOCTL_CHIMERA_PROTECT)
    {
        if (s->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG) &&
            Irp->AssociatedIrp.SystemBuffer)
        {
            ULONG pid = *(ULONG*)Irp->AssociatedIrp.SystemBuffer;
            ULONG c1 = (ULONG)InterlockedCompareExchange(&g_ProtPid1, 0, 0);
            if (c1 == 0 || pid == 0)
                InterlockedExchange(&g_ProtPid1, (LONG)pid);
            else
                InterlockedExchange(&g_ProtPid2, (LONG)pid);
            status = STATUS_SUCCESS;
        }
        else status = STATUS_INVALID_PARAMETER;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID DriverUnload(PDRIVER_OBJECT Drv)
{
    UNREFERENCED_PARAMETER(Drv);
    if (g_ObHandle) { ObUnRegisterCallbacks(g_ObHandle); g_ObHandle = NULL; }
    UNICODE_STRING sym;
    RtlInitUnicodeString(&sym, L"\\DosDevices\\ChimeraGuard");
    IoDeleteSymbolicLink(&sym);
    if (g_Device) { IoDeleteDevice(g_Device); g_Device = NULL; }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Reg)
{
    UNREFERENCED_PARAMETER(Reg);
    UNICODE_STRING dev, sym;
    RtlInitUnicodeString(&dev, L"\\Device\\ChimeraGuard");
    RtlInitUnicodeString(&sym, L"\\DosDevices\\ChimeraGuard");

    NTSTATUS st = IoCreateDevice(Drv, 0, &dev, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, FALSE, &g_Device);
    if (!NT_SUCCESS(st)) return st;

    st = IoCreateSymbolicLink(&sym, &dev);
    if (!NT_SUCCESS(st)) { IoDeleteDevice(g_Device); return st; }

    st = RegisterCallbacks();
    if (!NT_SUCCESS(st)) { IoDeleteSymbolicLink(&sym); IoDeleteDevice(g_Device); return st; }

    Drv->DriverUnload = DriverUnload;
    Drv->MajorFunction[IRP_MJ_CREATE]         = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_CLOSE]          = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchIoctl;
    g_Device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
