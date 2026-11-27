
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>

#include "GopDiagnostics.h"

#define NVIDIA_VENDOR_ID  0x10DE
#define NVIDIA_DEVICE_ID  0x1F36
#define GOP_TEST_WIDTH    8
#define GOP_TEST_HEIGHT   8

/*
 * Machine-specific BAR1 locations established by the Linux-tested layout.
 * The old framebuffer base was observed as 0xA0000000 before resizing.
 */
#define NVIDIA_OLD_BAR1_BASE     0xA0000000ULL
#define NVIDIA_NEW_BAR1_BASE     0x4200000000ULL
#define NVIDIA_NEW_BAR1_SIZE     0x200000000ULL

STATIC
VOID
ReportNvidiaGopFramebuffer (
  IN CONST CHAR16 *When,
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop
  )
{
  UINT64 FbBase;
  UINT64 FbSize;
  UINT64 FbEnd;
  UINT64 Bar1End;
  UINT64 MaxUint64;

  if (Gop == NULL || Gop->Mode == NULL) {
    Print(L"GOP %s: no mode structure\n", When);
    return;
  }

  FbBase = Gop->Mode->FrameBufferBase;
  FbSize = Gop->Mode->FrameBufferSize;
  MaxUint64 = ~(UINT64)0;
  Bar1End = NVIDIA_NEW_BAR1_BASE + NVIDIA_NEW_BAR1_SIZE - 1;

  if (FbSize == 0 || FbBase > MaxUint64 - (FbSize - 1)) {
    Print(
      L"GOP %s: invalid framebuffer range base=%016Lx size=%016Lx\n",
      When, FbBase, FbSize
      );
    return;
  }

  FbEnd = FbBase + FbSize - 1;

  Print(
    L"GOP %s framebuffer: %016Lx-%016Lx (%Lu bytes)\n",
    When, FbBase, FbEnd, FbSize
    );

  if (FbBase >= NVIDIA_NEW_BAR1_BASE && FbEnd <= Bar1End) {
    Print(L"GOP %s: framebuffer is inside the new 8-GiB BAR1 range\n", When);

  } else if (FbBase == NVIDIA_OLD_BAR1_BASE) {
    Print(L"GOP %s: STALE OLD BAR1 BASE — GOP has not remapped its framebuffer\n",
          When);

  } else {
    Print(L"GOP %s: framebuffer is outside the new BAR1 range\n", When);
  }
}

STATIC
EFI_STATUS
OpenNvidiaGop (
  IN EFI_HANDLE ImageHandle,
  OUT EFI_HANDLE *GopHandle,
  OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **Gop
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;

  *GopHandle = NULL;
  *Gop = NULL;
  Handles = NULL;
  Count = 0;

  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );

  if (EFI_ERROR(Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Candidate;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
    EFI_DEVICE_PATH_PROTOCOL *Remaining;
    EFI_HANDLE PciHandle;
    EFI_PCI_IO_PROTOCOL *PciIo;
    UINTN Segment, Bus, Device, Function;
    UINT32 Id;

    Candidate = NULL;
    DevicePath = NULL;
    PciIo = NULL;
    PciHandle = NULL;

    Status = gBS->OpenProtocol(
                    Handles[Index],
                    &gEfiGraphicsOutputProtocolGuid,
                    (VOID **)&Candidate,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );

    if (EFI_ERROR(Status) || Candidate == NULL) {
      continue;
    }

    Status = gBS->OpenProtocol(
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );

    if (EFI_ERROR(Status) || DevicePath == NULL) {
      gBS->CloseProtocol(
             Handles[Index],
             &gEfiGraphicsOutputProtocolGuid,
             ImageHandle,
             NULL
             );
      continue;
    }

    Remaining = DevicePath;

    Status = gBS->LocateDevicePath(
                    &gEfiPciIoProtocolGuid,
                    &Remaining,
                    &PciHandle
                    );

    if (!EFI_ERROR(Status) && PciHandle != NULL) {
      Status = gBS->OpenProtocol(
                      PciHandle,
                      &gEfiPciIoProtocolGuid,
                      (VOID **)&PciIo,
                      ImageHandle,
                      NULL,
                      EFI_OPEN_PROTOCOL_GET_PROTOCOL
                      );

      if (!EFI_ERROR(Status) && PciIo != NULL) {
        Status = PciIo->GetLocation(
                          PciIo,
                          &Segment,
                          &Bus,
                          &Device,
                          &Function
                          );

        if (!EFI_ERROR(Status) &&
            Segment == 0 &&
            Bus == 1 &&
            Device == 0 &&
            Function == 0) {

          Status = PciIo->Pci.Read(
                              PciIo,
                              EfiPciIoWidthUint32,
                              0,
                              1,
                              &Id
                              );

          if (!EFI_ERROR(Status) &&
              (UINT16)(Id & 0xFFFF) == NVIDIA_VENDOR_ID &&
              (UINT16)(Id >> 16) == NVIDIA_DEVICE_ID) {

            gBS->CloseProtocol(
                   PciHandle,
                   &gEfiPciIoProtocolGuid,
                   ImageHandle,
                   NULL
                   );

            gBS->CloseProtocol(
                   Handles[Index],
                   &gEfiDevicePathProtocolGuid,
                   ImageHandle,
                   NULL
                   );

            *GopHandle = Handles[Index];
            *Gop = Candidate;

            FreePool(Handles);
            return EFI_SUCCESS;
          }

        }

        gBS->CloseProtocol(
               PciHandle,
               &gEfiPciIoProtocolGuid,
               ImageHandle,
               NULL
               );
      }
    }

    gBS->CloseProtocol(
           Handles[Index],
           &gEfiDevicePathProtocolGuid,
           ImageHandle,
           NULL
           );

    gBS->CloseProtocol(
           Handles[Index],
           &gEfiGraphicsOutputProtocolGuid,
           ImageHandle,
           NULL
           );
  }

  FreePool(Handles);
  return EFI_NOT_FOUND;
}

STATIC
VOID
CloseNvidiaGop (
  IN EFI_HANDLE ImageHandle,
  IN EFI_HANDLE GopHandle
  )
{
  if (GopHandle != NULL) {
    gBS->CloseProtocol(
           GopHandle,
           &gEfiGraphicsOutputProtocolGuid,
           ImageHandle,
           NULL
           );
  }
}

STATIC
EFI_STATUS
GopBltRoundTrip (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  OUT EFI_STATUS *ReadStatus,
  OUT EFI_STATUS *WriteStatus
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
  UINTN BufferBytes;
  EFI_STATUS Result;

  *ReadStatus = EFI_NOT_STARTED;
  *WriteStatus = EFI_NOT_STARTED;

  BufferBytes =
    GOP_TEST_WIDTH *
    GOP_TEST_HEIGHT *
    sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  Pixels = AllocatePool(BufferBytes);
  if (Pixels == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *ReadStatus = Gop->Blt(
                       Gop,
                       Pixels,
                       EfiBltVideoToBltBuffer,
                       0, 0,
                       0, 0,
                       GOP_TEST_WIDTH,
                       GOP_TEST_HEIGHT,
                       GOP_TEST_WIDTH *
                         sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                       );

  if (!EFI_ERROR(*ReadStatus)) {
    *WriteStatus = Gop->Blt(
                         Gop,
                         Pixels,
                         EfiBltBufferToVideo,
                         0, 0,
                         0, 0,
                         GOP_TEST_WIDTH,
                         GOP_TEST_HEIGHT,
                         GOP_TEST_WIDTH *
                           sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                         );
  }

  Result = EFI_ERROR(*ReadStatus) ?
           *ReadStatus : *WriteStatus;

  FreePool(Pixels);
  return Result;
}

EFI_STATUS
EFIAPI
CaptureNvidiaGop (
  IN EFI_HANDLE ImageHandle,
  OUT NVIDIA_GOP_SNAPSHOT *Snapshot
  )
{
  EFI_STATUS Status;
  EFI_HANDLE Handle;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

  if (Snapshot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  SetMem(Snapshot, sizeof(*Snapshot), 0);

  Status = OpenNvidiaGop(ImageHandle, &Handle, &Gop);
  if (EFI_ERROR(Status)) {
    Print(L"GOP pre: NVIDIA GOP not found (%r)\n", Status);
    return Status;
  }

  if (Gop->Mode == NULL || Gop->Mode->Info == NULL) {
    Status = EFI_DEVICE_ERROR;
  } else {
    Snapshot->Handle = Handle;
    Snapshot->ModeNumber = Gop->Mode->Mode;
    Snapshot->HorizontalResolution =
      Gop->Mode->Info->HorizontalResolution;
    Snapshot->VerticalResolution =
      Gop->Mode->Info->VerticalResolution;
    Snapshot->PixelFormat = Gop->Mode->Info->PixelFormat;
    Snapshot->FrameBufferBase = Gop->Mode->FrameBufferBase;
    Snapshot->FrameBufferSize = Gop->Mode->FrameBufferSize;
    Snapshot->Valid = TRUE;

    Print(
      L"GOP pre: mode=%u %ux%u fb=%016Lx/%Lu fmt=%u\n",
      Snapshot->ModeNumber,
      Snapshot->HorizontalResolution,
      Snapshot->VerticalResolution,
      Snapshot->FrameBufferBase,
      Snapshot->FrameBufferSize,
      Snapshot->PixelFormat
      );

    Status = EFI_SUCCESS;
  }

  CloseNvidiaGop(ImageHandle, Handle);
  return Status;
}

VOID
EFIAPI
RunNvidiaGopPostResizeDiagnostics (
  IN EFI_HANDLE ImageHandle,
  IN CONST NVIDIA_GOP_SNAPSHOT *Snapshot
  )
{
  EFI_STATUS Status;
  EFI_STATUS ReadBefore, WriteBefore;
  EFI_STATUS SetModeStatus;
  EFI_STATUS ReadAfter, WriteAfter;
  EFI_HANDLE Handle;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  BOOLEAN SameHandle;

  Status = OpenNvidiaGop(ImageHandle, &Handle, &Gop);
  if (EFI_ERROR(Status)) {
    Print(L"GOP post: NVIDIA GOP unavailable (%r)\n", Status);
    return;
  }

  if (Gop->Mode == NULL || Gop->Mode->Info == NULL) {
    Print(L"GOP post: mode information unavailable\n");
    ReportNvidiaGopFramebuffer(L"before recovery", Gop);
    CloseNvidiaGop(ImageHandle, Handle);
    return;
  }

  SameHandle =
    Snapshot != NULL &&
    Snapshot->Valid &&
    Snapshot->Handle == Handle;

  Print(
    L"GOP post: handle=%s mode=%u %ux%u fb=%016Lx/%Lu\n",
    SameHandle ? L"same" : L"changed",
    Gop->Mode->Mode,
    Gop->Mode->Info->HorizontalResolution,
    Gop->Mode->Info->VerticalResolution,
    Gop->Mode->FrameBufferBase,
    Gop->Mode->FrameBufferSize
    );

  Status = GopBltRoundTrip(
             Gop,
             &ReadBefore,
             &WriteBefore
             );

  Print(
    L"GOP Blt before SetMode: read=%r write=%r result=%r\n",
    ReadBefore,
    WriteBefore,
    Status
    );

  if (Snapshot == NULL ||
      !Snapshot->Valid ||
      Snapshot->ModeNumber >= Gop->Mode->MaxMode) {

    SetModeStatus = EFI_NOT_FOUND;

  } else if (!SameHandle) {

    // Do not apply a saved mode index to a different GOP instance.
    SetModeStatus = EFI_ABORTED;

  } else {

    SetModeStatus =
      Gop->SetMode(Gop, Snapshot->ModeNumber);
  }
  
  ReportNvidiaGopFramebuffer(L"before SetMode", Gop);
  
  if (!EFI_ERROR(SetModeStatus)) {
    ReportNvidiaGopFramebuffer(L"after SetMode", Gop);
  }
  
  Print(
    L"GOP SetMode(saved=%u): %r\n",
    (Snapshot != NULL && Snapshot->Valid) ?
      Snapshot->ModeNumber : 0,
    SetModeStatus
    );

  Status = GopBltRoundTrip(
             Gop,
             &ReadAfter,
             &WriteAfter
             );

  Print(
    L"GOP Blt after SetMode: read=%r write=%r result=%r\n",
    ReadAfter,
    WriteAfter,
    Status
    );

  CloseNvidiaGop(ImageHandle, Handle);
}

//
// Experimental NVIDIA GOP driver reconnect.
//
// This does not resize BARs, reset the GPU, or modify PCI configuration.
// It disconnects and reconnects only the driver owning the target GPU.
//
#define NVIDIA_GPU_BUS          1
#define NVIDIA_GPU_DEVICE       0
#define NVIDIA_GPU_FUNCTION     0

#define NVIDIA_ROOT_BUS         0
#define NVIDIA_ROOT_DEVICE      1
#define NVIDIA_ROOT_FUNCTION    0

#define NVIDIA_GPU_BAR0         0x10
#define NVIDIA_GPU_BAR1_LOW     0x14
#define NVIDIA_GPU_BAR1_HIGH    0x18
#define NVIDIA_GPU_BAR3_LOW     0x1C
#define NVIDIA_GPU_BAR3_HIGH    0x20
#define NVIDIA_GPU_BAR5         0x24

#define NVIDIA_GPU_VSEC_610     0x610
#define NVIDIA_GPU_REBAR_CTRL   0xBC0

#define NVIDIA_ROOT_PREF_LOW    0x24
#define NVIDIA_ROOT_PREF_BASE   0x28
#define NVIDIA_ROOT_PREF_LIMIT  0x2C

typedef struct {
  UINT32 Bar0;
  UINT32 Bar1Low;
  UINT32 Bar1High;
  UINT32 Bar3Low;
  UINT32 Bar3High;
  UINT32 Bar5;
  UINT32 Vsec610;
  UINT32 RebarControl;

  UINT32 RootPrefLow;
  UINT32 RootPrefBase;
  UINT32 RootPrefLimit;

  BOOLEAN GpuValid;
  BOOLEAN RootValid;
} NVIDIA_PCI_STATE;

STATIC
EFI_STATUS
FindPciController (
  IN EFI_HANDLE ImageHandle,
  IN UINTN WantedSegment,
  IN UINTN WantedBus,
  IN UINTN WantedDevice,
  IN UINTN WantedFunction,
  IN UINT16 WantedVendor,
  IN UINT16 WantedDeviceId,
  OUT EFI_HANDLE *ControllerHandle
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  EFI_PCI_IO_PROTOCOL *PciIo;
  UINTN Segment;
  UINTN Bus;
  UINTN Device;
  UINTN Function;
  UINT32 Id;

  if (ControllerHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *ControllerHandle = NULL;
  Handles = NULL;
  Count = 0;

  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );

  if (EFI_ERROR(Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    PciIo = NULL;

    Status = gBS->OpenProtocol(
                    Handles[Index],
                    &gEfiPciIoProtocolGuid,
                    (VOID **)&PciIo,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );

    if (EFI_ERROR(Status) || PciIo == NULL) {
      continue;
    }

    Status = PciIo->GetLocation(
                      PciIo,
                      &Segment,
                      &Bus,
                      &Device,
                      &Function
                      );

    if (!EFI_ERROR(Status) &&
        Segment == WantedSegment &&
        Bus == WantedBus &&
        Device == WantedDevice &&
        Function == WantedFunction) {

      Status = PciIo->Pci.Read(
                          PciIo,
                          EfiPciIoWidthUint32,
                          0,
                          1,
                          &Id
                          );

      if (!EFI_ERROR(Status) &&
          (UINT16)(Id & 0xFFFF) == WantedVendor &&
          (UINT16)(Id >> 16) == WantedDeviceId) {

        *ControllerHandle = Handles[Index];

        gBS->CloseProtocol(
               Handles[Index],
               &gEfiPciIoProtocolGuid,
               ImageHandle,
               NULL
               );

        FreePool(Handles);
        return EFI_SUCCESS;
      }
    }

    gBS->CloseProtocol(
           Handles[Index],
           &gEfiPciIoProtocolGuid,
           ImageHandle,
           NULL
           );
  }

  FreePool(Handles);
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
FindNvidiaDriverImage (
  IN EFI_HANDLE GpuHandle,
  OUT EFI_HANDLE *DriverImageHandle
  )
{
  EFI_STATUS Status;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Entries;
  UINTN EntryCount;
  UINTN Index;
  EFI_HANDLE DriverAgent;
  EFI_DRIVER_BINDING_PROTOCOL *Binding;

  if (DriverImageHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DriverImageHandle = NULL;
  Entries = NULL;
  EntryCount = 0;
  DriverAgent = NULL;

  Status = gBS->OpenProtocolInformation(
                  GpuHandle,
                  &gEfiPciIoProtocolGuid,
                  &Entries,
                  &EntryCount
                  );

  if (EFI_ERROR(Status)) {
    Print(L"GOP recovery: OpenProtocolInformation failed: %r\n",
          Status);
    return Status;
  }

  for (Index = 0; Index < EntryCount; Index++) {
    if ((Entries[Index].Attributes &
         EFI_OPEN_PROTOCOL_BY_DRIVER) == 0) {
      continue;
    }

    if (DriverAgent != NULL) {
      Print(L"GOP recovery: multiple BY_DRIVER owners found.\n");
      for (Index = 0; Index < EntryCount; Index++) {
        if (Entries[Index].AgentHandle == DriverAgent &&
            (Entries[Index].Attributes &
             EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) != 0) {
      
          Print(
            L"  NVIDIA driver child controller: %p (opens=%u)\n",
            Entries[Index].ControllerHandle,
            Entries[Index].OpenCount
            );
        }
      }
      if (Entries != NULL) {
        FreePool(Entries);
      }
      return EFI_ACCESS_DENIED;
    }

    DriverAgent = Entries[Index].AgentHandle;
  }

  FreePool(Entries);

  if (DriverAgent == NULL) {
    Print(L"GOP recovery: no BY_DRIVER owner found.\n");
    return EFI_NOT_FOUND;
  }

  Binding = NULL;

  Status = gBS->HandleProtocol(
                  DriverAgent,
                  &gEfiDriverBindingProtocolGuid,
                  (VOID **)&Binding
                  );

  if (EFI_ERROR(Status) || Binding == NULL) {
    Print(L"GOP recovery: cannot retrieve Driver Binding: %r\n",
          Status);
    return EFI_NOT_FOUND;
  }

  if (Binding->ImageHandle == NULL) {
    Print(L"GOP recovery: driver has a NULL image handle.\n");
    return EFI_DEVICE_ERROR;
  }

  *DriverImageHandle = Binding->ImageHandle;

  Print(L"GOP recovery: NVIDIA driver identified.\n");
  Print(L"  Agent handle:        %p\n", DriverAgent);
  Print(L"  Driver image handle: %p\n", Binding->ImageHandle);
  Print(L"  Binding handle:      %p\n", Binding->DriverBindingHandle);
  Print(L"  Driver version:      %08x\n", Binding->Version);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReadPciDword (
  IN EFI_HANDLE ImageHandle,
  IN EFI_HANDLE ControllerHandle,
  IN UINT32 Offset,
  OUT UINT32 *Value
  )
{
  EFI_STATUS Status;
  EFI_PCI_IO_PROTOCOL *PciIo;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  PciIo = NULL;

  Status = gBS->OpenProtocol(
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read(
                      PciIo,
                      EfiPciIoWidthUint32,
                      Offset,
                      1,
                      Value
                      );

  gBS->CloseProtocol(
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         ImageHandle,
         NULL
         );

  return Status;
}

STATIC
EFI_STATUS
CaptureNvidiaPciState (
  IN EFI_HANDLE ImageHandle,
  OUT NVIDIA_PCI_STATE *State
  )
{
  EFI_STATUS Status;
  EFI_HANDLE GpuHandle;
  EFI_HANDLE RootHandle;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  SetMem(State, sizeof(*State), 0);

  Status = FindPciController(
             ImageHandle,
             0,
             NVIDIA_GPU_BUS,
             NVIDIA_GPU_DEVICE,
             NVIDIA_GPU_FUNCTION,
             NVIDIA_VENDOR_ID,
             NVIDIA_DEVICE_ID,
             &GpuHandle
             );

  if (EFI_ERROR(Status)) {
    return Status;
  }

#define READ_GPU_FIELD(Field, Offset)                    \
  do {                                                   \
    Status = ReadPciDword(ImageHandle, GpuHandle,        \
                          Offset, &State->Field);        \
    if (EFI_ERROR(Status)) {                             \
      Print(L"PCI read failed at offset %03x: %r\n",     \
            Offset, Status);                             \
      return Status;                                     \
    }                                                    \
  } while (0)

  READ_GPU_FIELD(Bar0, NVIDIA_GPU_BAR0);
  READ_GPU_FIELD(Bar1Low, NVIDIA_GPU_BAR1_LOW);
  READ_GPU_FIELD(Bar1High, NVIDIA_GPU_BAR1_HIGH);
  READ_GPU_FIELD(Bar3Low, NVIDIA_GPU_BAR3_LOW);
  READ_GPU_FIELD(Bar3High, NVIDIA_GPU_BAR3_HIGH);
  READ_GPU_FIELD(Bar5, NVIDIA_GPU_BAR5);
  READ_GPU_FIELD(Vsec610, NVIDIA_GPU_VSEC_610);
  READ_GPU_FIELD(RebarControl, NVIDIA_GPU_REBAR_CTRL);

#undef READ_GPU_FIELD

  State->GpuValid = TRUE;

  Status = FindPciController(
             ImageHandle,
             0,
             NVIDIA_ROOT_BUS,
             NVIDIA_ROOT_DEVICE,
             NVIDIA_ROOT_FUNCTION,
             0x8086,
             0x1901,
             &RootHandle
             );

  if (!EFI_ERROR(Status)) {
    if (!EFI_ERROR(ReadPciDword(
                     ImageHandle, RootHandle,
                     NVIDIA_ROOT_PREF_LOW,
                     &State->RootPrefLow)) &&
        !EFI_ERROR(ReadPciDword(
                     ImageHandle, RootHandle,
                     NVIDIA_ROOT_PREF_BASE,
                     &State->RootPrefBase)) &&
        !EFI_ERROR(ReadPciDword(
                     ImageHandle, RootHandle,
                     NVIDIA_ROOT_PREF_LIMIT,
                     &State->RootPrefLimit))) {
      State->RootValid = TRUE;
    }
  }

  return EFI_SUCCESS;
}

STATIC
VOID
PrintNvidiaPciState (
  IN CONST CHAR16 *Label,
  IN CONST NVIDIA_PCI_STATE *State
  )
{
  Print(L"\n=== NVIDIA PCI STATE: %s ===\n", Label);

  if (!State->GpuValid) {
    Print(L"GPU state unavailable.\n");
    return;
  }

  Print(L"BAR0:       %08x\n", State->Bar0);
  Print(L"BAR1 low:   %08x\n", State->Bar1Low);
  Print(L"BAR1 high:  %08x\n", State->Bar1High);
  Print(L"BAR3 low:   %08x\n", State->Bar3Low);
  Print(L"BAR3 high:  %08x\n", State->Bar3High);
  Print(L"BAR5:       %08x\n", State->Bar5);
  Print(L"VSEC 0x610: %08x\n", State->Vsec610);
  Print(L"ReBAR BC0:  %08x\n", State->RebarControl);

  if (State->RootValid) {
    Print(L"Root PREF:  %08x\n", State->RootPrefLow);
    Print(L"Root base:  %08x\n", State->RootPrefBase);
    Print(L"Root limit: %08x\n", State->RootPrefLimit);
  } else {
    Print(L"Root-port state unavailable.\n");
  }

  if (State->Bar1Low == 0x0000000C &&
      State->Bar1High == 0x00000042 &&
      State->Bar3Low == 0xFE00000C &&
      State->Bar3High == 0x00000041 &&
      State->Vsec610 == 0x00001B81 &&
      State->RebarControl == 0x00000D01) {
    Print(L"GPU BAR/ReBAR/VSEC expected values: PASS\n");
  } else {
    Print(L"GPU BAR/ReBAR/VSEC expected values: MISMATCH\n");
  }

  if (State->RootValid &&
      State->RootPrefLow == 0xFFF1FE01 &&
      State->RootPrefBase == 0x00000041 &&
      State->RootPrefLimit == 0x00000043) {
    Print(L"Root-port window expected values: PASS\n");
  } else {
    Print(L"Root-port window expected values: NOT VERIFIED\n");
  }

  Print(L"=== END PCI STATE ===\n");
}

STATIC
VOID
CompareNvidiaPciStates (
  IN CONST NVIDIA_PCI_STATE *Before,
  IN CONST NVIDIA_PCI_STATE *After
  )
{
  BOOLEAN Same;

  Same = Before->GpuValid && After->GpuValid &&
         Before->Bar0 == After->Bar0 &&
         Before->Bar1Low == After->Bar1Low &&
         Before->Bar1High == After->Bar1High &&
         Before->Bar3Low == After->Bar3Low &&
         Before->Bar3High == After->Bar3High &&
         Before->Bar5 == After->Bar5 &&
         Before->Vsec610 == After->Vsec610 &&
         Before->RebarControl == After->RebarControl;

  if (Before->RootValid && After->RootValid) {
    Same = Same &&
           Before->RootPrefLow == After->RootPrefLow &&
           Before->RootPrefBase == After->RootPrefBase &&
           Before->RootPrefLimit == After->RootPrefLimit;
  } else {
    Same = FALSE;
  }

  Print(L"\nPCI configuration preserved across reconnect: %s\n",
        Same ? L"YES" : L"NO - REVIEW THE SNAPSHOTS");
}

STATIC
VOID
PrintHandleProtocols (
  IN CONST CHAR16 *Label,
  IN EFI_HANDLE Handle
  )
{
  EFI_STATUS Status;
  EFI_GUID **ProtocolGuids;
  UINTN Count;
  UINTN Index;

  ProtocolGuids = NULL;
  Count = 0;

  Print(L"\nProtocols on %s handle %p:\n", Label, Handle);

  Status = gBS->ProtocolsPerHandle(
                  Handle,
                  &ProtocolGuids,
                  &Count
                  );

  if (EFI_ERROR(Status)) {
    Print(L"  Protocol enumeration failed: %r\n", Status);
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (CompareGuid(ProtocolGuids[Index],
                    &gEfiGraphicsOutputProtocolGuid)) {
      Print(L"  Graphics Output Protocol\n");
    } else if (CompareGuid(ProtocolGuids[Index],
                           &gEfiPciIoProtocolGuid)) {
      Print(L"  PCI I/O Protocol\n");
    } else if (CompareGuid(ProtocolGuids[Index],
                           &gEfiDevicePathProtocolGuid)) {
      Print(L"  Device Path Protocol\n");
    } else if (CompareGuid(ProtocolGuids[Index],
                           &gEfiDriverBindingProtocolGuid)) {
      Print(L"  Driver Binding Protocol\n");
    } else {
      Print(L"  %g\n", ProtocolGuids[Index]);
    }
  }

  if (ProtocolGuids != NULL) {
    FreePool(ProtocolGuids);
  }
}

EFI_STATUS
EFIAPI
RunNvidiaGopDriverReconnect (
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS Status;
  EFI_STATUS DisconnectStatus;
  EFI_STATUS ConnectStatus;
  EFI_STATUS GopStatus;

  EFI_HANDLE GpuHandle;
  EFI_HANDLE DriverImageHandle;
  EFI_HANDLE OldGopHandle;
  EFI_HANDLE NewGopHandle;

  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

  NVIDIA_PCI_STATE Before;
  NVIDIA_PCI_STATE After;

  Print(L"\n");
  Print(L"===============================================\n");
  Print(L" EXPERIMENTAL NVIDIA GOP DRIVER RECONNECT\n");
  Print(L"===============================================\n");

  //
  // Find the exact NVIDIA GPU.
  //
  Status = FindPciController(
             ImageHandle,
             0,
             NVIDIA_GPU_BUS,
             NVIDIA_GPU_DEVICE,
             NVIDIA_GPU_FUNCTION,
             NVIDIA_VENDOR_ID,
             NVIDIA_DEVICE_ID,
             &GpuHandle
             );
  PrintHandleProtocols(L"NVIDIA GPU", GpuHandle);

  if (EFI_ERROR(Status)) {
    Print(L"GOP recovery: target GPU not found: %r\n", Status);
    return Status;
  }

  //
  // Find its actual BY_DRIVER owner and driver image.
  //
  Status = FindNvidiaDriverImage(
             GpuHandle,
             &DriverImageHandle
             );

  if (EFI_ERROR(Status)) {
    Print(L"GOP recovery: driver identification failed: %r\n",
          Status);
    return Status;
  }

  //
  // Capture the known-good resized PCI configuration.
  //
  Status = CaptureNvidiaPciState(ImageHandle, &Before);

  if (EFI_ERROR(Status)) {
    Print(L"GOP recovery: initial PCI snapshot failed: %r\n",
          Status);
    return Status;
  }

  PrintNvidiaPciState(L"BEFORE DISCONNECT", &Before);

  //
  // Record the current GOP handle and framebuffer.
  //
  OldGopHandle = NULL;
  Gop = NULL;

  GopStatus = OpenNvidiaGop(
                ImageHandle,
                &OldGopHandle,
                &Gop
                );

  PrintHandleProtocols(L"Original NVIDIA GOP", OldGopHandle);
  if (!EFI_ERROR(GopStatus)) {
    Print(L"Old GOP handle: %p\n", OldGopHandle);
    ReportNvidiaGopFramebuffer(L"before driver disconnect", Gop);
    CloseNvidiaGop(ImageHandle, OldGopHandle);
  } else {
    Print(L"Old GOP could not be opened: %r\n", GopStatus);
  }

  //
  // Disconnect only the identified NVIDIA driver from this GPU.
  // Child controllers created by this driver may also be disconnected.
  //
  Print(L"\nDisconnecting NVIDIA driver...\n");

  DisconnectStatus = gBS->DisconnectController(
                           GpuHandle,
                           DriverImageHandle,
                           NULL
                           );

  Print(L"DisconnectController: %r\n", DisconnectStatus);

  if (EFI_ERROR(DisconnectStatus)) {
    Print(L"Disconnect failed; reconnect will NOT be attempted.\n");
    return DisconnectStatus;
  }

  //
  // Reconnect only the same NVIDIA driver.
  //
  Print(L"\nReconnecting NVIDIA driver...\n");

  ConnectStatus = gBS->ConnectController(
                        GpuHandle,
                        DriverImageHandle,
                        NULL,
                        TRUE
                        );

  Print(L"ConnectController: %r\n", ConnectStatus);

  //
  // Inspect PCI state even if ConnectController returned an error.
  //
  Status = CaptureNvidiaPciState(ImageHandle, &After);

  if (EFI_ERROR(Status)) {
    Print(L"Post-reconnect PCI snapshot failed: %r\n", Status);
  } else {
    PrintNvidiaPciState(L"AFTER RECONNECT", &After);
    CompareNvidiaPciStates(&Before, &After);
  }

  //
  // Find the GOP again. It may have a different handle.
  //
  NewGopHandle = NULL;
  Gop = NULL;

  GopStatus = OpenNvidiaGop(
                ImageHandle,
                &NewGopHandle,
                &Gop
                );
  
  PrintHandleProtocols(L"Reconnected NVIDIA GOP", NewGopHandle);
  
  if (EFI_ERROR(GopStatus)) {
    Print(L"\nNew NVIDIA GOP not found: %r\n", GopStatus);
  } else {
    Print(L"\nNew GOP handle: %p\n", NewGopHandle);
    Print(L"GOP handle: %s\n",
          NewGopHandle == OldGopHandle ?
            L"same" : L"changed");

    ReportNvidiaGopFramebuffer(L"after driver reconnect", Gop);

    CloseNvidiaGop(ImageHandle, NewGopHandle);
  }

  Print(L"\n=== NVIDIA GOP RECONNECT RESULT ===\n");
  Print(L"Disconnect: %r\n", DisconnectStatus);
  Print(L"Reconnect:  %r\n", ConnectStatus);
  Print(L"GOP lookup: %r\n", GopStatus);
  Print(L"=== END GOP RECONNECT RESULT ===\n");

  if (EFI_ERROR(ConnectStatus)) {
    return ConnectStatus;
  }

  return GopStatus;
}
