First release of the Qt 6 rebuild.

- Seven views: processes, cpu, memory, disk, network, gpu, forensic.
- GPU-drawn graphs that scroll at a constant speed, a cores-by-time heat map, custom window chrome.
- Per-process disk and network through ETW; per-process GPU utilisation and VRAM.
- CPU package and chiplet temperature, package power and effective clocks, and DDR5 module temperatures through PawnIO (offered by the installer).
- Drive temperatures, volumes and free space.
- Memory composition from the memory manager's page lists.
- Forensic view: identity, parent chain, modules with signers, sockets, thread stacks, findings.

Requires Windows 11 24H2 or later with sudo for Windows enabled in Inline mode (`sudo config --enable normal`). The installer checks both.
