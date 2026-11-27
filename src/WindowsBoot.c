// SPDX-License-Identifier: MIT
#include <Uefi.h>

#include <Protocol/SimpleFileSystem.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>

#include "Bar1Log.h"

#define WINDOWS_BOOT_PATH \
  L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"

EFI_STATUS
EFIAPI
BootWindows(
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;

  EFI_DEVICE_PATH_PROTOCOL *BootPath;
  EFI_HANDLE BootImage;

  Handles = NULL;
  Count = 0;

  Bar1LogEvent(L"Searching exposed filesystems for Windows Boot Manager");

  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );

  if (EFI_ERROR(Status)) {
    Bar1LogStatus(L"Filesystem enumeration failed", Status);
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    FileSystem = NULL;
    Root = NULL;
    File = NULL;
    BootPath = NULL;
    BootImage = NULL;

    Status = gBS->OpenProtocol(
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );

    if (EFI_ERROR(Status)) {
      continue;
    }

    Status = FileSystem->OpenVolume(FileSystem, &Root);

    gBS->CloseProtocol(
           Handles[Index],
           &gEfiSimpleFileSystemProtocolGuid,
           ImageHandle,
           NULL
           );

    if (EFI_ERROR(Status)) {
      continue;
    }

    Status = Root->Open(
                     Root,
                     &File,
                     WINDOWS_BOOT_PATH,
                     EFI_FILE_MODE_READ,
                     0
                     );

    if (EFI_ERROR(Status)) {
      Root->Close(Root);
      continue;
    }

    File->Close(File);
    Root->Close(Root);

    Bar1LogEvent(L"Microsoft boot manager found on an exposed filesystem");

    BootPath = FileDevicePath(
                 Handles[Index],
                 WINDOWS_BOOT_PATH
                 );

    if (BootPath == NULL) {
      Bar1LogEvent(L"FileDevicePath allocation failed");
      continue;
    }

    Status = gBS->LoadImage(
                    FALSE,
                    ImageHandle,
                    BootPath,
                    NULL,
                    0,
                    &BootImage
                    );

    FreePool(BootPath);

    if (EFI_ERROR(Status)) {
      Bar1LogStatus(L"Windows Boot Manager LoadImage failed", Status);
      continue;
    }

    Bar1LogEvent(L"Windows Boot Manager loaded; calling StartImage");

    Status = gBS->StartImage(
                    BootImage,
                    NULL,
                    NULL
                    );

    Bar1LogStatus(L"Windows Boot Manager StartImage returned", Status);

    if (!EFI_ERROR(Status)) {
      FreePool(Handles);
      return EFI_SUCCESS;
    }

    gBS->UnloadImage(BootImage);
  }

  FreePool(Handles);

  Bar1LogEvent(L"Windows Boot Manager was not successfully launched");

  return EFI_NOT_FOUND;
}
