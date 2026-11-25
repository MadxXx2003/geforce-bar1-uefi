# GeForce BAR1 UEFI

An experimental EDK II project intended to investigate a firmware-stage BAR1
resize for NVIDIA Turing GPUs, then chainload Windows Boot Manager.

## Current status: Stage 0 — read-only PCI scanner

The first build is intentionally **read-only**. It:

- Enumerates PCI functions exposed through `EFI_PCI_IO_PROTOCOL`.
- Reports NVIDIA vendor/device IDs, class codes, BDFs, and raw BAR registers.
- Walks PCI Express extended capabilities where firmware exposes them.
- Reads (but does not modify) NVIDIA config-space offsets `0x600`, `0x604`,
  `0x60c`, and `0x610`.
- Reports the Intel root port at `0000:00:01.0`.

It does **not** resize BAR1, write the NVIDIA VSEC, remove PCI functions,
modify NVRAM boot entries, or chainload another EFI program.

## Build

Push this repository to GitHub and run **Actions → Build UEFI PCI scanner**.
The workflow builds with EDK II and publishes `GeForceBar1Uefi.efi` as an
artifact.

## Test without changing the boot configuration

1. Download the Actions artifact.
2. Copy `GeForceBar1Uefi.efi` to a FAT32 USB drive.
3. Boot the machine's built-in UEFI shell (or a trusted UEFI shell from USB).
4. Run the application from the shell, for example `fs0:\GeForceBar1Uefi.efi`.
5. Save or photograph the complete output.

Do not install it as the default bootloader yet. Do not add it to BootOrder.

## Hardware target under investigation

The initial target machine has:

- NVIDIA GPU: `10de:1f36`, subsystem `103c:860e`, BDF `0000:01:00.0`
- Companion functions: `10de:1ada` (xHCI, `.2`) and `10de:1adb` (UCSI, `.3`)
- Intel root port: `0000:00:01.0`
- Windows ESP GPT partition GUID:
  `3fb3b584-d310-11f1-96a3-b89a2a09d2af`
- Windows loader: `\EFI\Microsoft\Boot\bootmgfw.efi`

## Why this is only a scanner

The Linux `geforce-bar1-resize` project does more than write `0x610`: it relies
on Linux PCI resource management (`resource1_resize`) to release/reassign BARs
and expand bridge windows. UEFI has no direct equivalent to that Linux sysfs
operation. A correct firmware implementation must explicitly handle BAR sizing,
alignment, bridge windows, sibling functions, and rollback, then test whether
Windows preserves the resulting allocation.

No destructive operation should be added until the scanner output has been
reviewed and the allocation strategy is established.
