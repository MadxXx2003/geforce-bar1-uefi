
/** @file
  Read-only PCI inventory and BAR1 preflight for the GeForce BAR1 UEFI project.

  This utility deliberately performs NO PCI configuration writes.
  It does not remove/hide functions, resize BARs, or chainload.

  Read-only inventory:
    - PCI Type 0 / Type 1 BAR enumeration
    - GetBarAttributes resource descriptors for NVIDIA BARs
    - EFI_PCI_IO_PROTOCOL open-agent ownership inventory
    - Bus-number-based parent-bridge association
    - PCI bridge-window decoding and 8-GiB comparison
    - PCIe extended-capability traversal and ReBAR/VSEC inspection
    - Root-bridge apertures and DXE GCD memory-space descriptors
**/

#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/DriverBinding.h>
#include <IndustryStandard/Acpi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Pi/PiDxeCis.h>
#include <Library/DxeServicesTableLib.h>
#include "GopDiagnostics.h"

#define NVIDIA_VENDOR_ID          0x10DE
#define NVIDIA_TARGET_DEVICE_ID   0x1F36

#define PCI_CLASS_DISPLAY         0x03
#define PCI_CLASS_BRIDGE          0x06
#define PCI_CLASS_USB             0x0C

#define PCI_HEADER_TYPE_ENDPOINT  0
#define PCI_HEADER_TYPE_BRIDGE    1

#define PCI_EXT_CAP_START         0x100
#define PCI_EXT_CAP_END           0x1000

#define PCI_EXT_CAP_DWORD_COUNT \
  ((PCI_EXT_CAP_END - PCI_EXT_CAP_START) / 4)

/*
 * Set to 1 to dump all aligned dwords from 0x100 through 0xFFC.
 * Leave at 0 for the compact targeted dump.
 */
#define DUMP_FULL_EXT_CONFIG 0
//
// Set to 1 only when deliberately testing NVIDIA GOP recovery.
//
#define ENABLE_NVIDIA_GOP_DRIVER_RECONNECT 1

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

#define NVIDIA_OVERRIDE_MASK       0x00000F80U
#define NVIDIA_OVERRIDE_ENABLE     0x00000800U
#define NVIDIA_OVERRIDE_VAL        7U
#define NVIDIA_OVERRIDE_FIELD      \
  (NVIDIA_OVERRIDE_ENABLE | (NVIDIA_OVERRIDE_VAL << 7))

#define NVIDIA_BAR1_TARGET_CAP_BIT (1U << 17)

#define BAR1_TARGET_BYTES         0x0000000200000000ULL /* 8 GiB */

#define BAR1_TARGET_BASE          0x0000004200000000ULL
#define BAR3_TARGET_BASE          0x00000041FE000000ULL
#define BAR3_TARGET_BYTES         0x0000000002000000ULL /* 32 MiB */

#define PREF_WINDOW_TARGET_BASE   BAR3_TARGET_BASE
#define PREF_WINDOW_TARGET_BYTES  0x0000000202000000ULL /* 8 GiB + 32 MiB */
#define PREF_WINDOW_TARGET_END    0x00000043FFFFFFFFULL

/* Implemented in src/Bar1Resize.c; invoked only after the read-only scan. */
EFI_STATUS
EFIAPI
RunBar1ResizeTransaction (
  IN EFI_HANDLE ImageHandle
  );


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

/* Snapshot record used only for read-only PCI topology correlation. */
typedef struct {
  EFI_HANDLE Handle;
  UINTN Segment;
  UINTN Bus;
  UINTN Device;
  UINTN Function;
  UINT16 Vendor;
  UINT16 DeviceId;
  UINT8 BaseClass;
  UINT8 SubClass;
  UINT8 ProgIf;
  UINT8 HeaderType;
  UINT8 SecondaryBus;
  UINT8 SubordinateBus;
  BOOLEAN Valid;
} PCI_DEVICE_RECORD;

/**
  Build a read-only snapshot of PCI handles for topology correlation.
**/
STATIC
VOID
CollectPciRecords (
  IN EFI_HANDLE *Handles,
  IN UINTN HandleCount,
  IN EFI_HANDLE ImageHandle,
  OUT PCI_DEVICE_RECORD *Records
  )
{
  UINTN Index;
  EFI_PCI_IO_PROTOCOL *PciIo;
  EFI_STATUS Status;
  UINT32 IdReg;
  UINT32 ClassReg;
  UINT32 HeaderReg;
  UINT32 BusReg;

  for (Index = 0; Index < HandleCount; Index++) {
    PciIo = NULL;
    Records[Index].Handle = Handles[Index];

    Status = gBS->OpenProtocol (
                    Handles[Index],
                    &gEfiPciIoProtocolGuid,
                    (VOID **)&PciIo,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR(Status) || PciIo == NULL) {
      if (!EFI_ERROR(Status)) {
        gBS->CloseProtocol(
               Handles[Index],
               &gEfiPciIoProtocolGuid,
               ImageHandle,
               NULL
               );
      }
      continue;
    }

    Status = PciIo->GetLocation (
                      PciIo,
                      &Records[Index].Segment,
                      &Records[Index].Bus,
                      &Records[Index].Device,
                      &Records[Index].Function
                      );
    if (EFI_ERROR(Status) ||
        EFI_ERROR(ReadConfig32(PciIo, 0x00, &IdReg)) ||
        EFI_ERROR(ReadConfig32(PciIo, 0x08, &ClassReg)) ||
        EFI_ERROR(ReadConfig32(PciIo, 0x0C, &HeaderReg))) {
      gBS->CloseProtocol(
             Handles[Index],
             &gEfiPciIoProtocolGuid,
             ImageHandle,
             NULL
             );
      continue;
    }

    Records[Index].Vendor = (UINT16)(IdReg & 0xFFFF);
    Records[Index].DeviceId = (UINT16)(IdReg >> 16);
    Records[Index].BaseClass = (UINT8)(ClassReg >> 24);
    Records[Index].SubClass = (UINT8)(ClassReg >> 16);
    Records[Index].ProgIf = (UINT8)(ClassReg >> 8);
    Records[Index].HeaderType = (UINT8)((HeaderReg >> 16) & 0x7F);

    if (Records[Index].HeaderType == PCI_HEADER_TYPE_BRIDGE &&
        !EFI_ERROR(ReadConfig32(PciIo, 0x18, &BusReg))) {
      Records[Index].SecondaryBus = (UINT8)(BusReg >> 8);
      Records[Index].SubordinateBus = (UINT8)(BusReg >> 16);
    }

    if (Records[Index].Vendor != 0xFFFF && Records[Index].Vendor != 0) {
      Records[Index].Valid = TRUE;
    }

    gBS->CloseProtocol(
           Handles[Index],
           &gEfiPciIoProtocolGuid,
           ImageHandle,
           NULL
           );
  }
}

/**
  Find the PCI-to-PCI bridge whose secondary bus is the endpoint's bus.
  This is a bus-number topology association, not a resource-allocation claim.
**/


/**
  Locate the exact target GPU by complete PCI address and identity.
**/
STATIC
PCI_DEVICE_RECORD *
FindTargetGpuRecord (
  IN PCI_DEVICE_RECORD *Records,
  IN UINTN RecordCount
  )
{
  UINTN Index;

  for (Index = 0; Index < RecordCount; Index++) {
    if (Records[Index].Valid &&
        Records[Index].Segment == 0 &&
        Records[Index].Bus == 1 &&
        Records[Index].Device == 0 &&
        Records[Index].Function == 0 &&
        Records[Index].Vendor == NVIDIA_VENDOR_ID &&
        Records[Index].DeviceId == NVIDIA_TARGET_DEVICE_ID &&
        Records[Index].BaseClass == PCI_CLASS_DISPLAY &&
        Records[Index].SubClass == 0x00) {
      return &Records[Index];
    }
  }

  return NULL;
}

/**
  Step 3: strictly audit the target GPU's companion functions.

  This routine is READ ONLY. It does not disable functions or write PCI
  configuration space.
**/
STATIC
VOID
PrintTargetCompanionAudit (
  IN PCI_DEVICE_RECORD *Records,
  IN UINTN RecordCount
  )
{
  PCI_DEVICE_RECORD *Gpu;
  PCI_DEVICE_RECORD *Record;
  UINTN Index;
  BOOLEAN SawAudio;
  BOOLEAN SawXhci;
  BOOLEAN SawUcsi;

  SawAudio = FALSE;
  SawXhci = FALSE;
  SawUcsi = FALSE;

  Print(L"\n=== STEP 3: GPU COMPANION AUDIT ===\n");

  Gpu = FindTargetGpuRecord(Records, RecordCount);

  if (Gpu == NULL) {
    Print(L"ERROR: exact target GPU 0000:01:00.0 10DE:1F36 not found.\n");
    Print(L"No companion functions selected.\n");
    Print(L"=== END STEP 3 ===\n");
    return;
  }

  Print(L"Target GPU: %04x:%02x:%02x.%x %04x:%04x\n",
        Gpu->Segment,
        Gpu->Bus,
        Gpu->Device,
        Gpu->Function,
        Gpu->Vendor,
        Gpu->DeviceId);

  Print(L"Scanning only the exact same segment/bus/device.\n");

  for (Index = 0; Index < RecordCount; Index++) {
    Record = &Records[Index];

    if (!Record->Valid ||
        Record->Segment != Gpu->Segment ||
        Record->Bus != Gpu->Bus ||
        Record->Device != Gpu->Device ||
        Record->Function == Gpu->Function) {
      continue;
    }

    Print(L"\n  Function %04x:%02x:%02x.%x\n",
          Record->Segment,
          Record->Bus,
          Record->Device,
          Record->Function);

    Print(L"    ID:    %04x:%04x\n",
          Record->Vendor,
          Record->DeviceId);

    Print(L"    Class: %02x:%02x:%02x\n",
          Record->BaseClass,
          Record->SubClass,
          Record->ProgIf);

    if (Record->Vendor != NVIDIA_VENDOR_ID) {
      Print(L"    RESULT: non-NVIDIA same-slot function; ignored.\n");
      continue;
    }

    /*
     * HDMI audio function .1 is explicitly excluded.
     */
    if (Record->Function == 1 &&
        Record->BaseClass == 0x04 &&
        Record->SubClass == 0x03) {
      SawAudio = TRUE;
      Print(L"    RESULT: HDMI audio; NEVER a quiesce target.\n");
      continue;
    }

    /*
     * NVIDIA xHCI: function .2, class 0C:03:30.
     */
    if (Record->Function == 2) {
      if (Record->BaseClass == PCI_CLASS_USB &&
          Record->SubClass == 0x03 &&
          Record->ProgIf == 0x30) {
        SawXhci = TRUE;
        Print(L"    RESULT: VERIFIED xHCI companion candidate.\n");
      } else {
        Print(L"    RESULT: function .2 class mismatch; NOT selected.\n");
      }

      continue;
    }

    /*
     * NVIDIA UCSI: function .3, class 0C:80:00.
     */
    if (Record->Function == 3) {
      if (Record->BaseClass == PCI_CLASS_USB &&
          Record->SubClass == 0x80 &&
          Record->ProgIf == 0x00) {
        SawUcsi = TRUE;
        Print(L"    RESULT: VERIFIED UCSI companion candidate.\n");
      } else {
        Print(L"    RESULT: function .3 class mismatch; NOT selected.\n");
      }

      continue;
    }

    Print(L"    RESULT: same-slot NVIDIA function; not selected.\n");
  }

  Print(L"\n=== STEP 3 SUMMARY ===\n");

  Print(L"HDMI audio .1 observed: %s\n",
        SawAudio ? L"YES" : L"NO");

  Print(L"xHCI .2 verified:      %s\n",
        SawXhci ? L"YES" : L"NO");

  Print(L"UCSI .3 verified:       %s\n",
        SawUcsi ? L"YES" : L"NO");

  if (SawXhci && SawUcsi) {
    Print(L"RESULT: BOTH COMPANIONS VERIFIED.\n");
  } else {
    Print(L"RESULT: INCOMPLETE. DO NOT QUIESCE ANY FUNCTION.\n");
  }

  Print(L"PCI configuration writes: NONE.\n");
  Print(L"=== END STEP 3 ===\n");
}

STATIC
PCI_DEVICE_RECORD *
FindParentBridge (
  IN PCI_DEVICE_RECORD *Records,
  IN UINTN RecordCount,
  IN UINTN Segment,
  IN UINTN ChildBus
  )
{
  UINTN Index;

  for (Index = 0; Index < RecordCount; Index++) {
    if (Records[Index].Valid &&
        Records[Index].Segment == Segment &&
        Records[Index].BaseClass == PCI_CLASS_BRIDGE &&
        Records[Index].SubClass == 0x04 &&
        Records[Index].HeaderType == PCI_HEADER_TYPE_BRIDGE &&
        Records[Index].SecondaryBus == ChildBus) {
      return &Records[Index];
    }
  }

  return NULL;
}

STATIC
VOID
PrintParentBridge (
  IN PCI_DEVICE_RECORD *Records,
  IN UINTN RecordCount,
  IN UINTN Segment,
  IN UINTN ChildBus
  )
{
  PCI_DEVICE_RECORD *Parent;

  if (ChildBus == 0) {
    Print(L"    Parent bridge: root complex (no upstream PCI bridge bus)\n");
    return;
  }

  Parent = FindParentBridge(Records, RecordCount, Segment, ChildBus);
  if (Parent == NULL) {
    Print(L"    Parent bridge: not identified from enumerated bridge bus numbers\n");
    return;
  }

  Print(
    L"    Parent bridge: %04x:%02x:%02x.%x  %04x:%04x  secondary=%u subordinate=%u\n",
    Parent->Segment,
    Parent->Bus,
    Parent->Device,
    Parent->Function,
    Parent->Vendor,
    Parent->DeviceId,
    Parent->SecondaryBus,
    Parent->SubordinateBus
    );
}

/**
  Query the PCI bus driver's current resource descriptor for every BAR.
  GetBarAttributes is a query; this routine never calls SetBarAttributes.
**/
STATIC
VOID
PrintBarResources (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINTN BarCount
  )
{
  UINTN Bar;
  UINTN DescriptorIndex;
  UINT64 Supports;
  VOID *Resources;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *Descriptor;
  EFI_STATUS Status;
  CONST CHAR16 *TypeName;
  BOOLEAN FoundDescriptor;

  Print(L"    Firmware PCI BAR resource descriptors (GetBarAttributes, read-only):\n");

  for (Bar = 0; Bar < BarCount && Bar < 6; Bar++) {
    Supports = 0;
    Resources = NULL;

    Status = PciIo->GetBarAttributes(
                      PciIo,
                      (UINT8)Bar,
                      &Supports,
                      &Resources
                      );

    if (Status == EFI_UNSUPPORTED) {
      Print(L"      BAR%u: unsupported/unimplemented\n", Bar);
      continue;
    }

    if (EFI_ERROR(Status)) {
      Print(L"      BAR%u: GetBarAttributes failed: %r\n", Bar, Status);
      continue;
    }

    Print(L"      BAR%u: supported attributes=%016Lx\n", Bar, Supports);

    if (Resources == NULL) {
      Print(L"        no resource descriptor returned\n");
      continue;
    }

    Descriptor = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Resources;
    FoundDescriptor = FALSE;

    /* The UEFI PCI I/O contract returns an ACPI descriptor list + END tag. */
    for (DescriptorIndex = 0; DescriptorIndex < 16; DescriptorIndex++) {
      if (Descriptor->Desc == ACPI_END_TAG_DESCRIPTOR) {
        break;
      }

      if (Descriptor->Desc != ACPI_ADDRESS_SPACE_DESCRIPTOR) {
        Print(L"        unexpected ACPI resource descriptor type=%02x; stopping\n",
              Descriptor->Desc);
        break;
      }

      FoundDescriptor = TRUE;
      if (Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
        TypeName = L"MEM";
      } else if (Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_IO) {
        TypeName = L"I/O";
      } else {
        TypeName = L"OTHER";
      }

      Print(
        L"        %s granularity=%Lu specific=%02x min=%016Lx max=%016Lx translation=%016Lx length=%016Lx\n",
        TypeName,
        Descriptor->AddrSpaceGranularity,
        Descriptor->SpecificFlag,
        Descriptor->AddrRangeMin,
        Descriptor->AddrRangeMax,
        Descriptor->AddrTranslationOffset,
        Descriptor->AddrLen
        );

      if (Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM &&
          (Descriptor->SpecificFlag & 0x06) == 0x06) {
        Print(L"          memory type: prefetchable\n");
      } else if (Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
        Print(L"          memory type: non-prefetchable/other\n");
      }

      Descriptor++;
    }

    if (!FoundDescriptor) {
      Print(L"        no address-space descriptor present\n");
    }

    if (DescriptorIndex == 16) {
      Print(L"        descriptor-list display limit reached\n");
    }

    FreePool(Resources);
  }
}

/**
  Report agents that currently have EFI_PCI_IO_PROTOCOL open on this handle.
  Handles are printed verbatim; this does not disconnect or claim any driver.
**/
STATIC
VOID
PrintProtocolOwners (
  IN EFI_HANDLE ControllerHandle,
  IN EFI_HANDLE ScannerImageHandle
  )
{
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Entries;
  EFI_DRIVER_BINDING_PROTOCOL *DriverBinding;
  UINTN EntryCount;
  UINTN Index;
  EFI_STATUS Status;
  UINT32 Attributes;
  BOOLEAN HasByDriver;

  Entries = NULL;
  EntryCount = 0;
  HasByDriver = FALSE;

  Print(L"    EFI_PCI_IO_PROTOCOL open/ownership inventory:\n");

  Status = gBS->OpenProtocolInformation(
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  &Entries,
                  &EntryCount
                  );

  if (Status == EFI_NOT_FOUND) {
    Print(L"      no protocol-open records reported\n");
    return;
  }

  if (EFI_ERROR(Status)) {
    Print(L"      OpenProtocolInformation failed: %r\n", Status);
    return;
  }

  if (EntryCount == 0) {
    Print(L"      no agents currently recorded\n");
  }

  for (Index = 0; Index < EntryCount; Index++) {
    Attributes = Entries[Index].Attributes;

    Print(
      L"      agent=%p controller=%p opens=%u attrs=%08x",
      Entries[Index].AgentHandle,
      Entries[Index].ControllerHandle,
      Entries[Index].OpenCount,
      Attributes
      );

      if ((Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) != 0) {
      DriverBinding = NULL;

      Status = gBS->HandleProtocol(
                      Entries[Index].AgentHandle,
                      &gEfiDriverBindingProtocolGuid,
                      (VOID **)&DriverBinding
                      );

      if (!EFI_ERROR(Status) && DriverBinding != NULL) {
        Print(
          L"\n        DRIVER BINDING FOUND: version=%08x "
          L"image=%p binding-handle=%p\n",
          DriverBinding->Version,
          DriverBinding->ImageHandle,
          DriverBinding->DriverBindingHandle
          );
      } else {
        Print(
          L"\n        No Driver Binding protocol on agent (%r)\n",
          Status
          );
      }
    }
    
    if ((Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) != 0) {
      HasByDriver = TRUE;
      Print(L" BY_DRIVER");
    }
    if ((Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) != 0) {
      Print(L" BY_CHILD_CONTROLLER");
    }
    if ((Attributes & EFI_OPEN_PROTOCOL_EXCLUSIVE) != 0) {
      Print(L" EXCLUSIVE");
    }
    if ((Attributes & EFI_OPEN_PROTOCOL_GET_PROTOCOL) != 0) {
      Print(L" GET_PROTOCOL");
    }
    if ((Attributes & EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL) != 0) {
      Print(L" BY_HANDLE_PROTOCOL");
    }
    if ((Attributes & EFI_OPEN_PROTOCOL_TEST_PROTOCOL) != 0) {
      Print(L" TEST_PROTOCOL");
    }

    if (Entries[Index].AgentHandle == ScannerImageHandle) {
      Print(L" SCANNER (temporary GET_PROTOCOL reference)");
    }

    Print(L"\n");
  }

  if (HasByDriver) {
    Print(L"      Direct BY_DRIVER ownership: present (see agent handle(s) above)\n");
  } else {
    Print(L"      Direct BY_DRIVER ownership: none reported on this PciIo handle\n");
  }

  if (Entries != NULL) {
    FreePool(Entries);
  }
}

STATIC
VOID
PrintRawDword (
  IN EFI_PCI_IO_PROTOCOL *PciIo,
  IN UINT16 Offset
  )
{
  UINT32 Value;
  EFI_STATUS Status;

  Status = ReadConfig32(PciIo, Offset, &Value);

  if (EFI_ERROR(Status)) {
    Print(L"      CFG[%03x] = <read failed: %r>\n",
          Offset, Status);
  } else {
    Print(L"      CFG[%03x] = %08x\n",
          Offset, Value);
  }
}

STATIC
VOID
PrintRawConfigDump (
  IN EFI_PCI_IO_PROTOCOL *PciIo
  )
{
  STATIC CONST UINT16 Offsets[] = {
    0x100,
    0x128,
    0x250,
    0x258,
    0x420,
    0x600,
    0x604,
    0x60C,
    0x610,
    0x900,
    0xBB0,
    0xBB4,
    0xBB8,
    0xBBC,
    0xBC0,
    0xBC4,
    0xBC8
  };

  UINTN Index;

  Print(L"    Targeted raw PCI configuration dump:\n");

  for (Index = 0;
       Index < sizeof(Offsets) / sizeof(Offsets[0]);
       Index++) {
    PrintRawDword(PciIo, Offsets[Index]);
  }

#if DUMP_FULL_EXT_CONFIG
  {
    UINT32 Base;
    UINT32 Column;
    UINT32 Value;
    EFI_STATUS Status;

    Print(L"\n    FULL extended configuration-space dump:\n");
    Print(L"    Range: 0x100 - 0xFFC, aligned dwords\n");

    for (Base = PCI_EXT_CAP_START;
         Base < PCI_EXT_CAP_END;
         Base += 16) {

      Print(L"      %03x:", Base);

      for (Column = 0; Column < 4; Column++) {
        Status = ReadConfig32(
                   PciIo,
                   Base + (Column * 4),
                   &Value
                   );

        if (EFI_ERROR(Status)) {
          Print(L" ????????");
        } else {
          Print(L" %08x", Value);
        }
      }

      Print(L"\n");
    }
  }
#endif

  Print(L"      (read-only; no configuration writes)\n");
}

/**
  Find a PCIe extended capability by ID.

  Backward next-pointers are permitted. A visited map detects cycles, and
  every access is range- and alignment-checked. This function is read-only.
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
  UINTN Slot;
  UINT8 Visited[PCI_EXT_CAP_DWORD_COUNT];
  EFI_STATUS Status;

  if (PciIo == NULL || CapabilityOffset == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *CapabilityOffset = 0;
  for (Slot = 0; Slot < PCI_EXT_CAP_DWORD_COUNT; Slot++) {
    Visited[Slot] = 0;
  }

  Offset = PCI_EXT_CAP_START;

  for (Count = 0; Count < PCI_EXT_CAP_DWORD_COUNT; Count++) {
    if (Offset < PCI_EXT_CAP_START ||
        Offset >= PCI_EXT_CAP_END ||
        (Offset & 3) != 0) {
      return EFI_COMPROMISED_DATA;
    }

    Slot = (Offset - PCI_EXT_CAP_START) / 4;
    if (Visited[Slot]) {
      return EFI_COMPROMISED_DATA;
    }
    Visited[Slot] = 1;

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

    /* Backward links are legal; the visited table detects cycles. */
    if (Next < PCI_EXT_CAP_START ||
        Next >= PCI_EXT_CAP_END ||
        (Next & 3) != 0) {
      return EFI_COMPROMISED_DATA;
    }

    Slot = (Next - PCI_EXT_CAP_START) / 4;
    if (Visited[Slot]) {
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
  UINTN Slot;
  UINT8 Visited[PCI_EXT_CAP_DWORD_COUNT];
  EFI_STATUS Status;

  for (Slot = 0; Slot < PCI_EXT_CAP_DWORD_COUNT; Slot++) {
    Visited[Slot] = 0;
}

  Print(L"    Extended capabilities:\n");

  Offset = PCI_EXT_CAP_START;
  Count = 0;

  while (Count < PCI_EXT_CAP_DWORD_COUNT) {

    /*
     * Validate the current offset before accessing config space.
     */
    if (Offset < PCI_EXT_CAP_START ||
        Offset >= PCI_EXT_CAP_END ||
        (Offset & 3) != 0) {

      Print(
        L"      Invalid capability offset: %03x\n",
        Offset
        );
      break;
    }

    Slot = (Offset - PCI_EXT_CAP_START) / 4;

    /*
     * Detect loops without rejecting backward links.
     */
    if (Visited[Slot]) {
      Print(
        L"      Capability loop detected at %03x\n",
        Offset
        );
      break;
    }

    Visited[Slot] = 1;

    Status = ReadConfig32(PciIo, Offset, &Header);

    if (EFI_ERROR(Status)) {
      Print(
        L"      @%03x: read failed: %r\n",
        Offset,
        Status
        );
      break;
    }

    if (Header == 0 || Header == 0xFFFFFFFF) {
      Print(
        L"      @%03x: empty/unavailable header %08x\n",
        Offset,
        Header
        );
      break;
    }

    Next = (Header >> 20) & 0xFFF;

    Print(
      L"      @%03x: ID=%04x version=%x next=%03x",
      Offset,
      Header & 0xFFFF,
      (Header >> 16) & 0xF,
      Next
      );

    if (Next != 0 && Next < Offset) {
      Print(L" (backward link)");
    }

    Print(L"\n");

    Count++;

    /*
     * A zero next pointer terminates the linked list normally.
     */
    if (Next == 0) {
      break;
    }

    /*
     * Do not reject Next < Offset.
     *
     * This GPU legitimately reports:
     *     0x258 -> 0x128
     */
    if (Next < PCI_EXT_CAP_START ||
        Next >= PCI_EXT_CAP_END ||
        (Next & 3) != 0) {

      Print(
        L"      Invalid next pointer: %03x\n",
        Next
        );
      break;
    }

    /*
     * Check the destination before following it.
     */
    Slot = (Next - PCI_EXT_CAP_START) / 4;

    if (Visited[Slot]) {
      Print(
        L"      Next pointer %03x would create a loop\n",
        Next
        );
      break;
    }

    Offset = Next;
  }

  if (Count == 0) {
    Print(
      L"      unavailable or no extended capabilities exposed\n"
      );
  }

  if (Count >= PCI_EXT_CAP_DWORD_COUNT) {
    Print(L"      Capability traversal limit reached\n");
  }
}

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
    Print(
      L"      Standard ReBAR 8-GiB support: %s (capability bit 17)\n",
      ((Capability & (1U << 17)) != 0) ? L"ADVERTISED" : L"NOT ADVERTISED"
      );
    Print(L"      (NVIDIA VSEC override is a separate mechanism; not inferred here)\n");
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
  UINT64 PrefWindowLength;
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
      L"      PREF raw: reg24=%08x base-upper=%08x limit-upper=%08x type-nibble=%x\n",
      PrefWindow,
      PrefBaseUpper,
      PrefLimitUpper,
      PrefWindow & 0x0F
      );
    
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
    } else {
      PrefWindowLength = PrefLimit64 - PrefBase64 + 1;
      Print(L"        Programmed window length: %016Lx bytes\n", PrefWindowLength);
      Print(
        L"        8-GiB BAR1 fit in this programmed window: %s\n",
        (PrefWindowLength >= BAR1_TARGET_BYTES) ? L"YES (size only; placement still matters)" : L"NO"
        );
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
  Report the firmware's root-bridge resource apertures and whether the
  host-bridge resource-allocation protocol is still exposed.

  This is deliberately query-only. Configuration() returns the root
  bridge's current resource descriptors; it does not report arbitrary free
  address space. GetAllocAttributes() is also query-only. We do not call
  NotifyPhase(), SubmitResources(), SetBusNumbers(), or any allocation phase
  method from this application.
**/
/**
  Print the known-good Linux allocation as the target for the UEFI port.

  This is arithmetic and reporting only. It does not reserve or program
  PCI resources.
**/
#if 0
STATIC
EFI_STATUS
ProbeNvidiaOverride8GiB (
  IN EFI_PCI_IO_PROTOCOL *PciIo
  )
{
  EFI_STATUS Status;
  EFI_STATUS RestoreStatus;
  UINT32 IdReg, ClassReg, HeaderReg;
  UINT32 VsecOffset, RebarOffset;
  UINT32 VsecHeader, VsecBody, OverrideSupport;
  UINT32 OriginalOverride, CurrentOverride, TargetOverride;
  UINT32 RebarHeader, Bar1Control;
  UINT32 CapBefore, CapArmed, CapRestored;
  UINT32 RestoreValue;
  BOOLEAN WriteAttempted = FALSE;
  BOOLEAN ProbePassed = FALSE;

  Print(L"\n=== NVIDIA 8-GiB 0x610 OVERRIDE PROBE ===\n");
  Print(L"Temporary PCI config write; no BAR resizing is performed.\n");

  if (PciIo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  /* Exact device identity. */
  Status = ReadConfig32(PciIo, 0x00, &IdReg);
  if (EFI_ERROR(Status) ||
      (IdReg & 0xFFFF) != NVIDIA_VENDOR_ID ||
      (IdReg >> 16) != NVIDIA_TARGET_DEVICE_ID) {
    Print(L"ABORT: target identity mismatch.\n");
    return EFI_SECURITY_VIOLATION;
  }

  Status = ReadConfig32(PciIo, 0x08, &ClassReg);
  if (EFI_ERROR(Status) ||
      (UINT8)(ClassReg >> 24) != PCI_CLASS_DISPLAY ||
      (UINT8)(ClassReg >> 16) != 0) {
    Print(L"ABORT: unexpected PCI class.\n");
    return EFI_SECURITY_VIOLATION;
  }

  Status = ReadConfig32(PciIo, 0x0C, &HeaderReg);
  if (EFI_ERROR(Status) ||
      (UINT8)((HeaderReg >> 16) & 0x7F) != PCI_HEADER_TYPE_ENDPOINT) {
    Print(L"ABORT: not a Type 0 endpoint.\n");
    return EFI_SECURITY_VIOLATION;
  }

  /* S1: VSEC location and header. */
  Status = FindExtendedCapability(
             PciIo, PCI_EXT_CAP_ID_VSEC, &VsecOffset);
  if (EFI_ERROR(Status) || VsecOffset != NVIDIA_VSEC_OFFSET) {
    Print(L"ABORT: S1 VSEC location failed.\n");
    return EFI_COMPROMISED_DATA;
  }

  Status = ReadConfig32(PciIo, VsecOffset, &VsecHeader);
  if (EFI_ERROR(Status) ||
      (VsecHeader & 0xFFFF) != PCI_EXT_CAP_ID_VSEC ||
      ((VsecHeader >> 16) & 0xF) != 1) {
    Print(L"ABORT: S1 VSEC header failed.\n");
    return EFI_COMPROMISED_DATA;
  }

  /* S2: VSEC body. */
  Status = ReadConfig32(PciIo, VsecOffset + 4, &VsecBody);
  if (EFI_ERROR(Status) ||
      (VsecBody & 0xFFFF) != 0x0001 ||
      ((VsecBody >> 16) & 0xF) != 1 ||
      ((VsecBody >> 20) & 0xFFF) != 0x024) {
    Print(L"ABORT: S2 VSEC body failed.\n");
    return EFI_COMPROMISED_DATA;
  }

  /* S3: framebuffer override support. */
  Status = ReadConfig32(
             PciIo, VsecOffset + 0x0C, &OverrideSupport);
  if (EFI_ERROR(Status) ||
      (OverrideSupport & (1U << 11)) == 0 ||
      (OverrideSupport & 0xFFFFE7F8U) != 0) {
    Print(L"ABORT: S3 override support check failed.\n");
    return EFI_UNSUPPORTED;
  }

  /* S4: original override field must be stock or validly armed. */
  Status = ReadConfig32(
             PciIo, VsecOffset + 0x10, &OriginalOverride);
  if (EFI_ERROR(Status) ||
      (OriginalOverride & 0xFFFFE07CU) != 0 ||
      (((OriginalOverride & NVIDIA_OVERRIDE_MASK) != 0) &&
       ((OriginalOverride & NVIDIA_OVERRIDE_ENABLE) == 0))) {
    Print(L"ABORT: S4 override register check failed.\n");
    return EFI_COMPROMISED_DATA;
  }

  /* S5: expected ReBAR capability location and BAR1 entry. */
  Status = FindExtendedCapability(
             PciIo, PCI_EXT_CAP_ID_REBAR, &RebarOffset);
  if (EFI_ERROR(Status) || RebarOffset != NVIDIA_REBAR_OFFSET) {
    Print(L"ABORT: S5 ReBAR location failed.\n");
    return EFI_COMPROMISED_DATA;
  }

  Status = ReadConfig32(PciIo, RebarOffset, &RebarHeader);
  if (EFI_ERROR(Status) ||
      (RebarHeader & 0xFFFF) != PCI_EXT_CAP_ID_REBAR) {
    Print(L"ABORT: ReBAR header invalid.\n");
    return EFI_COMPROMISED_DATA;
  }

  Status = ReadConfig32(PciIo, NVIDIA_BAR1_CAP_OFFSET + 4, &Bar1Control);
  if (EFI_ERROR(Status) || (Bar1Control & 7U) != REBAR_BAR1_INDEX) {
    Print(L"ABORT: BAR1 ReBAR control entry mismatch.\n");
    return EFI_COMPROMISED_DATA;
  }

  Status = ReadConfig32(PciIo, NVIDIA_BAR1_CAP_OFFSET, &CapBefore);
  if (EFI_ERROR(Status)) {
    Print(L"ABORT: cannot read BAR1 capability bitmap.\n");
    return Status;
  }

  Print(L"Original 0x610:       %08x\n", OriginalOverride);
  Print(L"Original BAR1 bitmap: %08x\n", CapBefore);
  Print(L"Target override:      %08x\n",
        (UINT32)NVIDIA_OVERRIDE_FIELD);

  TargetOverride =
    (OriginalOverride & ~NVIDIA_OVERRIDE_MASK) |
    NVIDIA_OVERRIDE_FIELD;

  /*
   * Write only bits 11:7. The original bits outside this mask
   * are preserved.
   */
  Status = PciIo->Pci.Write(
                      PciIo,
                      EfiPciIoWidthUint32,
                      VsecOffset + 0x10,
                      1,
                      &TargetOverride);

  WriteAttempted = TRUE;

  if (EFI_ERROR(Status)) {
    Print(L"0x610 write failed: %r\n", Status);
  } else {
    Status = ReadConfig32(
               PciIo, VsecOffset + 0x10, &CurrentOverride);

    if (!EFI_ERROR(Status) &&
        (CurrentOverride & NVIDIA_OVERRIDE_MASK) ==
          NVIDIA_OVERRIDE_FIELD) {

      Print(L"0x610 armed: %08x PASS\n", CurrentOverride);

      Status = ReadConfig32(
                 PciIo, NVIDIA_BAR1_CAP_OFFSET, &CapArmed);

      if (!EFI_ERROR(Status)) {
        Print(L"Armed BAR1 bitmap: %08x\n", CapArmed);

        if (((CapArmed & CapBefore) & 0xFFFFFFF0U) ==
              (CapBefore & 0xFFFFFFF0U) &&
            (CapArmed & NVIDIA_BAR1_TARGET_CAP_BIT) != 0) {
          Print(L"Capability superset: PASS\n");
          Print(L"8-GiB capability bit: PRESENT\n");
          ProbePassed = TRUE;
        } else {
          Print(L"Capability verification FAILED.\n");
        }
      }
    } else {
      Print(L"0x610 did not latch the requested field.\n");
    }
  }

  /*
   * Restore even if the write/readback/capability verification failed.
   */
  if (WriteAttempted) {
    RestoreStatus = ReadConfig32(
                      PciIo, VsecOffset + 0x10, &RestoreValue);

    if (!EFI_ERROR(RestoreStatus)) {
      RestoreValue =
        (RestoreValue & ~NVIDIA_OVERRIDE_MASK) |
        (OriginalOverride & NVIDIA_OVERRIDE_MASK);

      RestoreStatus = PciIo->Pci.Write(
                                  PciIo,
                                  EfiPciIoWidthUint32,
                                  VsecOffset + 0x10,
                                  1,
                                  &RestoreValue);
    }

    if (!EFI_ERROR(RestoreStatus)) {
      RestoreStatus = ReadConfig32(
                        PciIo, VsecOffset + 0x10, &CurrentOverride);

      if (!EFI_ERROR(RestoreStatus) &&
          (CurrentOverride & NVIDIA_OVERRIDE_MASK) !=
            (OriginalOverride & NVIDIA_OVERRIDE_MASK)) {
        RestoreStatus = EFI_DEVICE_ERROR;
      }
    }

    if (EFI_ERROR(RestoreStatus)) {
      Print(L"CRITICAL: 0x610 RESTORE FAILED: %r\n", RestoreStatus);
      Print(L"A reboot/reset may be required to clear the volatile state.\n");
      return RestoreStatus;
    }

    Print(L"Original 0x610 field restored: PASS (%08x)\n",
          CurrentOverride);

    Status = ReadConfig32(
               PciIo, NVIDIA_BAR1_CAP_OFFSET, &CapRestored);

    if (!EFI_ERROR(Status)) {
      Print(L"Restored BAR1 bitmap: %08x\n", CapRestored);

      if (((CapRestored ^ CapBefore) & 0xFFFFFFF0U) == 0) {
        Print(L"Capability bitmap restored: PASS\n");
      } else {
        Print(L"WARNING: capability bitmap differs from original.\n");
      }
    }
  }

  Print(L"PROBE RESULT: %s\n",
        ProbePassed ? L"PASS" : L"FAIL");
  Print(L"BAR1 resize attempted: NO\n");
  Print(L"Bridge-window writes:  NO\n");
  Print(L"=== END OVERRIDE PROBE ===\n");

  return ProbePassed ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
BOOLEAN
ConfirmOverrideProbe (
  VOID
  )
{
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;

  Print(L"\nTemporarily arm 8-GiB override? Press Y to continue: ");

  for (;;) {
    Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);

    if (Status == EFI_NOT_READY) {
      UINTN Index = 0;
      Status = gBS->WaitForEvent(
                       1, &gST->ConIn->WaitForKey, &Index);
      if (EFI_ERROR(Status)) {
        return FALSE;
      }
      continue;
    }

    if (EFI_ERROR(Status)) {
      return FALSE;
    }

    if (Key.UnicodeChar == L'y' || Key.UnicodeChar == L'Y') {
      Print(L"YES\n");
      return TRUE;
    }

    if (Key.UnicodeChar == L'n' || Key.UnicodeChar == L'N' ||
        Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Print(L"NO\n");
      return FALSE;
    }
  }
}
#endif

STATIC
VOID
PrintLinuxTargetResourcePlan (
  VOID
  )
{
  UINT64 Bar3End;
  UINT64 Bar1End;
  UINT64 WindowEnd;

  Bar3End = BAR3_TARGET_BASE + BAR3_TARGET_BYTES - 1;
  Bar1End = BAR1_TARGET_BASE + BAR1_TARGET_BYTES - 1;
  WindowEnd = PREF_WINDOW_TARGET_BASE + PREF_WINDOW_TARGET_BYTES - 1;

  Print(L"\n=== STEP 4: LINUX-REFERENCE RESOURCE PLAN (READ ONLY) ===\n");

  Print(L"BAR3:        %016Lx - %016Lx (%016Lx bytes)\n",
        BAR3_TARGET_BASE, Bar3End, BAR3_TARGET_BYTES);

  Print(L"BAR1:        %016Lx - %016Lx (%016Lx bytes)\n",
        BAR1_TARGET_BASE, Bar1End, BAR1_TARGET_BYTES);

  Print(L"PREF bridge: %016Lx - %016Lx (%016Lx bytes)\n",
        PREF_WINDOW_TARGET_BASE, WindowEnd, PREF_WINDOW_TARGET_BYTES);

  Print(L"BAR3 immediately precedes BAR1: %s\n",
        (Bar3End + 1 == BAR1_TARGET_BASE) ? L"YES" : L"NO");

  Print(L"BAR1 base is 8-GiB aligned:      %s\n",
        ((BAR1_TARGET_BASE & (BAR1_TARGET_BYTES - 1)) == 0)
          ? L"YES" : L"NO");

  Print(L"Bridge window covers both BARs: %s\n",
        (PREF_WINDOW_TARGET_BASE <= BAR3_TARGET_BASE &&
         WindowEnd >= Bar1End) ? L"YES" : L"NO");

  Print(L"Required bridge-window type: 64-bit prefetchable\n");
  Print(L"Reference: successful Linux PCI resource reassignment log.\n");
  Print(L"No PCI or GCD resource was modified.\n");

  Print(L"=== END LINUX-REFERENCE RESOURCE PLAN ===\n");
}

STATIC
VOID
PrintRootBridgeResourceMaps (
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *RootHandles;
  UINTN RootCount;
  UINTN Index;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *RootIo;
  EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *HostAlloc;
  VOID *Resources;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *Descriptor;
  UINTN DescriptorIndex;
  UINT64 AllocationAttributes;
  EFI_HANDLE ParentHandle;
  BOOLEAN SawDescriptor;
  BOOLEAN TargetCoveredByRootAperture;
  BOOLEAN HostAllocAttributesValid;
  BOOLEAN HostMem64Decode;

  RootHandles = NULL;
  RootCount = 0;

  Print(L"\n=== PCI ROOT-BRIDGE RESOURCE MAPS (READ ONLY) ===\n");

  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiPciRootBridgeIoProtocolGuid,
                  NULL,
                  &RootCount,
                  &RootHandles
                  );

  if (EFI_ERROR(Status)) {
    Print(L"Root bridge protocol enumeration failed: %r\n", Status);
    return;
  }

  Print(L"PCI root-bridge handles: %u\n", RootCount);

  for (Index = 0; Index < RootCount; Index++) {
    RootIo = NULL;
    Resources = NULL;
    HostAlloc = NULL;
    ParentHandle = NULL;

    TargetCoveredByRootAperture = FALSE;
    HostAllocAttributesValid = FALSE;
    HostMem64Decode = FALSE;

    Status = gBS->OpenProtocol(
                    RootHandles[Index],
                    &gEfiPciRootBridgeIoProtocolGuid,
                    (VOID **)&RootIo,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );

    if (EFI_ERROR(Status) || RootIo == NULL) {
      Print(L"  root[%u]: open failed: %r\n", Index, Status);
      continue;
    }

    ParentHandle = RootIo->ParentHandle;

    Print(
      L"  root[%u]: handle=%p segment=%04x parent-host-bridge=%p\n",
      Index,
      RootHandles[Index],
      RootIo->SegmentNumber,
      ParentHandle
      );

    if (RootIo->Configuration == NULL) {
      Print(L"    Configuration(): method unavailable\n");
    } else {
      Status = RootIo->Configuration(RootIo, &Resources);

      if (EFI_ERROR(Status)) {
        Print(L"    Configuration(): %r\n", Status);

      } else if (Resources == NULL) {
        Print(L"    Configuration(): success, NULL descriptor list\n");

      } else {
        Descriptor = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Resources;
        SawDescriptor = FALSE;

        for (DescriptorIndex = 0;
             DescriptorIndex < 64;
             DescriptorIndex++) {

          if (Descriptor->Desc == ACPI_END_TAG_DESCRIPTOR) {
            break;
          }

          if (Descriptor->Desc != ACPI_ADDRESS_SPACE_DESCRIPTOR) {
            Print(
              L"    unexpected resource descriptor type=%02x; stopping\n",
              Descriptor->Desc
              );
            break;
          }

          SawDescriptor = TRUE;

          Print(
            L"    aperture[%u]: type=%u granularity=%u flags=%02x "
            L"min=%016Lx max=%016Lx length=%016Lx translation=%016Lx\n",
            DescriptorIndex,
            Descriptor->ResType,
            Descriptor->AddrSpaceGranularity,
            Descriptor->SpecificFlag,
            Descriptor->AddrRangeMin,
            Descriptor->AddrRangeMax,
            Descriptor->AddrLen,
            Descriptor->AddrTranslationOffset
            );

          /*
           * Require one identity-mapped memory aperture to cover the
           * entire BAR3 + BAR1 target window.
           *
           * This checks firmware-reported resources only. It does not
           * prove that the root complex forwards transactions there.
           */
          if ((Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) &&
              (Descriptor->AddrRangeMin <= PREF_WINDOW_TARGET_BASE) &&
              (Descriptor->AddrRangeMax >=
                 (PREF_WINDOW_TARGET_BASE +
                  PREF_WINDOW_TARGET_BYTES - 1)) &&
              (Descriptor->AddrTranslationOffset == 0)) {

            TargetCoveredByRootAperture = TRUE;
          }

          Descriptor++;
        }

        if (!SawDescriptor) {
          Print(L"    no address-space descriptors returned\n");
        }

        if (DescriptorIndex == 64) {
          Print(L"    descriptor display limit reached\n");
        }

        /* Configuration() owns this buffer; do not FreePool(). */
      }
    }

    Print(
      L"    Full target inside identity-mapped root memory aperture: %s\n",
      TargetCoveredByRootAperture ? L"YES" : L"NO"
      );

    /*
     * Query the parent host bridge's allocation attributes.
     */
    if (ParentHandle != NULL) {
      Status = gBS->HandleProtocol(
                      ParentHandle,
                      &gEfiPciHostBridgeResourceAllocationProtocolGuid,
                      (VOID **)&HostAlloc
                      );

      if (EFI_ERROR(Status) || HostAlloc == NULL) {
        Print(
          L"    host-bridge resource-allocation protocol: "
          L"not exposed (%r)\n",
          Status
          );

      } else {
        if (HostAlloc->GetAllocAttributes == NULL) {
          Print(L"    GetAllocAttributes(): method unavailable\n");

        } else {
          AllocationAttributes = 0;

          Status = HostAlloc->GetAllocAttributes(
                                HostAlloc,
                                RootHandles[Index],
                                &AllocationAttributes
                                );

          if (EFI_ERROR(Status)) {
            Print(L"    GetAllocAttributes(): %r\n", Status);

          } else {
            HostAllocAttributesValid = TRUE;

            HostMem64Decode =
              ((AllocationAttributes &
                EFI_PCI_HOST_BRIDGE_MEM64_DECODE) != 0);

            Print(
              L"    host-bridge allocation attributes: %016Lx\n",
              AllocationAttributes
              );

            Print(
              L"      combine MEM/PMEM windows: %s\n",
              (AllocationAttributes &
               EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM) != 0 ?
                L"yes" : L"no"
              );

            Print(
              L"      64-bit memory decode:     %s\n",
              HostMem64Decode ? L"yes" : L"no"
              );
          }
        }

        Print(
          L"    host-bridge method availability: "
          L"NotifyPhase=%s GetNextRootBridge=%s "
          L"GetAllocAttributes=%s StartBusEnumeration=%s "
          L"SetBusNumbers=%s SubmitResources=%s "
          L"GetProposedResources=%s PreprocessController=%s\n",

          HostAlloc->NotifyPhase != NULL ? L"yes" : L"no",
          HostAlloc->GetNextRootBridge != NULL ? L"yes" : L"no",
          HostAlloc->GetAllocAttributes != NULL ? L"yes" : L"no",
          HostAlloc->StartBusEnumeration != NULL ? L"yes" : L"no",
          HostAlloc->SetBusNumbers != NULL ? L"yes" : L"no",
          HostAlloc->SubmitResources != NULL ? L"yes" : L"no",
          HostAlloc->GetProposedResources != NULL ? L"yes" : L"no",
          HostAlloc->PreprocessController != NULL ? L"yes" : L"no"
          );

        Print(
          L"    No host-bridge phase or allocation method was invoked.\n"
          );

        Print(
          L"    SubmitResources/NotifyPhase/SetResources are "
          L"enumeration-phase operations, not a safe late-resize API.\n"
          );

        Print(
          L"    Protocol presence does not prove late reallocation is legal.\n"
          );
      }

    } else {
      Print(L"    host-bridge parent handle unavailable\n");
    }

    /*
     * Final read-only audit.
     *
     * A mismatch here describes the firmware's published resource map.
     * It is not a hardware-forwarding test.
     */
    Print(L"\n    === ROOT-BRIDGE RESOURCE AUDIT ===\n");

    Print(L"    Target covered by Configuration(): %s\n",
          TargetCoveredByRootAperture ? L"YES" : L"NO");

    if (!HostAllocAttributesValid) {
      Print(L"    Host allocation attributes: unavailable\n");
    } else {
      Print(L"    Host 64-bit memory decode: %s\n",
            HostMem64Decode ? L"advertised" : L"not advertised");
    }

    Print(L"\n    Linux reference aperture: 4000000000-7FFFFFFFFF\n");
    Print(L"    Linux-tested target:      41FE000000-43FFFFFFFF\n");

    Print(L"    Configuration() and GCD are separate resource maps.\n");
    Print(L"    No host-bridge allocation phase was invoked.\n");
    Print(L"    No host-bridge resources were reallocated.\n");
    Print(L"    Physical forwarding was not tested by this audit.\n");

    gBS->CloseProtocol(
           RootHandles[Index],
           &gEfiPciRootBridgeIoProtocolGuid,
           ImageHandle,
           NULL
           );
  }

  if (RootHandles != NULL) {
    FreePool(RootHandles);
  }

  Print(L"=== END ROOT-BRIDGE RESOURCE MAPS ===\n");

  Print(
    L"NOTE: these are firmware-reported current apertures, "
    L"not a free-MMIO map.\n"
    );
}

/**
  Return a readable name for a GCD memory-space type.
**/
STATIC
CONST CHAR16 *
GcdMemoryTypeName (
  IN EFI_GCD_MEMORY_TYPE Type
  )
{
  switch (Type) {
    case EfiGcdMemoryTypeNonExistent:
      return L"NonExistent";
    case EfiGcdMemoryTypeReserved:
      return L"Reserved";
    case EfiGcdMemoryTypeSystemMemory:
      return L"SystemMemory";
    case EfiGcdMemoryTypeMemoryMappedIo:
      return L"MemoryMappedIo";
    case EfiGcdMemoryTypePersistent:
      return L"Persistent";
    case EfiGcdMemoryTypeMoreReliable:
      return L"MoreReliable";
    case EfiGcdMemoryTypeUnaccepted:
      return L"Unaccepted";
    default:
      return L"Unknown";
  }
}

/**
  Determine whether a descriptor contains a target-sized, target-aligned
  subrange. BARs must be aligned to their decoded size.
**/
STATIC
BOOLEAN
FindAlignedWindow (
  IN UINT64 Base,
  IN UINT64 Length,
  IN UINT64 WindowSize,
  OUT UINT64 *WindowBase
  )
{
  UINT64 AlignedBase;
  UINT64 Offset;
  UINT64 MaxUint64;

  if ((WindowBase == NULL) || (WindowSize == 0) ||
      ((WindowSize & (WindowSize - 1)) != 0) || (Length < WindowSize))
  {
    return FALSE;
  }

  MaxUint64 = ~(UINT64)0;
  if ((Base > (MaxUint64 - (WindowSize - 1))) ||
      (Length - 1 > (MaxUint64 - Base)))
  {
    return FALSE;
  }

  AlignedBase = (Base + WindowSize - 1) & ~(WindowSize - 1);
  if (AlignedBase < Base) {
    return FALSE;
  }

  Offset = AlignedBase - Base;
  if ((Offset > Length) || (WindowSize > (Length - Offset))) {
    return FALSE;
  }

  *WindowBase = AlignedBase;
  return TRUE;
}

/**
  Print the DXE Global Coherency Domain (GCD) memory-space map.

  This is query-only. In particular, NonExistent ranges are not assumed to
  be usable MMIO: they are merely address ranges that no component currently
  decodes. A free MemoryMappedIo descriptor with no ImageHandle is a stronger
  candidate, but it still does not prove that the PCI host bridge can decode
  or allocate the range. No GCD allocation or AddMemorySpace operation occurs.
**/
STATIC
VOID
PrintGcdMemorySpaceMap (
  VOID
  )
{
  EFI_STATUS Status;
  UINTN DescriptorCount;
  UINTN Index;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Map;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor;
  UINT64 WindowBase;
  UINT64 EndAddress;
  UINT64 MaxUint64;
  BOOLEAN HasAlignedWindow;
  BOOLEAN PrintedAny;
  UINT64 TargetEnd;
  UINT64 DescriptorEnd;
  
  BOOLEAN TargetOverlap;
  BOOLEAN TargetCoveredByFreeMmio;
  BOOLEAN TargetCoveredByOtherType;

  Print(L"\n=== DXE GCD MEMORY-SPACE MAP (READ ONLY) ===\n");

  if ((gDS == NULL) || (gDS->GetMemorySpaceMap == NULL)) {
    Print(L"DXE Services / GetMemorySpaceMap unavailable.\n");
    Print(L"=== END GCD MAP ===\n");
    return;
  }

  DescriptorCount = 0;
  Map = NULL;
  Status = gDS->GetMemorySpaceMap(&DescriptorCount, &Map);
  if (EFI_ERROR(Status) || (Map == NULL)) {
    Print(L"GetMemorySpaceMap(): %r (descriptors=%u map=%p)\n",
          Status, DescriptorCount, Map);
    Print(L"=== END GCD MAP ===\n");
    return;
  }

  Print(L"GCD memory descriptors: %u\n", DescriptorCount);
  Print(L"Target window size/alignment: %016Lx bytes\n", BAR1_TARGET_BYTES);
  MaxUint64 = ~(UINT64)0;
  PrintedAny = FALSE;
  TargetEnd = PREF_WINDOW_TARGET_END;

  TargetOverlap = FALSE;
  TargetCoveredByFreeMmio = FALSE;
  TargetCoveredByOtherType = FALSE;
  
  Print(L"Linux-reference prefetchable bridge range: %016Lx-%016Lx\n",
        PREF_WINDOW_TARGET_BASE, TargetEnd);

  for (Index = 0; Index < DescriptorCount; Index++) {
    Descriptor = &Map[Index];

    /* Keep the report focused on MMIO and large address-space ranges. */
    if ((Descriptor->GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) &&
        (Descriptor->Length < BAR1_TARGET_BYTES))
    {
      continue;
    }

    PrintedAny = TRUE;
    if ((Descriptor->Length == 0) ||
        (Descriptor->BaseAddress > (MaxUint64 - (Descriptor->Length - 1))))
    {
      EndAddress = MaxUint64;
    } else {
      EndAddress = Descriptor->BaseAddress + Descriptor->Length - 1;
    }

    Print(
      L"  gcd[%u]: type=%s base=%016Lx end=%016Lx length=%016Lx\n",
      Index,
      GcdMemoryTypeName(Descriptor->GcdMemoryType),
      Descriptor->BaseAddress,
      EndAddress,
      Descriptor->Length
      );
    Print(
      L"           capabilities=%016Lx attributes=%016Lx image=%p device=%p\n",
      Descriptor->Capabilities,
      Descriptor->Attributes,
      Descriptor->ImageHandle,
      Descriptor->DeviceHandle
      );
    /*
     * Audit the complete 8224-MiB bridge aperture required by the
     * successful Linux allocation.
     */
    if ((Descriptor->Length != 0) &&
        (Descriptor->BaseAddress <= MaxUint64 - (Descriptor->Length - 1))) {
    
      DescriptorEnd =
        Descriptor->BaseAddress + Descriptor->Length - 1;
    
      if ((Descriptor->BaseAddress <= TargetEnd) &&
          (DescriptorEnd >= PREF_WINDOW_TARGET_BASE)) {
    
        TargetOverlap = TRUE;
    
        Print(L"           OVERLAPS required Linux prefetchable bridge range\n");
    
        if ((Descriptor->BaseAddress <= PREF_WINDOW_TARGET_BASE) &&
            (DescriptorEnd >= TargetEnd)) {
    
          if ((Descriptor->GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) &&
              (Descriptor->ImageHandle == NULL)) {
    
            TargetCoveredByFreeMmio = TRUE;
    
            Print(L"           FULL 8224-MiB target is inside unowned GCD MMIO\n");
    
          } else {
    
            TargetCoveredByOtherType = TRUE;
    
            Print(L"           FULL target is covered, but not by unowned MMIO\n");
          }
        }
      }
    }

    HasAlignedWindow = FindAlignedWindow(
                         Descriptor->BaseAddress,
                         Descriptor->Length,
                         BAR1_TARGET_BYTES,
                         &WindowBase
                         );
    if (!HasAlignedWindow) {
      continue;
    }

    if ((Descriptor->GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) &&
        (Descriptor->ImageHandle == NULL))
    {
      Print(
        L"           8-GiB aligned UNALLOCATED MMIO candidate: %016Lx - %016Lx\n",
        WindowBase,
        WindowBase + BAR1_TARGET_BYTES - 1
        );
    } else if (Descriptor->GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Print(
        L"           8-GiB aligned NONEXISTENT range: %016Lx - %016Lx\n",
        WindowBase,
        WindowBase + BAR1_TARGET_BYTES - 1
        );
      Print(L"           not registered as MMIO; platform decode/registration required\n");
    }
  }

  if (!PrintedAny) {
    Print(L"No MMIO or target-sized GCD descriptors were reported.\n");
  }

  Print(L"Linux target-range overlap observed: %s\n",
        TargetOverlap ? L"YES" : L"NO");

  Print(L"Full target inside unowned MMIO descriptor: %s\n",
        TargetCoveredByFreeMmio ? L"YES" : L"NO");
  
  if (TargetCoveredByOtherType) {
    Print(L"WARNING: target range is covered by a non-free-MMIO GCD descriptor.\n");
  }
  
  if (!TargetCoveredByFreeMmio) {
    Print(L"WARNING: GCD does not prove the complete Linux bridge window is free MMIO.\n");
  }
  FreePool(Map);
  Print(L"No GCD ranges were added, allocated, freed, or modified.\n");
  Print(L"=== END GCD MAP ===\n");
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
  EFI_STATUS TransactionStatus;

  EFI_HANDLE *Handles;
  UINTN HandleCount;
  UINTN Index;
  EFI_PCI_IO_PROTOCOL *PciIo;
  PCI_DEVICE_RECORD *Records;

  NVIDIA_GOP_SNAPSHOT GopSnapshot;

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
  Records = NULL;
  HandleCount = 0;

  //
  // Capture the original GOP state before changing PCI configuration.
  // This function should remain silent.
  //
  (VOID)CaptureNvidiaGop(ImageHandle, &GopSnapshot);

  //
  // Perform the BAR1 resize transaction first.
  //
  TransactionStatus = RunBar1ResizeTransaction(ImageHandle);

  //
  // Everything below is post-transaction diagnostics.
  // Run even if the transaction failed or rolled back.
  //
  Print(L"\n=== GeForce BAR1 UEFI - POST-TRANSACTION RESOURCE SCAN ===\n");
  Print(L"PCI/GCD inventory captured after the resize transaction.\n");
  Print(L"BAR1 target: %016Lx bytes (8 GiB).\n\n", BAR1_TARGET_BYTES);

  PrintLinuxTargetResourcePlan();

  PrintRootBridgeResourceMaps(ImageHandle);
  PrintGcdMemorySpaceMap();
  Print(L"\n");

  Status = gBS->LocateHandleBuffer(
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );

  if (EFI_ERROR(Status)) {
    Print(L"LocateHandleBuffer(PciIo) failed: %r\n", Status);
    goto PostResizeDiagnostics;
  }

  Print(L"PCI I/O handles: %u\n\n", HandleCount);

  Records = AllocateZeroPool(sizeof(*Records) * HandleCount);
  if (Records == NULL) {
    Print(L"Failed to allocate PCI device records.\n");
    goto PostResizeDiagnostics;
  }

  CollectPciRecords(Handles, HandleCount, ImageHandle, Records);
  PrintTargetCompanionAudit(Records, HandleCount);

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
      if (!EFI_ERROR(Status)) {
        gBS->CloseProtocol(
               Handles[Index],
               &gEfiPciIoProtocolGuid,
               ImageHandle,
               NULL
               );
      }
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
      gBS->CloseProtocol(
             Handles[Index],
             &gEfiPciIoProtocolGuid,
             ImageHandle,
             NULL
             );
      continue;
    }

    if (EFI_ERROR(ReadConfig32(PciIo, 0x00, &IdReg)) ||
        EFI_ERROR(ReadConfig32(PciIo, 0x08, &ClassReg)) ||
        EFI_ERROR(ReadConfig32(PciIo, 0x0C, &HeaderReg))) {
      gBS->CloseProtocol(
             Handles[Index],
             &gEfiPciIoProtocolGuid,
             ImageHandle,
             NULL
             );
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
      gBS->CloseProtocol(
             Handles[Index],
             &gEfiPciIoProtocolGuid,
             ImageHandle,
             NULL
             );
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


    if (Vendor == NVIDIA_VENDOR_ID) {
      PrintParentBridge(Records, HandleCount, Segment, Bus);
      PrintBars(PciIo, BarCount);
      PrintBarResources(PciIo, BarCount);
      PrintProtocolOwners(Handles[Index], ImageHandle);
    
      if (IsNvidiaDisplay) {
        Print(L"    NVIDIA display controller candidate\n");
    
        /* Raw data first: independent evidence for decoded results. */
        PrintRawConfigDump(PciIo);
        PrintExtendedCapabilities(PciIo);
        PrintNvidiaVsec(PciIo, HeaderType);
      } else {
        Print(L"    NVIDIA companion/other function\n");
      }
    
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

    gBS->CloseProtocol(
           Handles[Index],
           &gEfiPciIoProtocolGuid,
           ImageHandle,
           NULL
           );
  }

  Print(L"=== END POST-TRANSACTION SCAN ===\n");
  Print(L"GetBarAttributes reports firmware resource descriptors; it does not enumerate all free host MMIO space.\n");

PostResizeDiagnostics:

  if (Records != NULL) {
    FreePool(Records);
    Records = NULL;
  }

  if (Handles != NULL) {
    FreePool(Handles);
    Handles = NULL;
  }

  //
  // Compare the GOP state captured before resizing with its current state.
  // This also runs after a failed or rolled-back transaction.
  //
  RunNvidiaGopPostResizeDiagnostics(ImageHandle, &GopSnapshot);
  
#if ENABLE_NVIDIA_GOP_DRIVER_RECONNECT
  
  //
  // Experimental phase: only run after a successful BAR1 transaction.
  // This is deliberately separate from the resize transaction.
  //
  if (!EFI_ERROR(TransactionStatus)) {
    NVIDIA_GOP_SNAPSHOT ReconnectedSnapshot;
    EFI_STATUS ReconnectStatus;
    EFI_STATUS CaptureStatus;

    Print(L"\n");
    Print(L"===============================================\n");
    Print(L" STARTING EXPERIMENTAL GOP DRIVER RECOVERY\n");
    Print(L"===============================================\n");

    ReconnectStatus =
      RunNvidiaGopDriverReconnect(ImageHandle);

    Print(L"GOP driver reconnect experiment returned: %r\n",
          ReconnectStatus);

    //
    // Attempt to capture GOP even if ConnectController reported
    // an error. The protocol may nevertheless have been installed.
    //
    CaptureStatus =
      CaptureNvidiaGop(ImageHandle, &ReconnectedSnapshot);

    if (!EFI_ERROR(CaptureStatus)) {
      RunNvidiaGopPostResizeDiagnostics(
        ImageHandle,
        &ReconnectedSnapshot
        );
    } else {
      Print(L"Post-reconnect GOP capture failed: %r\n",
            CaptureStatus);
    }
  }
  
#endif
  
  if (EFI_ERROR(TransactionStatus)) {
    Print(L"\nBAR1 resize transaction returned: %r\n",
          TransactionStatus);
    return TransactionStatus;
  }
  
  return EFI_SUCCESS;
