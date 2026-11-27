
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>
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
  } if (!EFI_ERROR(SetModeStatus)) {
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
