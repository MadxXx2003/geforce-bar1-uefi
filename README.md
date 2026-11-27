# GeForce BAR1 UEFI

Experimental EDK II UEFI application for resizes bar1 before chainbooting windows on NVIDIA Turing systems with less than or equal to 8gb of vram.
this project was inspired by [geforce-bar1-resize](https://github.com/noon-at-cgn/geforce-bar1-resize) and mostly written by Chatgpt over 3 days

## Current status: Fully booting with windows and fully detected on gpu-z

This revision adds:

- mostly logging

## Build

Push this repository to GitHub and run **Actions → Build UEFI PCI resource audit**. The workflow builds with EDK II and publishes `GeForceBar1Uefi.efi` as an artifact.

## Test without changing boot configuration

1. Download the Actions artifact.
2. Copy `GeForceBar1Uefi.efi` to a FAT32 USB drive.
3. Boot the built-in UEFI shell (or a trusted UEFI shell from USB).
4. Run `fs0:\GeForceBar1Uefi.efi` (adjust the filesystem mapping as needed).
5. Save the complete output.

note this project require your system already having above 4g decoding be on and working 

## Acknowledgements

This project was developed through independent implementation and hardware experimentation.

Its development was informed by the following projects and research:

- [geforce-bar1-resize](https://github.com/noon-at-cgn/geforce-bar1-resize) — BAR1 resizing techniques and NVIDIA framebuffer-size override research.
- [Linux kernel](https://github.com/torvalds/linux) — PCI resource allocation and BAR management behavior.
- [nv-uefi-unlocker](https://github.com/physicsdolphin/nv-uefi-unlocker) — UEFI-based NVIDIA hardware experimentation.
- [TianoCore EDK II](https://github.com/tianocore/edk2) — Used the UEFI development platform 

These projects are acknowledged as technical references and inspiration. This project does not claim ownership of their work or imply endorsement by their authors.

still if any of the above wish it off the internet send me a git issue because i never read my emails i will comply because it's too much of a bother 
