/**
  Standalone, post-enumeration BAR1 resize transaction for the HP ZBook 17 G6.

  This is intentionally machine-specific. It reserves the Linux-proven MMIO
  range in GCD, quiesces NVIDIA functions .2/.3, then directly programs the
  NVIDIA VSEC/ReBAR registers, endpoint BARs, and Intel root-port window.

  It does not modify firmware volumes, NVRAM, BootOrder, or Windows files.
  It does not automatically chainload Windows. Run only after reviewing the
  preflight output and explicitly confirming with R.
**/

#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <PiDxe.h>
#include <Library/DxeServicesTableLib.h>
#include <Pi/PiDxeCis.h>

#define GPU_VENDOR                 0x10DE
#define GPU_DEVICE                 0x1F36
#define XHCI_DEVICE                0x1ADA
#define UCSI_DEVICE                0x1ADB
#define ROOT_VENDOR                0x8086
#define ROOT_DEVICE                0x1901

#define CFG_VENDOR_DEVICE          0x00
#define CFG_COMMAND                0x04
#define CFG_BAR1_LOW               0x14
#define CFG_BAR1_HIGH              0x18
#define CFG_BAR3_LOW               0x1C
#define CFG_BAR3_HIGH              0x20
#define CFG_PREF_WINDOW            0x24
#define CFG_PREF_BASE_UPPER        0x28
#define CFG_PREF_LIMIT_UPPER       0x2C

#define NVIDIA_VSEC_OVERRIDE       0x610
#define NVIDIA_BAR1_CAP             0xBBC
#define NVIDIA_BAR1_CTRL            0xBC0

#define VSEC_OVERRIDE_MASK         0x00000F80U
#define VSEC_OVERRIDE_VALUE        0x00000B80U
#define BAR1_CAP_8G                 (1U << 17)
#define REBAR_SIZE_FIELD_MASK      0x00001F00U
#define REBAR_SIZE_8G              (13U << 8)

#define BAR1_BASE_8G                0x0000004200000000ULL
#define BAR1_SIZE_8G                0x0000000200000000ULL
#define BAR3_BASE                   0x00000041FE000000ULL
#define BAR3_SIZE                   0x0000000002000000ULL
#define PREF_BASE                   BAR3_BASE
#define PREF_LENGTH                 0x0000000202000000ULL
#define PREF_END                    0x00000043FFFFFFFFULL

#define BAR1_OLD_LOW                0xA000000C
#define BAR1_OLD_HIGH               0x00000000
#define BAR3_OLD_LOW                0xB000000C
#define BAR3_OLD_HIGH               0x00000000
#define BAR1_NEW_LOW                0x0000000C
#define BAR1_NEW_HIGH               0x00000042
#define BAR3_NEW_LOW                0xFE00000C
#define BAR3_NEW_HIGH               0x00000041

#define PREF_NEW_REG                0xFFF1FE01
#define PREF_NEW_BASE_UPPER         0x00000041
#define PREF_NEW_LIMIT_UPPER        0x00000043

STATIC
EFI_STATUS
CfgRead32(EFI_PCI_IO_PROTOCOL *PciIo, UINT32 Offset, UINT32 *Value)
{
  if (PciIo == NULL || Value == NULL) return EFI_INVALID_PARAMETER;
  return PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, Offset, 1, Value);
}

STATIC
EFI_STATUS
CfgWrite32(EFI_PCI_IO_PROTOCOL *PciIo, UINT32 Offset, UINT32 Value)
{
  return PciIo->Pci.Write(PciIo, EfiPciIoWidthUint32, Offset, 1, &Value);
}

STATIC
EFI_STATUS
CfgRead16(EFI_PCI_IO_PROTOCOL *PciIo, UINT32 Offset, UINT16 *Value)
{
  if (PciIo == NULL || Value == NULL) return EFI_INVALID_PARAMETER;
  return PciIo->Pci.Read(PciIo, EfiPciIoWidthUint16, Offset, 1, Value);
}

STATIC
EFI_STATUS
CfgWrite16(EFI_PCI_IO_PROTOCOL *PciIo, UINT32 Offset, UINT16 Value)
{
  return PciIo->Pci.Write(PciIo, EfiPciIoWidthUint16, Offset, 1, &Value);
}

STATIC
EFI_STATUS
FindPciHandle(
  EFI_HANDLE ImageHandle,
  UINTN WantBus, UINTN WantDevice, UINTN WantFunction,
  EFI_HANDLE *FoundHandle
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  EFI_PCI_IO_PROTOCOL *PciIo;
  UINTN Segment, Bus, Device, Function;

  if (FoundHandle == NULL) return EFI_INVALID_PARAMETER;
  *FoundHandle = NULL;
  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid,
                                   NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;

  for (Index = 0; Index < Count; Index++) {
    PciIo = NULL;
    Status = gBS->OpenProtocol(Handles[Index], &gEfiPciIoProtocolGuid,
                               (VOID **)&PciIo, ImageHandle, NULL,
                               EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status) || PciIo == NULL) continue;

    Status = PciIo->GetLocation(PciIo, &Segment, &Bus, &Device, &Function);
    gBS->CloseProtocol(Handles[Index], &gEfiPciIoProtocolGuid,
                       ImageHandle, NULL);
    if (!EFI_ERROR(Status) && Segment == 0 && Bus == WantBus &&
        Device == WantDevice && Function == WantFunction) {
      *FoundHandle = Handles[Index];
      FreePool(Handles);
      return EFI_SUCCESS;
    }
  }

  FreePool(Handles);
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
OpenPciIo(EFI_HANDLE ImageHandle, EFI_HANDLE Handle, EFI_PCI_IO_PROTOCOL **PciIo)
{
  return gBS->OpenProtocol(Handle, &gEfiPciIoProtocolGuid,
                           (VOID **)PciIo, ImageHandle, NULL,
                           EFI_OPEN_PROTOCOL_GET_PROTOCOL);
}

STATIC
VOID
ClosePciIo(EFI_HANDLE ImageHandle, EFI_HANDLE Handle, EFI_PCI_IO_PROTOCOL *PciIo)
{
  if (Handle != NULL && PciIo != NULL) {
    gBS->CloseProtocol(Handle, &gEfiPciIoProtocolGuid, ImageHandle, NULL);
  }
}

STATIC
EFI_STATUS
CheckIdentity(EFI_PCI_IO_PROTOCOL *PciIo, UINT16 Vendor, UINT16 Device)
{
  EFI_STATUS Status;
  UINT32 Id;
  Status = CfgRead32(PciIo, CFG_VENDOR_DEVICE, &Id);
  if (EFI_ERROR(Status)) return Status;
  if ((UINT16)Id != Vendor || (UINT16)(Id >> 16) != Device) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WaitForResizeConfirmation(VOID)
{
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;
  UINTN EventIndex;
  EFI_EVENT WaitEvent;

  Print(L"\nTHIS WILL WRITE PCI CONFIGURATION SPACE.\n");
  Print(L"It will resize BAR1 to 8 GiB and move BAR3/BAR1 to the Linux-tested addresses.\n");
  Print(L"NVIDIA xHCI .2 and UCSI .3 will be left disabled for Windows to re-enumerate.\n");
  Print(L"Type R to proceed; any other key cancels: ");

  WaitEvent = gST->ConIn->WaitForKey;
  Status = gBS->WaitForEvent(1, &WaitEvent, &EventIndex);
  if (EFI_ERROR(Status)) return Status;
  Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
  if (EFI_ERROR(Status)) return Status;
  Print(L"%c\n", Key.UnicodeChar);
  return (Key.UnicodeChar == L'R' || Key.UnicodeChar == L'r')
           ? EFI_SUCCESS : EFI_ABORTED;
}

/**
  Execute the explicit BAR1 resize transaction.

  ImageHandle is used as the GCD allocation owner. The caller should invoke
  this only after its read-only scan has completed.
**/
EFI_STATUS
EFIAPI
RunBar1ResizeTransaction(IN EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  EFI_STATUS RollbackStatus;
  EFI_HANDLE GpuHandle, RootHandle, XhciHandle, UcsiHandle;
  EFI_PCI_IO_PROTOCOL *Gpu, *Root, *Xhci, *Ucsi;
  EFI_PHYSICAL_ADDRESS ReservedBase;
  BOOLEAN GcdAllocated;
  BOOLEAN XhciDisconnected;
  BOOLEAN UcsiDisconnected;
  BOOLEAN AlreadyDone;
  BOOLEAN RollbackFailed;

  UINT32 OldBar1Low, OldBar1High, OldBar3Low, OldBar3High;
  UINT32 OldOverride, OldCap, OldRebarCtrl;
  UINT32 OldPref, OldPrefBaseUpper, OldPrefLimitUpper;
  UINT32 ReadBack;
  UINT16 OldGpuCommand, OldRootCommand, OldXhciCommand, OldUcsiCommand;
  UINT16 ReadBack16;

  GpuHandle = RootHandle = XhciHandle = UcsiHandle = NULL;
  Gpu = Root = Xhci = Ucsi = NULL;
  GcdAllocated = FALSE;
  RollbackFailed = FALSE;
  XhciDisconnected = FALSE;
  UcsiDisconnected = FALSE;
  ReservedBase = PREF_BASE;

  Print(L"\n=== STANDALONE EFI BAR1 RESIZE TRANSACTION ===\n");
  Print(L"GPU: 0000:01:00.0 10DE:1F36\n");
  Print(L"Root port: 0000:00:01.0 8086:1901\n");
  Print(L"Target BAR3: %016Lx - %016Lx\n", BAR3_BASE, BAR3_BASE + BAR3_SIZE - 1);
  Print(L"Target BAR1: %016Lx - %016Lx\n", BAR1_BASE_8G, BAR1_BASE_8G + BAR1_SIZE_8G - 1);
  Print(L"Target bridge window: %016Lx - %016Lx (%Lu MiB)\n",
        PREF_BASE, PREF_END, PREF_LENGTH / 0x100000ULL);

  Status = FindPciHandle(ImageHandle, 1, 0, 0, &GpuHandle);
  if (EFI_ERROR(Status)) goto Done;
  Status = FindPciHandle(ImageHandle, 0, 1, 0, &RootHandle);
  if (EFI_ERROR(Status)) goto Done;
  Status = FindPciHandle(ImageHandle, 1, 0, 2, &XhciHandle);
  if (EFI_ERROR(Status)) goto Done;
  Status = FindPciHandle(ImageHandle, 1, 0, 3, &UcsiHandle);
  if (EFI_ERROR(Status)) goto Done;

  Status = OpenPciIo(ImageHandle, GpuHandle, &Gpu);
  if (EFI_ERROR(Status)) goto Done;
  Status = OpenPciIo(ImageHandle, RootHandle, &Root);
  if (EFI_ERROR(Status)) goto Done;
  Status = OpenPciIo(ImageHandle, XhciHandle, &Xhci);
  if (EFI_ERROR(Status)) goto Done;
  Status = OpenPciIo(ImageHandle, UcsiHandle, &Ucsi);
  if (EFI_ERROR(Status)) goto Done;

  Status = CheckIdentity(Gpu, GPU_VENDOR, GPU_DEVICE);
  if (EFI_ERROR(Status)) goto Done;
  Status = CheckIdentity(Root, ROOT_VENDOR, ROOT_DEVICE);
  if (EFI_ERROR(Status)) goto Done;
  Status = CheckIdentity(Xhci, GPU_VENDOR, XHCI_DEVICE);
  if (EFI_ERROR(Status)) goto Done;
  Status = CheckIdentity(Ucsi, GPU_VENDOR, UCSI_DEVICE);
  if (EFI_ERROR(Status)) goto Done;

  if (EFI_ERROR(CfgRead32(Gpu, CFG_BAR1_LOW, &OldBar1Low)) ||
      EFI_ERROR(CfgRead32(Gpu, CFG_BAR1_HIGH, &OldBar1High)) ||
      EFI_ERROR(CfgRead32(Gpu, CFG_BAR3_LOW, &OldBar3Low)) ||
      EFI_ERROR(CfgRead32(Gpu, CFG_BAR3_HIGH, &OldBar3High)) ||
      EFI_ERROR(CfgRead32(Gpu, NVIDIA_VSEC_OVERRIDE, &OldOverride)) ||
      EFI_ERROR(CfgRead32(Gpu, NVIDIA_BAR1_CAP, &OldCap)) ||
      EFI_ERROR(CfgRead32(Gpu, NVIDIA_BAR1_CTRL, &OldRebarCtrl)) ||
      EFI_ERROR(CfgRead32(Root, CFG_PREF_WINDOW, &OldPref)) ||
      EFI_ERROR(CfgRead32(Root, CFG_PREF_BASE_UPPER, &OldPrefBaseUpper)) ||
      EFI_ERROR(CfgRead32(Root, CFG_PREF_LIMIT_UPPER, &OldPrefLimitUpper)) ||
      EFI_ERROR(CfgRead16(Gpu, CFG_COMMAND, &OldGpuCommand)) ||
      EFI_ERROR(CfgRead16(Root, CFG_COMMAND, &OldRootCommand)) ||
      EFI_ERROR(CfgRead16(Xhci, CFG_COMMAND, &OldXhciCommand)) ||
      EFI_ERROR(CfgRead16(Ucsi, CFG_COMMAND, &OldUcsiCommand))) {
    Status = EFI_DEVICE_ERROR;
    goto Done;
  }

  AlreadyDone =
    OldBar1Low == BAR1_NEW_LOW && OldBar1High == BAR1_NEW_HIGH &&
    OldBar3Low == BAR3_NEW_LOW && OldBar3High == BAR3_NEW_HIGH &&
    (OldRebarCtrl & REBAR_SIZE_FIELD_MASK) == REBAR_SIZE_8G &&
    (OldOverride & VSEC_OVERRIDE_MASK) == VSEC_OVERRIDE_VALUE &&
    OldPref == PREF_NEW_REG &&
    OldPrefBaseUpper == PREF_NEW_BASE_UPPER &&
    OldPrefLimitUpper == PREF_NEW_LIMIT_UPPER;
  if (AlreadyDone) {
    Print(L"The target BAR/window/register state is already present. No writes needed.\n");
    Status = EFI_SUCCESS;
    goto Done;
  }

  /* Refuse to operate on an unexpected starting state. */
  if (OldBar1Low != BAR1_OLD_LOW || OldBar1High != BAR1_OLD_HIGH ||
      OldBar3Low != BAR3_OLD_LOW || OldBar3High != BAR3_OLD_HIGH ||
      (OldRebarCtrl & REBAR_SIZE_FIELD_MASK) != (8U << 8) ||
      (OldOverride & VSEC_OVERRIDE_MASK) != 0 ||
      OldPref != 0xB201A001 || OldPrefBaseUpper != 0 ||
      OldPrefLimitUpper != 0 || (OldCap & BAR1_CAP_8G) != 0) {
    Print(L"ABORT: live PCI state differs from the known-good scan baseline.\n");
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  Status = WaitForResizeConfirmation();
  if (EFI_ERROR(Status)) {
    Print(L"Cancelled; no hardware changes made.\n");
    Status = EFI_SUCCESS;
    goto Done;
  }

  /* Reserve the complete 8224-MiB window in GCD before touching hardware. */
  Status = gDS->AllocateMemorySpace(EfiGcdAllocateAddress,
                                    EfiGcdMemoryTypeMemoryMappedIo,
                                    25, PREF_LENGTH, &ReservedBase,
                                    ImageHandle, RootHandle);
  if (EFI_ERROR(Status) || ReservedBase != PREF_BASE) {
    Print(L"GCD reservation failed or returned an unexpected base: %r base=%016Lx\n",
          Status, ReservedBase);
    if (!EFI_ERROR(Status)) {
      gDS->FreeMemorySpace(ReservedBase, PREF_LENGTH);
      Status = EFI_NOT_FOUND;
    }
    goto Done;
  }
  GcdAllocated = TRUE;
  Print(L"GCD reservation: PASS\n");

  /* Disconnect only the unused NVIDIA USB-C companion functions. */
  ClosePciIo(ImageHandle, GpuHandle, Gpu); Gpu = NULL;
  ClosePciIo(ImageHandle, RootHandle, Root); Root = NULL;
  ClosePciIo(ImageHandle, XhciHandle, Xhci); Xhci = NULL;
  ClosePciIo(ImageHandle, UcsiHandle, Ucsi); Ucsi = NULL;

  Status = gBS->DisconnectController(XhciHandle, NULL, NULL);
  if (Status == EFI_SUCCESS) XhciDisconnected = TRUE;
  else if (Status != EFI_NOT_FOUND) goto Done;

  Status = gBS->DisconnectController(UcsiHandle, NULL, NULL);
  if (Status == EFI_SUCCESS) UcsiDisconnected = TRUE;
  else if (Status != EFI_NOT_FOUND) goto Done;

  Status = OpenPciIo(ImageHandle, GpuHandle, &Gpu); if (EFI_ERROR(Status)) goto Done;
  Status = OpenPciIo(ImageHandle, RootHandle, &Root); if (EFI_ERROR(Status)) goto Done;
  Status = OpenPciIo(ImageHandle, XhciHandle, &Xhci); if (EFI_ERROR(Status)) goto Done;
  Status = OpenPciIo(ImageHandle, UcsiHandle, &Ucsi); if (EFI_ERROR(Status)) goto Done;

  /* Recheck identities after driver disconnection. */
  Status = CheckIdentity(Gpu, GPU_VENDOR, GPU_DEVICE); if (EFI_ERROR(Status)) goto Done;
  Status = CheckIdentity(Root, ROOT_VENDOR, ROOT_DEVICE); if (EFI_ERROR(Status)) goto Done;
  Status = CheckIdentity(Xhci, GPU_VENDOR, XHCI_DEVICE); if (EFI_ERROR(Status)) goto Done;
  Status = CheckIdentity(Ucsi, GPU_VENDOR, UCSI_DEVICE); if (EFI_ERROR(Status)) goto Done;

  /* Driver Stop() may have changed these bits; snapshot the post-stop state. */
  Status = CfgRead16(Xhci, CFG_COMMAND, &OldXhciCommand); if (EFI_ERROR(Status)) goto Done;
  Status = CfgRead16(Ucsi, CFG_COMMAND, &OldUcsiCommand); if (EFI_ERROR(Status)) goto Done;

  Status = CfgWrite16(Xhci, CFG_COMMAND, (UINT16)(OldXhciCommand & ~0x0007));
  if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite16(Ucsi, CFG_COMMAND, (UINT16)(OldUcsiCommand & ~0x0007));
  if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite16(Gpu, CFG_COMMAND, (UINT16)(OldGpuCommand & ~0x0006));
  if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite16(Root, CFG_COMMAND, (UINT16)(OldRootCommand & ~0x0002));
  if (EFI_ERROR(Status)) goto Rollback;
  if (EFI_ERROR(CfgRead16(Xhci, CFG_COMMAND, &ReadBack16)) || (ReadBack16 & 0x0007) != 0 ||
      EFI_ERROR(CfgRead16(Ucsi, CFG_COMMAND, &ReadBack16)) || (ReadBack16 & 0x0007) != 0) {
    Status = EFI_DEVICE_ERROR;
    goto Rollback;
  }

  /* Enable the NVIDIA-specific 8-GiB capability, then select ReBAR size 13. */
  Status = CfgWrite32(Gpu, NVIDIA_VSEC_OVERRIDE,
                      (OldOverride & ~VSEC_OVERRIDE_MASK) | VSEC_OVERRIDE_VALUE);
  if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgRead32(Gpu, NVIDIA_BAR1_CAP, &ReadBack);
  if (EFI_ERROR(Status) || (ReadBack & BAR1_CAP_8G) == 0) {
    Status = EFI_UNSUPPORTED;
    goto Rollback;
  }
  Status = CfgWrite32(Gpu, NVIDIA_BAR1_CTRL,
                      (OldRebarCtrl & ~REBAR_SIZE_FIELD_MASK) | REBAR_SIZE_8G);
  if (EFI_ERROR(Status)) goto Rollback;

  /* Move the two GPU BARs. Write upper dwords before lower dwords. */
  Status = CfgWrite32(Gpu, CFG_BAR3_HIGH, BAR3_NEW_HIGH); if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite32(Gpu, CFG_BAR3_LOW, BAR3_NEW_LOW); if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite32(Gpu, CFG_BAR1_HIGH, BAR1_NEW_HIGH); if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite32(Gpu, CFG_BAR1_LOW, BAR1_NEW_LOW); if (EFI_ERROR(Status)) goto Rollback;

  /* Program the Intel root-port 64-bit prefetchable forwarding window. */
  Status = CfgWrite32(Root, CFG_PREF_BASE_UPPER, PREF_NEW_BASE_UPPER);
  if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite32(Root, CFG_PREF_LIMIT_UPPER, PREF_NEW_LIMIT_UPPER);
  if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite32(Root, CFG_PREF_WINDOW, PREF_NEW_REG);
  if (EFI_ERROR(Status)) goto Rollback;

  /* Verify every changed register before re-enabling GPU/root-port decoding. */
  if (EFI_ERROR(CfgRead32(Gpu, NVIDIA_VSEC_OVERRIDE, &ReadBack)) ||
      (ReadBack & VSEC_OVERRIDE_MASK) != VSEC_OVERRIDE_VALUE) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Gpu, NVIDIA_BAR1_CAP, &ReadBack)) ||
      (ReadBack & BAR1_CAP_8G) == 0) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Gpu, NVIDIA_BAR1_CTRL, &ReadBack)) ||
      (ReadBack & REBAR_SIZE_FIELD_MASK) != REBAR_SIZE_8G) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Gpu, CFG_BAR1_LOW, &ReadBack)) || ReadBack != BAR1_NEW_LOW) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Gpu, CFG_BAR1_HIGH, &ReadBack)) || ReadBack != BAR1_NEW_HIGH) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Gpu, CFG_BAR3_LOW, &ReadBack)) || ReadBack != BAR3_NEW_LOW) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Gpu, CFG_BAR3_HIGH, &ReadBack)) || ReadBack != BAR3_NEW_HIGH) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Root, CFG_PREF_WINDOW, &ReadBack)) || ReadBack != PREF_NEW_REG) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Root, CFG_PREF_BASE_UPPER, &ReadBack)) || ReadBack != PREF_NEW_BASE_UPPER) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }
  if (EFI_ERROR(CfgRead32(Root, CFG_PREF_LIMIT_UPPER, &ReadBack)) || ReadBack != PREF_NEW_LIMIT_UPPER) {
    Status = EFI_DEVICE_ERROR; goto Rollback;
  }

  Status = CfgWrite16(Root, CFG_COMMAND, OldRootCommand);
  if (EFI_ERROR(Status)) goto Rollback;
  Status = CfgWrite16(Gpu, CFG_COMMAND, OldGpuCommand);
  if (EFI_ERROR(Status)) goto Rollback;
  if (EFI_ERROR(CfgRead16(Root, CFG_COMMAND, &ReadBack16)) || ReadBack16 != OldRootCommand ||
      EFI_ERROR(CfgRead16(Gpu, CFG_COMMAND, &ReadBack16)) || ReadBack16 != OldGpuCommand) {
    Status = EFI_DEVICE_ERROR;
    goto Rollback;
  }

  Print(L"\nSUCCESS: BAR1 RESIZE TRANSACTION VERIFIED\n");
  Print(L"BAR3: %016Lx (%u MiB)\n", BAR3_BASE, (UINTN)(BAR3_SIZE / 0x100000ULL));
  Print(L"BAR1: %016Lx (8192 MiB)\n", BAR1_BASE_8G);
  Print(L"Root-port prefetch window: %016Lx-%016Lx\n", PREF_BASE, PREF_END);
  Print(L"NVIDIA 0x610 override remains enabled; ReBAR control is 0x00000D01.\n");
  Print(L"NVIDIA functions .2/.3 remain decode-disabled; HDMI audio .1 was untouched.\n");
  Print(L"GCD target window remains reserved by this EFI image.\n");
  Print(L"No Windows chainload was attempted. Verify this state before booting Windows.\n");
  Status = EFI_SUCCESS;
  goto Done;

Rollback:
  Print(L"\nERROR during resize transaction: %r. Attempting rollback...\n", Status);
  RollbackStatus = EFI_SUCCESS;
  /* Keep forwarding and endpoint decoding disabled while restoring old state. */
  if (Root != NULL) {
    if (EFI_ERROR(CfgWrite16(Root, CFG_COMMAND, (UINT16)(OldRootCommand & ~0x0002)))) RollbackStatus = EFI_DEVICE_ERROR;
  }
  if (Gpu != NULL) {
    if (EFI_ERROR(CfgWrite16(Gpu, CFG_COMMAND, (UINT16)(OldGpuCommand & ~0x0006)))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Gpu, NVIDIA_BAR1_CTRL, OldRebarCtrl))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Gpu, CFG_BAR3_HIGH, OldBar3High))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Gpu, CFG_BAR3_LOW, OldBar3Low))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Gpu, CFG_BAR1_HIGH, OldBar1High))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Gpu, CFG_BAR1_LOW, OldBar1Low))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Gpu, NVIDIA_VSEC_OVERRIDE, OldOverride))) RollbackStatus = EFI_DEVICE_ERROR;
  }
  if (Root != NULL) {
    if (EFI_ERROR(CfgWrite32(Root, CFG_PREF_BASE_UPPER, OldPrefBaseUpper))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Root, CFG_PREF_LIMIT_UPPER, OldPrefLimitUpper))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite32(Root, CFG_PREF_WINDOW, OldPref))) RollbackStatus = EFI_DEVICE_ERROR;
  }
  /* Re-enable decoders only if every resource register was restored. */
  if (RollbackStatus == EFI_SUCCESS && Root != NULL && Gpu != NULL) {
    if (EFI_ERROR(CfgWrite16(Root, CFG_COMMAND, OldRootCommand))) RollbackStatus = EFI_DEVICE_ERROR;
    if (EFI_ERROR(CfgWrite16(Gpu, CFG_COMMAND, OldGpuCommand))) RollbackStatus = EFI_DEVICE_ERROR;
  }
  if (RollbackStatus == EFI_SUCCESS) {
    if (Xhci != NULL && EFI_ERROR(CfgWrite16(Xhci, CFG_COMMAND, OldXhciCommand))) RollbackStatus = EFI_DEVICE_ERROR;
    if (Ucsi != NULL && EFI_ERROR(CfgWrite16(Ucsi, CFG_COMMAND, OldUcsiCommand))) RollbackStatus = EFI_DEVICE_ERROR;
  }

  if (RollbackStatus == EFI_SUCCESS) {
    Print(L"Rollback register writes completed.\n");
  } else {
    RollbackFailed = TRUE;
    Print(L"WARNING: one or more rollback writes failed. Decode may remain disabled; keeping GCD reservation and companions quiesced. Power-cycle may be required.\n");
  }

Done:
  ClosePciIo(ImageHandle, GpuHandle, Gpu);
  ClosePciIo(ImageHandle, RootHandle, Root);
  ClosePciIo(ImageHandle, XhciHandle, Xhci);
  ClosePciIo(ImageHandle, UcsiHandle, Ucsi);

  if (Status != EFI_SUCCESS) {
    if (GcdAllocated && !RollbackFailed) {
      EFI_STATUS FreeStatus = gDS->FreeMemorySpace(ReservedBase, PREF_LENGTH);
      if (EFI_ERROR(FreeStatus)) Print(L"WARNING: GCD FreeMemorySpace failed: %r\n", FreeStatus);
    }
    if (XhciDisconnected && !RollbackFailed) {
      EFI_STATUS ConnectStatus = gBS->ConnectController(XhciHandle, NULL, NULL, TRUE);
      if (EFI_ERROR(ConnectStatus)) Print(L"WARNING: reconnect xHCI failed: %r\n", ConnectStatus);
    }
    if (UcsiDisconnected && !RollbackFailed) {
      EFI_STATUS ConnectStatus = gBS->ConnectController(UcsiHandle, NULL, NULL, TRUE);
      if (EFI_ERROR(ConnectStatus)) Print(L"WARNING: reconnect UCSI failed: %r\n", ConnectStatus);
    }
  }
  return Status;
}
