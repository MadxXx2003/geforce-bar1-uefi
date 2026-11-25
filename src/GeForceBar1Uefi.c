/** @file
  Read-only PCI inventory for the GeForce BAR1 UEFI project.

  This first-stage utility deliberately performs NO PCI configuration writes,
  does not remove/hide functions, does not resize BARs, and does not chainload.
**/
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#define NVIDIA_VENDOR_ID  0x10DE
#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_BRIDGE  0x06
#define PCI_CLASS_USB     0x0C
#define PCI_EXT_CAP_START 0x100
#define PCI_EXT_CAP_END   0x1000

STATIC
EFI_STATUS
ReadConfig32 (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINT32 Offset,
  OUT UINT32 *Value
  )
{
  *Value = 0xFFFFFFFF;
  return PciIo->Pci.Read (
                    PciIo,
                    EfiPciIoWidthUint32,
                    Offset,
                    1,
                    Value
                    );
}

STATIC
VOID
PrintExtendedCapabilities (
  IN EFI_PCI_IO_PROTOCOL *PciIo
  )
{
  UINT32 Offset;
  UINT32 Header;
  UINT32 Next;
  UINTN Count;
  EFI_STATUS Status;

  Print(L"    Extended capabilities:\n");
  Offset = PCI_EXT_CAP_START;
  Count = 0;

  while (Offset >= PCI_EXT_CAP_START &&
         Offset < PCI_EXT_CAP_END &&
         Count < 256) {
    Status = ReadConfig32(PciIo, Offset, &Header);
    if (EFI_ERROR(Status) || Header == 0 || Header == 0xFFFFFFFF) {
      break;
    }

    Next = (Header >> 20) & 0xFFF;
    Print(
      L"      @%03x: ID=%04x version=%x next=%03x\n",
      Offset,
      Header & 0xFFFF,
      (Header >> 16) & 0xF,
      Next
      );

    Count++;
    if (Next == 0 || Next == Offset || Next < PCI_EXT_CAP_START ||
        (Next & 3) != 0) {
      break;
    }
    Offset = Next;
  }

  if (Count == 0) {
    Print(L"      unavailable or no extended capabilities exposed by firmware\n");
  }
}

STATIC
VOID
PrintBars (
  IN EFI_PCI_IO_PROTOCOL *PciIo
  )
{
  UINTN Bar;
  UINT32 Low;
  UINT32 High;
  EFI_STATUS Status;

  Print(L"    BAR registers (raw config values; not decoded sizes):\n");
  for (Bar = 0; Bar < 6; Bar++) {
    Status = ReadConfig32(PciIo, (UINT32)(0x10 + (Bar * 4)), &Low);
    if (EFI_ERROR(Status)) {
      Print(L"      BAR%u: read failed: %r\n", Bar, Status);
      continue;
    }

    if (Low == 0) {
      Print(L"      BAR%u: 00000000 (unassigned/unused)\n", Bar);
      continue;
    }

    if ((Low & 1) != 0) {
      Print(L"      BAR%u: I/O raw=%08x\n", Bar, Low);
      continue;
    }

    if (((Low >> 1) & 3) == 2 && Bar < 5) {
      Status = ReadConfig32(PciIo, (UINT32)(0x14 + (Bar * 4)), &High);
      if (!EFI_ERROR(Status)) {
        Print(L"      BAR%u: MEM64 raw=%08x:%08x prefetch=%u\n",
              Bar, High, Low, (Low >> 3) & 1);
      } else {
        Print(L"      BAR%u: MEM64 low=%08x high-read-failed\n", Bar, Low);
      }
      Bar++;
    } else {
      Print(L"      BAR%u: MEM32 raw=%08x prefetch=%u\n",
            Bar, Low, (Low >> 3) & 1);
    }
  }
}

STATIC
VOID
PrintBridgeWindows (
  IN EFI_PCI_IO_PROTOCOL *PciIo
  )
{
  UINT32 BusNumbers;
  UINT32 IoWindow;
  UINT32 MemWindow;
  UINT32 PrefWindow;
  UINT32 PrefBaseUpper;
  UINT32 PrefLimitUpper;
  EFI_STATUS S0, S1, S2, S3, S4, S5;

  S0 = ReadConfig32(PciIo, 0x18, &BusNumbers);
  S1 = ReadConfig32(PciIo, 0x1C, &IoWindow);
  S2 = ReadConfig32(PciIo, 0x20, &MemWindow);
  S3 = ReadConfig32(PciIo, 0x24, &PrefWindow);
  S4 = ReadConfig32(PciIo, 0x28, &PrefBaseUpper);
  S5 = ReadConfig32(PciIo, 0x2C, &PrefLimitUpper);

  Print(L"    PCI bridge windows (raw config registers):\n");
  if (!EFI_ERROR(S0)) Print(L"      bus numbers 0x18: %08x\n", BusNumbers);
  if (!EFI_ERROR(S1)) Print(L"      I/O window  0x1c: %08x\n", IoWindow);
  if (!EFI_ERROR(S2)) Print(L"      MEM window  0x20: %08x\n", MemWindow);
  if (!EFI_ERROR(S3)) Print(L"      PREF window 0x24: %08x\n", PrefWindow);
  if (!EFI_ERROR(S4)) Print(L"      PREF base upper  0x28: %08x\n", PrefBaseUpper);
  if (!EFI_ERROR(S5)) Print(L"      PREF limit upper 0x2c: %08x\n", PrefLimitUpper);
  Print(L"      (raw values only; no bridge-window writes)\n");
}

STATIC
VOID
PrintNvidiaVsec (
  IN EFI_PCI_IO_PROTOCOL *PciIo
  )
{
  UINT32 VsecHeader;
  UINT32 VsecBody;
  UINT32 OverrideSupport;
  UINT32 OverrideValue;
  EFI_STATUS S0, S1, S2, S3;

  S0 = ReadConfig32(PciIo, 0x600, &VsecHeader);
  S1 = ReadConfig32(PciIo, 0x604, &VsecBody);
  S2 = ReadConfig32(PciIo, 0x60C, &OverrideSupport);
  S3 = ReadConfig32(PciIo, 0x610, &OverrideValue);

  Print(L"    NVIDIA VSEC probe (read-only):\n");
  if (EFI_ERROR(S0)) {
    Print(L"      0x600: unavailable: %r\n", S0);
  } else {
    Print(L"      0x600: %08x%s\n", VsecHeader,
          ((VsecHeader & 0xFFFF) == 0x000B) ? L" (VSEC ID 000b)" : L"");
  }
  if (EFI_ERROR(S1)) {
    Print(L"      0x604: unavailable: %r\n", S1);
  } else {
    Print(L"      0x604: %08x\n", VsecBody);
  }
  if (EFI_ERROR(S2)) {
    Print(L"      0x60c: unavailable: %r\n", S2);
  } else {
    Print(L"      0x60c: %08x\n", OverrideSupport);
  }
  if (EFI_ERROR(S3)) {
    Print(L"      0x610: unavailable: %r\n", S3);
  } else {
    Print(L"      0x610: %08x (NOT MODIFIED)\n", OverrideValue);
  }
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN HandleCount;
  UINTN Index;
  EFI_PCI_IO_PROTOCOL *PciIo;
  UINTN Segment, Bus, Device, Function;
  UINT32 IdReg, ClassReg, HeaderReg;
  UINT16 Vendor, DeviceId;
  UINT8 BaseClass, SubClass;
  BOOLEAN IsNvidiaDisplay;

  (VOID)SystemTable;
  Handles = NULL;
  HandleCount = 0;

  Print(L"\n=== GeForce BAR1 UEFI - READ-ONLY PCI SCAN ===\n");
  Print(L"Firmware-side inventory only. No PCI writes or device removal.\n\n");

  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR(Status)) {
    Print(L"LocateHandleBuffer(PciIo) failed: %r\n", Status);
    return Status;
  }

  Print(L"PCI I/O handles: %u\n\n", HandleCount);

  for (Index = 0; Index < HandleCount; Index++) {
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
    if (EFI_ERROR(Status)) {
      continue;
    }

    if (EFI_ERROR(ReadConfig32(PciIo, 0x00, &IdReg)) ||
        EFI_ERROR(ReadConfig32(PciIo, 0x08, &ClassReg)) ||
        EFI_ERROR(ReadConfig32(PciIo, 0x0C, &HeaderReg))) {
      continue;
    }

    Vendor = (UINT16)(IdReg & 0xFFFF);
    DeviceId = (UINT16)(IdReg >> 16);
    BaseClass = (UINT8)(ClassReg >> 24);
    SubClass = (UINT8)(ClassReg >> 16);

    IsNvidiaDisplay = (Vendor == NVIDIA_VENDOR_ID) &&
                      (BaseClass == PCI_CLASS_DISPLAY) &&
                      ((SubClass == 0x00) || (SubClass == 0x02));

    if (Vendor != NVIDIA_VENDOR_ID &&
        !(Segment == 0 && Bus == 0 && Device == 1 && Function == 0)) {
      continue;
    }

    Print(
      L"%04x:%02x:%02x.%x  %04x:%04x  class=%02x:%02x  hdr=%02x\n",
      Segment, Bus, Device, Function,
      Vendor, DeviceId, BaseClass, SubClass,
      (UINT8)((HeaderReg >> 16) & 0x7F)
      );

    if (IsNvidiaDisplay) {
      Print(L"    NVIDIA display controller candidate\n");
      PrintBars(PciIo);
      PrintExtendedCapabilities(PciIo);
      PrintNvidiaVsec(PciIo);
    } else if (Vendor == NVIDIA_VENDOR_ID) {
      Print(L"    NVIDIA companion/other function\n");
      PrintBars(PciIo);
    } else {
      Print(L"    Intel root-port candidate\n");
      PrintBars(PciIo);
    }

    Print(L"\n");
  }

  if (Handles != NULL) {
    FreePool(Handles);
  }

  Print(L"=== END SCAN ===\n");
  Print(L"IMPORTANT: this build is read-only; no BAR resize is attempted.\n");
  return EFI_SUCCESS;
}
