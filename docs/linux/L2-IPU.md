# TASK-074 L2: the fast IPU video decoder on Linux (TASK-027 IN-490, slice365)

The release's IPU capsule (`scripts/prepare_ipu_fast.py`; pinned by `ipu_capsule_sha256` in `slice408-pcsx2.json` and
`slice408-spec2.json`, the hash of its `runtime.json`, which pins `ipu.cpp`) now builds into the Linux game, both games.

**What it does:** GT4 plays video behind its menus and in the intro; the donor's decoder took about half of the EE thread
there. The derived `ipu/ipu.cpp` uses the bit reader, inverse DCT and colour conversion of `native/ipu_fast.hpp`, with
exactly the donor's results.

**Build:** `cmake/LinuxHost.cmake` `-DGT4_LINUX_IPU_RUNTIME=<capsule>`: the receipt must equal the release pin and
`ipu.cpp` its receipt; it replaces the donor `ipu/ipu.cpp` in the game runtime (the L1 headless smoke runner keeps the donor
file). The capsule comes from `scripts/linux/v200-inputs.sh` (`<prefix>-ipu-fast-runtime`), one for both games.

## Checks (2026-11-23)

| Check | Result | Label |
|---|---|---|
| Capsule against the pins | receipt `80da60bc...` = both slice408 pins; `ipu.cpp` `ff393282...` | Confirmed |
| v2.00, 3,000 frames headless from the start (logos and intro movie), a frame every 25 | 120 of 120 frames byte-identical to the build without the capsule; the IPU counters identical (24.0 M IDEC, 157.6 M qwords out) | Confirmed |
| Wall time of that run | 9.8 s without, 7.4 s with (one run each; -25%) | Confirmed, single run |
| CTest | 271/271 in `linux074-frameskip-build` and `linux074-spec2-build` (`native_ipu_fast` compares each part with the donor's) | Confirmed |
| Spec II video | 9,000 frames headless decode no video (no movie without input); same capsule as v2.00 | Not exercised |

Not yet: the tester watching the videos; the PGO builds (new training needed).
