#include <ntifs.h>

extern "C" {
    NTKERNELAPI NTSTATUS IoCreateDriver(PUNICODE_STRING DriverName,
                                        PDRIVER_INITIALIZE InitializationFunction);

    NTKERNELAPI NTSTATUS MmCopyVirtualMemory(PEPROCESS SourceProcess, PVOID SourceAddress,
                                             PEPROCESS TargetProcess, PVOID TargetAddress,
                                             SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
                                             PSIZE_T ReturnSize);
}

// Helper for kernel debug printing.
void debug_print(PCSTR text) {
// Otherwise you cannot build in Release mode.
#ifndef DEBUG
    UNREFERENCED_PARAMETER(text);
#endif  // !DEBUG

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, text));
}

namespace driver {
    namespace codes {
        // used to setup the driver.
        constexpr ULONG attach =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

        // read process memory
        constexpr ULONG read =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

        // write process memory
        constexpr ULONG write =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
    }  // namespace codes

    // shared between user mode & kernel mode.
    struct Request {
        HANDLE process_id;

        PVOID target;
        PVOID buffer;

        SIZE_T size;
        SIZE_T return_size;
    };

    NTSTATUS create(PDEVICE_OBJECT device_object, PIRP irp) {
        UNREFERENCED_PARAMETER(device_object);

        IoCompleteRequest(irp, IO_NO_INCREMENT);

        return irp->IoStatus.Status;
    }

    NTSTATUS close(PDEVICE_OBJECT device_object, PIRP irp) {
        UNREFERENCED_PARAMETER(device_object);

        IoCompleteRequest(irp, IO_NO_INCREMENT);

        return irp->IoStatus.Status;
    }

    NTSTATUS device_control(PDEVICE_OBJECT device_object, PIRP irp) {
        UNREFERENCED_PARAMETER(device_object);

        debug_print("[+] device control called\n");

        NTSTATUS status = STATUS_UNSUCCESSFUL;

        // we need this to determine which code was passed through
        PIO_STACK_LOCATION stack_irp = IoGetCurrentIrpStackLocation(irp);

        // access the request object sent from user mode
        auto request = reinterpret_cast<Request*>(irp->AssociatedIrp.SystemBuffer);

        if (stack_irp == nullptr || request == nullptr) {
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return status;
        }

        // the target process we want access to
        static PEPROCESS target_process = nullptr;

        const ULONG control_code = stack_irp->Parameters.DeviceIoControl.IoControlCode;
        switch (control_code) {
            case codes::attach:
                status = PsLookupProcessByProcessId(request->process_id, &target_process);
                break;

                // read process memory implementation
            case codes::read:
                if (target_process != nullptr)
                    status = MmCopyVirtualMemory(target_process, request->target,
                                                 PsGetCurrentProcess(), request->buffer,
                                                 request->size, KernelMode, &request->return_size);
                break;

            case codes::write:
                if (target_process != nullptr)
                    status = MmCopyVirtualMemory(PsGetCurrentProcess(), request->buffer,
                                                 target_process, request->target, request->size,
                                                 KernelMode, &request->return_size);
                break;

            default:
                break;
        }

        irp->IoStatus.Status = status;
        irp->IoStatus.Information = sizeof(Request);

        IoCompleteRequest(irp, IO_NO_INCREMENT);

        return status;
    }

}  // namespace driver

// Our "real" entry point.
NTSTATUS driver_main(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);

    UNICODE_STRING device_name = {};
    RtlInitUnicodeString(&device_name, L"\\Device\\IO");

    // Create driver device obj.
    PDEVICE_OBJECT device_object = nullptr;
    NTSTATUS status = IoCreateDevice(driver_object, 0, &device_name, FILE_DEVICE_UNKNOWN,
                                     FILE_DEVICE_SECURE_OPEN, FALSE, &device_object);
    if (status != STATUS_SUCCESS) {
        debug_print("[-] failed to create driver device\n");
        return status;
    }

    debug_print("[+] driver device created\n");

    UNICODE_STRING symbolic_link = {};
    RtlInitUnicodeString(&symbolic_link, L"\\DosDevices\\IO");

    status = IoCreateSymbolicLink(&symbolic_link, &device_name);
    if (status != STATUS_SUCCESS) {
        debug_print("[-] failed to establish symbolic link\n");
        return status;
    }

    debug_print("[+] symbolic link established\n");

    // allow us to send small amounts of data between um/km
    SetFlag(device_object->Flags, DO_BUFFERED_IO);

    // set the driver handlers to our functions with our logic
    driver_object->MajorFunction[IRP_MJ_CREATE] = driver::create;
    driver_object->MajorFunction[IRP_MJ_CLOSE] = driver::close;
    driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = driver::device_control;

    // we have initialized our device
    ClearFlag(device_object->Flags, DO_DEVICE_INITIALIZING);

    debug_print("[+] driver loaded\n");

    return status;
}

// mappers "entry point"
NTSTATUS DriverEntry() {
    debug_print("[+] kernel loaded\n");

    UNICODE_STRING driver_name = {};
    RtlInitUnicodeString(&driver_name, L"\\Driver\\IO");

    return IoCreateDriver(&driver_name, &driver_main);
}
