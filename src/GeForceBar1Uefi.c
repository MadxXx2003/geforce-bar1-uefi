
/** @file
  Read-only PCI inventory and BAR1 preflight for the GeForce BAR1 UEFI project.

  This utility deliberately performs NO PCI configuration writes.
  It does not remove/hide functions, resize BARs, or chainload.

  Stage 1:
    - PCI Type 0 / Type 1 header-aware BAR enumeration
    - PCI bridge-window decoding
    - PCIe extended-capability traversal
    - Resizable BAR entry enumeration
    - NVIDIA VSEC structural validation
    - Read-only resource-layout diagnostics
**/

#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#define NVIDIA_VENDOR_ID          0x10DE

#define PCI_CLASS_DISPLAY         0x03
#define PCI_CLASS_BRIDGE          0x06
#define PCI_CLASS_USB             0x0C

#define PCI_HEADER_TYPE_ENDPOINT  0
#define PCI_HEADER_TYPE_BRIDGE    1

#define PCI_EXT_CAP_START         0x100
#define PCI_EXT_CAP_END           0x1000
#define PCI_EXT_CAP_MAX_COUNT     256

#define PCI_EXT_CAP_ID_VSEC       0x000B
#define PCI_EXT_CAP_ID_REBAR      0x0015

#define REBAR_BAR1_INDEX          1
#define REBAR_SIZE_SHIFT          8
#define REBAR_SIZE_MASK           0x1F

#define NVIDIA_VSEC_OFFSET        0x600
#define NVIDIA_VSEC_BODY_OFFSET   0x604
#define NVIDIA_VSEC_SUPPORT_OFF   0x60C
#define NVIDIA_VSEC_OVERRIDE_OFF  0x610

#define NVIDIA_REBAR_OFFSET       0xBB0
#define NVIDIA_BAR1_CAP_OFFSET    0xBBC

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

STATIC
EFI_STATUS
ReadConfig32 (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINT32 Offset,
  OUT UINT32 *Value
  )
{
  if (PciIo == NULL || Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Value = 0xFFFFFFFF;

  return PciIo->Pci.Read (
                    PciIo,
                    EfiPciIoWidthUint32,
                    Offset,
                    1,
                    Value
                    );
}

/**
  Find a PCIe extended capability by ID.

  The capability chain is followed using the next-pointer field.
  Invalid, unaligned, backward, out-of-range and cyclic-looking
  pointers terminate traversal.

  This function only reads configuration space.
**/
STATIC
EFI_STATUS
FindExtendedCapability (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINT16 CapabilityId,
  OUT UINT32 *CapabilityOffset
  )
{
  UINT32 Offset;
  UINT32 Header;
  UINT32 Next;
  UINTN Count;
  EFI_STATUS Status;

  if (CapabilityOffset == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *CapabilityOffset = 0;
  Offset = PCI_EXT_CAP_START;

  for (Count = 0; Count < PCI_EXT_CAP_MAX_COUNT; Count++) {
    Status = ReadConfig32(PciIo, Offset, &Header);

    if (EFI_ERROR(Status)) {
      return Status;
    }

    if (Header == 0 || Header == 0xFFFFFFFF) {
      return EFI_NOT_FOUND;
    }

    if ((Header & 0xFFFF) == CapabilityId) {
      *CapabilityOffset = Offset;
      return EFI_SUCCESS;
    }

    Next = (Header >> 20) & 0xFFF;

    if (Next == 0) {
      return EFI_NOT_FOUND;
    }

    if (Next < PCI_EXT_CAP_START ||
        Next >= PCI_EXT_CAP_END ||
        Next <= Offset ||
        (Next & 3) != 0) {
      return EFI_COMPROMISED_DATA;
    }

    Offset = Next;
  }

  return EFI_COMPROMISED_DATA;
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
         Count < PCI_EXT_CAP_MAX_COUNT) {

    Status = ReadConfig32(PciIo, Offset, &Header);

    if (EFI_ERROR(Status) ||
        Header == 0 ||
        Header == 0xFFFFFFFF) {
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

    if (Next == 0) {
      break;
    }

    if (Next < PCI_EXT_CAP_START ||
        Next >= PCI_EXT_CAP_END ||
        Next <= Offset ||
        (Next & 3) != 0) {
      Print(L"      Invalid extended-capability next pointer\n");
      break;
    }

    Offset = Next;
  }

  if (Count == 0) {
    Print(L"      unavailable or no extended capabilities exposed\n");
  }
}

/**
  Print BAR registers according to the PCI header type.

  Type 0 endpoint: six BAR slots.
  Type 1 bridge:   two BAR slots.

  The values are raw BAR register contents, not decoded sizes.
**/
STATIC
VOID
PrintBars (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINTN BarCount
  )
{
  UINTN Bar;
  UINT32 Low;
  UINT32 High;
  EFI_STATUS Status;

  Print(L"    BAR registers (raw config values; not decoded sizes):\n");

  for (Bar = 0; Bar < BarCount && Bar < 6; Bar++) {
    Status = ReadConfig32(
               PciIo,
               (UINT32)(0x10 + (Bar * 4)),
               &Low
               );

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

    if (((Low >> 1) & 3) == 2) {
      if (Bar + 1 >= BarCount) {
        Print(L"      BAR%u: malformed MEM64 BAR at final slot\n", Bar);
        continue;
      }

      Status = ReadConfig32(
                 PciIo,
                 (UINT32)(0x14 + (Bar * 4)),
                 &High
                 );

      if (!EFI_ERROR(Status)) {
        Print(
          L"      BAR%u: MEM64 raw=%08x:%08x prefetch=%u\n",
          Bar,
          High,
          Low,
          (Low >> 3) & 1
          );
      } else {
        Print(L"      BAR%u: MEM64 high-read-failed: %r\n", Bar, Status);
      }

      Bar++;
    } else {
      Print(
        L"      BAR%u: MEM32 raw=%08x prefetch=%u\n",
        Bar,
        Low,
        (Low >> 3) & 1
        );
    }
  }
}

/**
  Print all Resizable BAR entries and identify BAR1.

  ReBAR capability offset is discovered dynamically.
  BAR1 capability/control offsets are returned to the caller.
**/
STATIC
VOID
PrintRebar (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINT32 RebarOffset,
  OUT UINT32 *Bar1CapabilityOffset
  )
{
  UINT32 Header;
  UINT32 FirstControl;
  UINT32 Capability;
  UINT32 Control;
  UINT32 Count;
  UINT32 Index;
  UINT32 BarIndex;
  UINT32 SizeIndex;
  UINT32 Offset;
  UINT32 EntryCount;
  UINT64 SizeMiB;
  BOOLEAN Found;
  EFI_STATUS Status;

  if (Bar1CapabilityOffset == NULL) {
    return;
  }

  *Bar1CapabilityOffset = 0;

  Print(L"    Resizable BAR analysis:\n");

  Status = ReadConfig32(PciIo, RebarOffset, &Header);

  if (EFI_ERROR(Status) ||
      (Header & 0xFFFF) != PCI_EXT_CAP_ID_REBAR) {
    Print(L"      Invalid ReBAR capability at %03x\n", RebarOffset);
    return;
  }

  Print(L"      Capability header: %08x\n", Header);

  Status = ReadConfig32(PciIo, RebarOffset + 8, &FirstControl);

  if (EFI_ERROR(Status)) {
    Print(L"      Cannot read ReBAR entry count: %r\n", Status);
    return;
  }

  EntryCount = (FirstControl >> 5) & 7;

  Print(L"      Number of ReBAR entries: %u\n", EntryCount);

  if (EntryCount == 0) {
    Print(L"      No ReBAR entries reported\n");
    return;
  }

  Found = FALSE;

  for (Index = 0; Index < EntryCount; Index++) {
    Offset = RebarOffset + 4 + (Index * 8);

    Status = ReadConfig32(PciIo, Offset, &Capability);

    if (EFI_ERROR(Status)) {
      Print(L"      Entry %u capability read failed: %r\n", Index, Status);
      continue;
    }

    Status = ReadConfig32(PciIo, Offset + 4, &Control);

    if (EFI_ERROR(Status)) {
      Print(L"      Entry %u control read failed: %r\n", Index, Status);
      continue;
    }

    BarIndex = Control & 7;

    Print(
      L"      Entry %u: capability=%03x control=%03x BAR=%u\n",
      Index,
      Offset,
      Offset + 4,
      BarIndex
      );

    if (BarIndex != REBAR_BAR1_INDEX) {
      continue;
    }

    if (Found) {
      Print(L"      WARNING: duplicate BAR1 ReBAR entry\n");
      continue;
    }

    Found = TRUE;
    *Bar1CapabilityOffset = Offset;

    SizeIndex = (Control >> REBAR_SIZE_SHIFT) & REBAR_SIZE_MASK;

    Print(L"      BAR1 supported sizes (standard capability bitmap):\n");

    for (Count = 4; Count < 32; Count++) {
      if ((Capability & ((UINT32)1 << Count)) != 0) {
        SizeMiB = (UINT64)1 << (Count - 4);

        Print(
          L"        %Lu MiB%s\n",
          SizeMiB,
          (Count == SizeIndex + 4) ? L" (selected)" : L""
          );
      }
    }

    SizeMiB = (UINT64)1 << SizeIndex;

    Print(L"      Current standard ReBAR size: %Lu MiB\n", SizeMiB);
    Print(L"      Raw capability: %08x\n", Capability);
    Print(L"      Raw control:    %08x\n", Control);
  }

  if (!Found) {
    Print(L"      No BAR1 entry found\n");
  }

  Print(L"      READ ONLY: no ReBAR registers modified\n");
}

/**
  Decode the Type 1 PCI-to-PCI bridge windows.

  Decodes:
    I/O window, including upper 16-bit registers
    Non-prefetchable memory window
    Prefetchable memory window, including upper 32-bit registers

  This reports configured address windows. It does NOT establish
  that the reported address space is free or can be reassigned.
**/
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
  UINT32 IoUpper;
  UINT32 IoBase;
  UINT32 IoLimit;
  UINT32 MemBase;
  UINT32 MemLimit;
  UINT32 PrefBaseLow;
  UINT32 PrefLimitLow;
  UINT64 IoBase64;
  UINT64 IoLimit64;
  UINT64 PrefBase64;
  UINT64 PrefLimit64;
  EFI_STATUS S0, S1, S2, S3, S4, S5, S6;

  S0 = ReadConfig32(PciIo, 0x18, &BusNumbers);
  S1 = ReadConfig32(PciIo, 0x1C, &IoWindow);
  S2 = ReadConfig32(PciIo, 0x20, &MemWindow);
  S3 = ReadConfig32(PciIo, 0x24, &PrefWindow);
  S4 = ReadConfig32(PciIo, 0x28, &PrefBaseUpper);
  S5 = ReadConfig32(PciIo, 0x2C, &PrefLimitUpper);
  S6 = ReadConfig32(PciIo, 0x30, &IoUpper);

  Print(L"    PCI bridge windows:\n");

  if (!EFI_ERROR(S0)) {
    Print(
      L"      Bus numbers: primary=%u secondary=%u subordinate=%u\n",
      BusNumbers & 0xFF,
      (BusNumbers >> 8) & 0xFF,
      (BusNumbers >> 16) & 0xFF
      );
  } else {
    Print(L"      Bus numbers: read failed: %r\n", S0);
  }

  //
  // I/O window.
  //
  if (!EFI_ERROR(S1) && !EFI_ERROR(S6)) {
    IoBase = (IoWindow >> 8) & 0xF0;
    IoLimit = (IoWindow >> 8) & 0xF000;

    IoBase64 = IoBase;
    IoLimit64 = IoLimit | 0xFFF;

    if ((IoWindow & 0x0F) == 1) {
      IoBase64 |= ((UINT64)(IoUpper & 0xFFFF) << 16);
      IoLimit64 |= ((UINT64)(IoUpper >> 16) << 16);
    }

    Print(
      L"      I/O window: base=%08x%08x limit=%08x%08x type=%s\n",
      (UINT32)(IoBase64 >> 32),
      (UINT32)IoBase64,
      (UINT32)(IoLimit64 >> 32),
      (UINT32)IoLimit64,
      ((IoWindow & 0x0F) == 1) ? L"32-bit" : L"16-bit"
      );

    if (IoBase64 > IoLimit64) {
      Print(L"        State: disabled/invalid window\n");
    }
  } else {
    Print(L"      I/O window: read failed\n");
  }

  //
  // Non-prefetchable 32-bit memory window.
  //
  if (!EFI_ERROR(S2)) {
    MemBase = (MemWindow & 0x0000FFF0) << 16;
    MemLimit = (MemWindow & 0xFFF00000) | 0x000FFFFF;

    Print(
      L"      MEM window: base=%08x limit=%08x\n",
      MemBase,
      MemLimit
      );

    if (MemBase > MemLimit) {
      Print(L"        State: disabled/invalid window\n");
    }
  } else {
    Print(L"      MEM window: read failed: %r\n", S2);
  }

  //
  // Prefetchable memory window.
  //
  if (!EFI_ERROR(S3) &&
      !EFI_ERROR(S4) &&
      !EFI_ERROR(S5)) {

    PrefBaseLow = (PrefWindow & 0x0000FFF0) << 16;
    PrefLimitLow = (PrefWindow & 0xFFF00000) | 0x000FFFFF;

    PrefBase64 = PrefBaseLow;
    PrefLimit64 = PrefLimitLow;

    if ((PrefWindow & 0x0F) == 1) {
      PrefBase64 |= ((UINT64)PrefBaseUpper << 32);
      PrefLimit64 |= ((UINT64)PrefLimitUpper << 32);
    }

    Print(
      L"      PREF window: base=%08x%08x limit=%08x%08x type=%s\n",
      (UINT32)(PrefBase64 >> 32),
      (UINT32)PrefBase64,
      (UINT32)(PrefLimit64 >> 32),
      (UINT32)PrefLimit64,
      ((PrefWindow & 0x0F) == 1) ? L"64-bit" : L"32-bit"
      );

    if (PrefBase64 > PrefLimit64) {
      Print(L"        State: disabled/invalid window\n");
    }
  } else {
    Print(L"      PREF window: read failed\n");
  }

  Print(L"      (decoded programmed windows; not free-space measurements)\n");
}

/**
  Validate the NVIDIA VSEC structure without modifying it.

  Structural checks:
    S1: NVIDIA VSEC capability at 0x600, ID 0x000B, version 1.
    S2: VSEC body ID/revision/length.
    S3: framebuffer-size override support and reserved bits.
    S4: override register reserved bits and stock/armed state.
    S5: ReBAR capability at 0xBB0 and BAR1 entry at 0xBBC.
    S6: PCI Type 0 endpoint header.

  Passing these checks is NOT authorization to write configuration space.
**/
STATIC
VOID
PrintNvidiaVsec (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINT8 HeaderType
  )
{
  UINT32 VsecOffset;
  UINT32 RebarOffset;
  UINT32 Bar1CapabilityOffset;
  UINT32 VsecHeader;
  UINT32 VsecBody;
  UINT32 OverrideSupport;
  UINT32 OverrideValue;
  UINT32 RebarHeader;
  EFI_STATUS Status;
  BOOLEAN S1, S2, S3, S4, S5, S6;

  VsecOffset = 0;
  RebarOffset = 0;
  Bar1CapabilityOffset = 0;

  VsecHeader = 0;
  VsecBody = 0;
  OverrideSupport = 0;
  OverrideValue = 0;
  RebarHeader = 0;

  S1 = FALSE;
  S2 = FALSE;
  S3 = FALSE;
  S4 = FALSE;
  S5 = FALSE;
  S6 = (HeaderType == PCI_HEADER_TYPE_ENDPOINT);

  Print(L"    NVIDIA VSEC validation (READ ONLY):\n");

  Status = FindExtendedCapability(
             PciIo,
             PCI_EXT_CAP_ID_VSEC,
             &VsecOffset
             );

  if (!EFI_ERROR(Status)) {
    Status = ReadConfig32(PciIo, VsecOffset, &VsecHeader);

    S1 = !EFI_ERROR(Status) &&
         VsecOffset == NVIDIA_VSEC_OFFSET &&
         (VsecHeader & 0xFFFF) == PCI_EXT_CAP_ID_VSEC &&
         ((VsecHeader >> 16) & 0xF) == 1;

    if (!EFI_ERROR(ReadConfig32(
                     PciIo,
                     VsecOffset + 4,
                     &VsecBody
                     ))) {

      S2 = (VsecBody & 0xFFFF) == 0x0001 &&
           ((VsecBody >> 16) & 0xF) == 1 &&
           ((VsecBody >> 20) & 0xFFF) == 0x024;
    }

    if (!EFI_ERROR(ReadConfig32(
                     PciIo,
                     VsecOffset + 0x0C,
                     &OverrideSupport
                     ))) {

      S3 = ((OverrideSupport & (1U << 11)) != 0) &&
           ((OverrideSupport & 0xFFFFE7F8U) == 0);
    }

    if (!EFI_ERROR(ReadConfig32(
                     PciIo,
                     VsecOffset + 0x10,
                     &OverrideValue
                     ))) {

      S4 = ((OverrideValue & 0xFFFFE07CU) == 0) &&
           (((OverrideValue & 0x00000F80U) == 0) ||
            ((OverrideValue & 0x00000800U) != 0));
    }

    Print(L"      VSEC offset: %03x\n", VsecOffset);
    Print(L"      0x%03x: %08x\n", VsecOffset, VsecHeader);
    Print(L"      0x%03x: %08x\n", VsecOffset + 4, VsecBody);
    Print(L"      0x%03x: %08x\n", VsecOffset + 0x0C, OverrideSupport);
    Print(L"      0x%03x: %08x (NOT MODIFIED)\n",
          VsecOffset + 0x10, OverrideValue);
  } else {
    Print(L"      NVIDIA VSEC not found: %r\n", Status);
  }

  Status = FindExtendedCapability(
             PciIo,
             PCI_EXT_CAP_ID_REBAR,
             &RebarOffset
             );

  if (!EFI_ERROR(Status)) {
    Status = ReadConfig32(PciIo, RebarOffset, &RebarHeader);

    if (!EFI_ERROR(Status) &&
        (RebarHeader & 0xFFFF) == PCI_EXT_CAP_ID_REBAR) {

      PrintRebar(
        PciIo,
        RebarOffset,
        &Bar1CapabilityOffset
        );

      S5 = (RebarOffset == NVIDIA_REBAR_OFFSET &&
            Bar1CapabilityOffset == NVIDIA_BAR1_CAP_OFFSET);
    } else {
      Print(L"      ReBAR header read/validation failed\n");
    }
  } else {
    Print(L"      ReBAR capability not found: %r\n", Status);
  }

  Print(L"\n      Structural checks:\n");

  Print(L"        S1 VSEC location/header:       %s\n",
        S1 ? L"PASS" : L"FAIL");

  Print(L"        S2 VSEC ID/revision/length:    %s\n",
        S2 ? L"PASS" : L"FAIL");

  Print(L"        S3 override support/reserved: %s\n",
        S3 ? L"PASS" : L"FAIL");

  Print(L"        S4 override value/reserved:   %s\n",
        S4 ? L"PASS" : L"FAIL");

  Print(L"        S5 expected BAR1 ReBAR entry: %s\n",
        S5 ? L"PASS" : L"FAIL");

  Print(L"        S6 PCI Type 0 endpoint:       %s\n",
        S6 ? L"PASS" : L"FAIL");

  if (S1 && S2 && S3 && S4 && S5 && S6) {
    Print(L"\n      PREFLIGHT: ALL STRUCTURAL CHECKS PASSED\n");
    Print(L"      This does NOT authorize a configuration write.\n");
  } else {
    Print(L"\n      PREFLIGHT: FAILED / INCOMPLETE\n");
    Print(L"      NVIDIA override route must not be attempted.\n");
  }

  Print(L"      0x610 remains untouched.\n");
}

/**
  UEFI application entry point.
**/
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

  UINTN Segment;
  UINTN Bus;
  UINTN Device;
  UINTN Function;

  UINT32 IdReg;
  UINT32 ClassReg;
  UINT32 HeaderReg;

  UINT16 Vendor;
  UINT16 DeviceId;

  UINT8 BaseClass;
  UINT8 SubClass;
  UINT8 HeaderType;

  UINTN BarCount;

  BOOLEAN IsNvidiaDisplay;
  BOOLEAN IsRootPort;

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

    HeaderType = (UINT8)((HeaderReg >> 16) & 0x7F);

    BarCount = (HeaderType == PCI_HEADER_TYPE_ENDPOINT) ? 6 :
               (HeaderType == PCI_HEADER_TYPE_BRIDGE) ? 2 : 0;

    IsNvidiaDisplay = (Vendor == NVIDIA_VENDOR_ID) &&
                      (BaseClass == PCI_CLASS_DISPLAY) &&
                      ((SubClass == 0x00) || (SubClass == 0x02));

    IsRootPort = (Segment == 0 &&
                  Bus == 0 &&
                  Device == 1 &&
                  Function == 0);

    if (Vendor != NVIDIA_VENDOR_ID && !IsRootPort) {
      continue;
    }

    Print(
      L"%04x:%02x:%02x.%x  %04x:%04x  class=%02x:%02x  hdr=%02x\n",
      Segment,
      Bus,
      Device,
      Function,
      Vendor,
      DeviceId,
      BaseClass,
      SubClass,
      HeaderType
      );

    if (IsNvidiaDisplay) {
      Print(L"    NVIDIA display controller candidate\n");

      PrintBars(PciIo, BarCount);
      PrintExtendedCapabilities(PciIo);
      PrintNvidiaVsec(PciIo, HeaderType);

    } else if (Vendor == NVIDIA_VENDOR_ID) {
      Print(L"    NVIDIA companion/other function\n");

      PrintBars(PciIo, BarCount);

    } else if (IsRootPort) {
      Print(L"    Intel root-port candidate\n");

      if (HeaderType != PCI_HEADER_TYPE_BRIDGE) {
        Print(L"    WARNING: expected Type 1 bridge header\n");
      }

      PrintBars(PciIo, BarCount);

      if (HeaderType == PCI_HEADER_TYPE_BRIDGE) {
        PrintBridgeWindows(PciIo);
      }
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
