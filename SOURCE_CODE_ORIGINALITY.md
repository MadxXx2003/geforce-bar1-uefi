# Source Code Originality and Attribution Review

## Summary

Based on the source files inspected, there is no indication that this project copied or stole source code from the following projects:

1. [geforce-bar1-resize](https://github.com/noon-at-cgn/geforce-bar1-resize)
2. [nv-uefi-unlocker](https://github.com/physicsdolphin/nv-uefi-unlocker)
3. [Linux kernel](https://github.com/torvalds/linux)
4. [EDK II](https://github.com/tianocore/edk2)

The project shares technical concepts, hardware knowledge, and general approaches with these projects. However, sharing an idea, technique, hardware register, or established PCI concept is not the same as copying source code.

## Project comparisons

### 1. geforce-bar1-resize

The project [geforce-bar1-resize](https://github.com/noon-at-cgn/geforce-bar1-resize) is a Linux-based solution for resizing NVIDIA GPU BAR1.

Both projects deal with NVIDIA BAR1 resizing and use the NVIDIA framebuffer-size override at PCI configuration-space offset `0x610`.

However, their implementations are substantially different:

- `geforce-bar1-resize` uses Linux kernel PCI resource-management APIs.
- Its `gbr_layout` implementation handles PCI resource layout, topology-aware placement, and resource reassignment.
- This project uses EDK II UEFI interfaces and direct PCI configuration-space access.
- Its implementation is a machine-specific transaction involving GCD memory reservation, PCI function quiescing, BAR and bridge-window modification, verification, and rollback.

The shared register and hardware technique are not evidence of source-code copying.

### 2. nv-uefi-unlocker

The project [nv-uefi-unlocker](https://github.com/physicsdolphin/nv-uefi-unlocker) is another UEFI-based NVIDIA-related project.

The inspected implementation is written in Rust and uses a different architecture from this project's C implementation.

Although both projects operate in the UEFI environment and deal with PCI devices, no obvious copied C implementation or substantial source-code overlap was identified in the inspected files.

### 3. Linux kernel

This project uses established PCI concepts and hardware mechanisms, including:

- PCI Base Address Registers (BARs).
- Resizable BAR (ReBAR).
- PCI configuration-space access.
- PCI command registers.
- PCI bridge windows.
- PCI resource allocation and address-space management.

These are standard PCI mechanisms, not code owned exclusively by the Linux kernel.

The Linux implementation uses kernel-specific structures and APIs, such as `struct pci_dev`, `struct resource`, and Linux PCI resource-management functions.

This project's implementation instead operates through UEFI interfaces and firmware-level PCI access.

No Linux kernel source implementation was identified as having been copied into the inspected project files.

### 4. EDK II

This project uses EDK II interfaces, headers, and libraries to operate in the UEFI environment.

For example, the EDK II PCI I/O protocol provides standard interfaces for PCI configuration-space access and device interaction.

Using these interfaces is not equivalent to copying their implementation.

EDK II is generally licensed under BSD-2-Clause-Patent, although individual components and third-party dependencies may have different licenses.

Appropriate license notices must be retained for any EDK II components actually distributed or incorporated into the final binary.

## What makes this implementation independent?

The project's implementation was developed around a specific hardware configuration and experimentally verified behavior.

Its main implementation includes:

- Identifying the NVIDIA GPU and relevant companion PCI functions.
- Reserving the required address range through UEFI GCD.
- Quiescing the relevant companion functions.
- Applying the NVIDIA framebuffer-size override.
- Resizing the GPU BARs.
- Adjusting the upstream PCI bridge's prefetchable memory window.
- Reading back and verifying the resulting configuration.
- Rolling back changes when verification fails.
- Performing GOP diagnostics.
- Chainloading Windows Boot Manager after the operation.

This is a distinct UEFI-oriented implementation rather than a direct port of the Linux resource-layout code.

## AI assistance and authorship

The project was developed with substantial AI assistance.

AI-generated code does not automatically establish that code is original or that it contains no similarities to existing implementations. However, the project's hardware requirements, design decisions, testing, debugging, and validation were driven by the project's development process and real hardware observations.

The use of AI assistance should therefore be acknowledged honestly rather than presented as proof of either copying or complete originality.

## Limitations of this review

This assessment is based on a review of selected source files, not a formal legal or software-similarity audit.

The following work has not been completed:

- A complete similarity scan against every revision of all four referenced projects.
- A full review of every source file and historical commit.
- A complete audit of all EDK II components and libraries linked into the final EFI binary.
- A guarantee that AI-generated code contains no coincidental similarities to existing code.

Consequently, the appropriate conclusion is that no evidence of copied source code was found in the inspected files, rather than an absolute guarantee that no overlap exists anywhere.

## Suggested attribution statement

This project is an independently developed, AI-assisted implementation informed by publicly available research, hardware experimentation, and existing projects.

The referenced projects are acknowledged for their technical contributions and inspiration. Their source code is not claimed as part of this project.

This project implements its own UEFI-based approach to NVIDIA BAR1 resizing, PCI resource configuration, verification, rollback, and Windows chainloading.

## Licensing considerations

The project's own original code can be licensed separately from the external interfaces and dependencies it uses.

If the goal is to require people who distribute modified versions or derivative works to provide corresponding source code, **GPL-3.0-or-later** is a reasonable choice for the project's own code, subject to checking compatibility with the actual dependencies and their licenses.

GPL does not generally require someone to publish modifications that remain entirely private and are not distributed.

An AGPL license has additional source-sharing obligations for certain network-accessible software, but those obligations are not generally relevant to a standalone UEFI application.

Third-party license notices must remain intact, and the final binary's dependencies should be reviewed before publishing a definitive licensing statement.

---

## Final assessment

Based on the files inspected, this project can reasonably be described as an independently developed, AI-assisted UEFI implementation inspired by existing research and projects.

There is no evidence from the reviewed source files that the project deliberately copied implementation code from `geforce-bar1-resize`, `nv-uefi-unlocker`, the Linux kernel, or EDK II.

The distinction is important: the project builds on established hardware knowledge and standard interfaces, but its inspected implementation follows a different architecture and performs its own UEFI-specific operations.
