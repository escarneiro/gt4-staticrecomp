# TASK-074 L2: the SPU2 sound capsule on Linux (TASK-125, slice396)

The release's SPU2 capsule (`scripts/prepare_spu2_runtime.py`; pinned by `spu2_capsule_sha256` in `slice408-pcsx2.json`
and `slice408-spec2.json`, the hash of its `runtime.json`, which pins `spu2.cpp`) now builds into the Linux game, both games.

**What it fixes (issue #104):** the donor's `Envelope::step` truncated `delta * level / 0x8000` toward zero, so a quiet
released voice stopped decreasing above zero and was never turned off: in GT Auto the oil change and car wash bubbles
played on until the game was closed. The derived `iop/spu2.cpp` takes that step from `native/spu2_envelope.hpp`, so the
decrease reaches silence and the voice ends, as on the PS2.

**Build:** `cmake/LinuxHost.cmake` `-DGT4_LINUX_SPU2_RUNTIME=<capsule>`: the receipt must equal the release pin and
`spu2.cpp` its receipt; it replaces the donor `iop/spu2.cpp` in the game runtime (the L1 headless smoke runner keeps the
donor file, by design). The capsule comes from `scripts/linux/v200-inputs.sh` (`<prefix>-spu2-runtime`), one for both games.

**Checks (2026-11-23):** the capsule equals the pin (receipt `554308af...`, `spu2.cpp` `7192d079...`); CTest 271/271 in
`private/linux074-frameskip-build` and `private/linux074-spec2-build` (`native_spu2_envelope` tests the step itself); a
windowed v2.00 start with sound: queue median 2,878 frames for a 2,880 target, 0 underruns, 0 late frames. Not yet: the
tester hearing the bubbles stop in GT Auto; the PGO builds (new training needed).
