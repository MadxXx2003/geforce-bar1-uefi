#include <Uefi.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>

#include "Bar1Log.h"

STATIC EFI_FILE_PROTOCOL *mLogFile = NULL;
STATIC UINT64 mSequence = 0;

STATIC
EFI_STATUS
WriteRecord(IN CONST CHAR16 *Message)
{
  EFI_STATUS Status;
  CHAR16 Line[512];
  UINTN Bytes;
  UINTN Written;

  if (mLogFile == NULL || Message == NULL) {
    return EFI_NOT_READY;
  }

  UnicodeSPrint(
    Line,
    sizeof(Line),
    L"[%06Lu] %s\r\n",
    ++mSequence,
    Message
    );

  Bytes = StrLen(Line) * sizeof(CHAR16);
  Written = Bytes;

  Status = mLogFile->Write(mLogFile, &Written, Line);

  if (EFI_ERROR(Status)) {
    return Status;
  }

  if (Written != Bytes) {
    return EFI_DEVICE_ERROR;
  }

  return mLogFile->Flush(mLogFile);
}

EFI_STATUS
EFIAPI
Bar1LogInitialize(IN EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_FILE_PROTOCOL *Root;

  LoadedImage = NULL;
  FileSystem = NULL;
  Root = NULL;

  Status = gBS->OpenProtocol(
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR(Status) || LoadedImage == NULL) {
    return EFI_ERROR(Status) ? Status : EFI_NOT_FOUND;
  }

  if (LoadedImage->DeviceHandle == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->OpenProtocol(
                  LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = FileSystem->OpenVolume(FileSystem, &Root);

  gBS->CloseProtocol(
         LoadedImage->DeviceHandle,
         &gEfiSimpleFileSystemProtocolGuid,
         ImageHandle,
         NULL
         );

  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = Root->Open(
                   Root,
                   &mLogFile,
                   L"bar1-log.txt",
                   EFI_FILE_MODE_READ |
                   EFI_FILE_MODE_WRITE |
                   EFI_FILE_MODE_CREATE,
                   0
                   );

  Root->Close(Root);

  if (EFI_ERROR(Status)) {
    mLogFile = NULL;
    return Status;
  }

  /* Append rather than overwrite previous crash logs. */
  Status = mLogFile->SetPosition(mLogFile, MAX_UINT64);

  if (EFI_ERROR(Status)) {
    mLogFile->Close(mLogFile);
    mLogFile = NULL;
    return Status;
  }

  WriteRecord(L"");
  WriteRecord(L"========== NEW EFI APPLICATION SESSION ==========");
  WriteRecord(L"Logger initialized on application filesystem");

  return EFI_SUCCESS;
}

VOID
EFIAPI
Bar1LogEvent(IN CONST CHAR16 *Message)
{
  WriteRecord(Message);
}

VOID
EFIAPI
Bar1LogStatus(
  IN CONST CHAR16 *Operation,
  IN EFI_STATUS Status
  )
{
  CHAR16 Line[256];

  UnicodeSPrint(
    Line,
    sizeof(Line),
    L"%s: %r",
    Operation,
    Status
    );

  WriteRecord(Line);
}

VOID
EFIAPI
Bar1LogPciWrite(
  IN UINT32 Offset,
  IN UINT32 Value,
  IN UINTN Width,
  IN BOOLEAN Before,
  IN EFI_STATUS Status
  )
{
  CHAR16 Line[256];

  if (Before) {
    UnicodeSPrint(
      Line,
      sizeof(Line),
      L"PCI WRITE ENTER width=%u offset=0x%03x value=0x%08x",
      Width,
      Offset,
      Value
      );
  } else {
    UnicodeSPrint(
      Line,
      sizeof(Line),
      L"PCI WRITE RETURN width=%u offset=0x%03x status=%r",
      Width,
      Offset,
      Status
      );
  }

  WriteRecord(Line);
}

VOID
EFIAPI
Bar1LogClose(VOID)
{
  if (mLogFile != NULL) {
    WriteRecord(L"Logger closing");
    mLogFile->Close(mLogFile);
    mLogFile = NULL;
  }
}
