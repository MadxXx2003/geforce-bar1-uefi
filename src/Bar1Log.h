// SPDX-License-Identifier: MIT
#ifndef BAR1_LOG_H
#define BAR1_LOG_H

#include <Uefi.h>

EFI_STATUS
EFIAPI
Bar1LogInitialize(IN EFI_HANDLE ImageHandle);

VOID
EFIAPI
Bar1LogEvent(IN CONST CHAR16 *Message);

VOID
EFIAPI
Bar1LogStatus(
  IN CONST CHAR16 *Operation,
  IN EFI_STATUS Status
  );

VOID
EFIAPI
Bar1LogPciWrite(
  IN UINT32 Offset,
  IN UINT32 Value,
  IN UINTN Width,
  IN BOOLEAN Before,
  IN EFI_STATUS Status
  );

VOID
EFIAPI
Bar1LogClose(VOID);

#endif
