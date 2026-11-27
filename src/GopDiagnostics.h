
#ifndef NVIDIA_GOP_DIAGNOSTICS_H
#define NVIDIA_GOP_DIAGNOSTICS_H

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

typedef struct {
  EFI_HANDLE Handle;
  UINT32 ModeNumber;
  UINT32 HorizontalResolution;
  UINT32 VerticalResolution;
  EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
  EFI_PHYSICAL_ADDRESS FrameBufferBase;
  UINTN FrameBufferSize;
  BOOLEAN Valid;
} NVIDIA_GOP_SNAPSHOT;

EFI_STATUS
EFIAPI
CaptureNvidiaGop (
  IN EFI_HANDLE ImageHandle,
  OUT NVIDIA_GOP_SNAPSHOT *Snapshot
  );

VOID
EFIAPI
RunNvidiaGopPostResizeDiagnostics (
  IN EFI_HANDLE ImageHandle,
  IN CONST NVIDIA_GOP_SNAPSHOT *Snapshot
  );

#endif
