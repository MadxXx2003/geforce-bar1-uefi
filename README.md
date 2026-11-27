# GeForce BAR1 UEFI

Experimental EDK II UEFI application for read-only PCI resource, root-bridge aperture, and driver-ownership diagnostics on NVIDIA Turing systems.

## Current status: Stage 2B — read-only GCD MMIO aperture audit

This revision adds:

- PCI function inventory for NVIDIA functions and the Intel upstream root port.
- Bus-number-based parent-bridge association for each NVIDIA function.
- Raw BAR register reporting.
- `EFI_PCI_IO_PROTOCOL.GetBarAttributes()` queries for each BAR on each NVIDIA function, including the returned ACPI address-space descriptors, resource range, granularity, flags, translation, and length.
- `OpenProtocolInformation()` inventory for agents opening each NVIDIA function's `EFI_PCI_IO_PROTOCOL`, including agent/controller handles, open count, and open attributes. Direct `BY_DRIVER` ownership is called out separately; handle values are printed as raw firmware handles, not guessed driver names.
- PCIe extended-capability traversal that permits backward next-pointers while detecting loops.
- ReBAR and NVIDIA VSEC structural inspection, without writes.
- Current upstream bridge-window decoding and an 8-GiB size comparison.
- `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL.Configuration()` aperture descriptors.
- Parent host-bridge resource-allocation protocol presence and decoded allocation attributes.
- DXE GCD memory-space map, including descriptor type, range, capabilities, current attributes, and allocation owner handles.
- Detection of 8-GiB-aligned ranges that are already registered as unallocated MMIO, separately from NonExistent address ranges that would require platform registration/decode.

The scanner does **not** disconnect drivers, disable functions, change PCI command bits, resize BARs, modify NVIDIA VSEC registers, alter NVRAM, or chainload Windows. The new root-bridge queries are read-only; the application does not call host-bridge allocation phase methods. The `BY_DRIVER` entries identify direct protocol ownership; `GET_PROTOCOL` and `BY_HANDLE_PROTOCOL` entries are references, not proof that the listed agent is actively managing the function.

## Interpreting the resource report

`GetBarAttributes()` reports the PCI bus driver's current resource descriptors for a BAR. It is not a complete map of unallocated host-bridge MMIO address space, and it does not prove that a larger allocation can be made.

The root-bridge `Configuration()` descriptors describe current apertures, not a free-MMIO map. The host-bridge allocation protocol being present does not mean its allocation state machine can be restarted after PCI enumeration. The GCD map adds a system-wide view: a `MemoryMappedIo` descriptor with a NULL `ImageHandle` is unallocated in GCD, while a `NonExistent` descriptor is not registered as a resource and is not automatically safe or usable for PCI MMIO. Even an aligned GCD candidate does not prove the root bridge can decode it.

The 8-GiB preflight therefore reports separate constraints:

1. Whether the standard ReBAR capability bitmap advertises the 8-GiB size.
2. Whether the currently programmed upstream prefetchable bridge window is at least 8 GiB.
3. The firmware-reported current BAR resource descriptors for the GPU and companion functions.

A proprietary NVIDIA VSEC override, if supported, is a separate mechanism and is not treated as proof that host MMIO space can be allocated. The current resource descriptors and bridge windows are observations of the active firmware configuration, not an enumeration of all possible free address ranges.

## Build

Push this repository to GitHub and run **Actions → Build UEFI PCI resource audit**. The workflow builds with EDK II and publishes `GeForceBar1Uefi.efi` as an artifact.

## Test without changing boot configuration

1. Download the Actions artifact.
2. Copy `GeForceBar1Uefi.efi` to a FAT32 USB drive.
3. Boot the built-in UEFI shell (or a trusted UEFI shell from USB).
4. Run `fs0:\GeForceBar1Uefi.efi` (adjust the filesystem mapping as needed).
5. Save the complete output.

Do not install it as the default bootloader or add it to BootOrder.

## Hardware under investigation

- NVIDIA GPU: `10de:1f36`, subsystem `103c:860e`, BDF `0000:01:00.0`
- NVIDIA audio: function `.1`
- NVIDIA xHCI: `10de:1ada`, function `.2`
- NVIDIA UCSI: `10de:1adb`, function `.3`
- Intel root port: `0000:00:01.0`

## Safety boundary

This build is strictly observational. Driver detachment and PCI writes remain out of scope until the root-bridge aperture, host-bridge allocation state, bridge-window programming requirements, DMA quiescence, and rollback strategy are established. In particular, disconnecting `.2`/`.3` does not itself release their BAR allocations from the PCI bus driver. A later transaction must either run at the correct pre-enumeration DXE integration point or implement and verify a complete resource move; it must not call `SubmitResources()` or `NotifyPhase()` out of sequence from this post-enumeration application. The GCD audit is observational only: it never calls `AllocateMemorySpace()`, `AddMemorySpace()`, `FreeMemorySpace()`, or `RemoveMemorySpace()`.
