# AGENTS.md — Milk-V Duo (SG2000) Gesture Recognition

## Project Purpose

Deploy a **custom-trained gesture classifier** (7 classes: fist, 1-5, thumbs-up) to a
Milk-V Duo Module 01 (SG2000, RISC-V CV181x, 512MB). Full pipeline: PyTorch training →
ONNX → TPU-MLIR cvimodel → on-board real-time inference → RTSP stream. The classification
channels on the evaluation board are **not working** for custom models; only the official
face-detection realtime path runs. This repo documents the effort and the working/dying paths.

Evaluation board: DuoS, `192.168.42.1` (USB-NCM), root/milkv, V2.0.1 firmware (musl),
GC2083 MIPI camera attached.

## How to Build

**Toolchain** (musl, RISC-V c906):
```
AI=/home/oooa/Milk-V-Duo/ai
PREFIX=$AI/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-
```

**duo-tdl-examples samples** (do NOT `source envsetup.sh` — it blocks on interactive
product/arch `read` prompts; set env manually):
```bash
cd $AI/duo-tdl-examples
export TOOLCHAIN_PREFIX="$PWD/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-"
export CC="${TOOLCHAIN_PREFIX}gcc"
export CFLAGS="-mcpu=c906fdv -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d -O3 -DNDEBUG -I$PWD/include/system -I$PWD/include/tdl"
export LDFLAGS="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -L$PWD/libs/system/musl_riscv64 -L$PWD/libs/tdl/cv181x_riscv64"
export CHIP=CV181X
export COMMON_DIR=$PWD/common
make -C sample_vi_fd   # NOTE: only *one* .c per dir — Makefile uses $(wildcard *.c)
```
- `sample_vi_fd.c` must NOT coexist with another source defining `main` (saw duplicate
  `main`/`run_tdl_thread` when `sample_vi_hand.c` was present). Rename stale `*.c`/delete `*.o`.
- `envsetup.sh` checks `host-tools/.git` and aborts; bypass it.

**test_img_hand_cls.c** (standalone): compiled via `build_hand_cls.sh` (duo-lib subset) or
`build_hand_cls_full.sh` (full firmware NEEDED chain + `atomic_shim.so`). Full chain works
because `atomic_shim.so` exports the `__sync_*`/`__atomic_*` symbols that firmware libs
(`libisp.so`, `libsys.so`) reference (C906 has no A ext; musl libgcc provides them hidden).

## Evaluation-Board Runtime

Libraries are split across two firmware dirs — both must be on the library path:
```
LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd
```
- TDL stack + OpenCV: `/mnt/system/lib/` (libcvi_tdl.so, libcvikernel.so, ...)
- Media framework:  `/mnt/system/usr/lib/` (libsys, libvi, libvo, libvpss, libgdc, libisp, ...)
- `libini.so`:      `/mnt/system/usr/lib/3rd/`
- Official models:  `/mnt/cvimodel/scrfd_768_432_int8_1x.cvimodel`; sensor cfg:
  `/mnt/data/sensor_cfg.ini` → GC2083 (bus 3, i2c 0x37, mipi 0)
- Remote run: `sshpass -p milkv ssh root@192.168.42.1 '...'`

## Current State (verified 2026-08-04)

- **`CVI_TDL_HandClassification` is effectively dead on this firmware**: returns
  `0xC0010109` (`VPSS_GET_FRAME`). The official `sample_img_hand_cls` does **not check the
  return value** (confirmed by objdump at 0x11070), reads `meta.info[0].name` and prints
  "(空)". It silently swallows the same 0109. Byte-for-byte strace comparison (same
  libcvi_tdl.so, same devices, same VPSS ioctls) showed no initialization difference.
- **`CVI_TDL_SUPPORTED_MODEL_IMAGE_CLASSIFICATION`**: OpenModel OK, inference
  `0xC001010B` (`NOT_YET_INITIALIZED`) regardless of SkipVpssPreprocess / SetPreParam /
  CreateHandle vs CreateHandle2. `RAW_IMAGE_CLASSIFICATION`: `0xC0010102` (open fail).
- **Only the realtime face-detection path runs**: `sample_vi_fd` (VI→VPSS Grp0 Chn1 →
  `CVI_TDL_FaceDetection` → draw → RTSP). It calls `CVI_TDL_CreateHandle2(...,1,0)` +
  `CVI_TDL_SetVBPool(tdl,0,2)` + `CVI_VPSS_GetChnFrame(0,VPSS_CHN1,...)` — this
  VI/VPSS+VBPool binding is the key framework setup the standalone classifier lacked.
- **`sample_vi_fd` ran but `SAMPLE_TDL_Get_VI_Config` reports `s32WorkingViNum<=0`**
  (sensor parses, VI count not filled) when run with firmware libs — ABI/version drift
  between duo-tdl-examples local libs and firmware `/mnt/system/usr/lib/*.so`.

## The A-Plan (current direction)

Reuse the working face-detection realtime pipeline (A) to carry the gesture classifier:
1. Get `sample_vi_fd` running with firmware libs (fix `s32WorkingViNum<=0` — likely needs
   `SAMPLE_COMM_VI_IniToViCfg` from firmware-matching `libsample.so`, or avoid
   `SAMPLE_TDL_Get_VI_Config` and init VI/VPSS directly).
2. Swap the detection call for the gesture classifier on the same VPSS Chn1 frame.

## Key Files

- `SG2000手势识别验证日志.md` — **append-only** verification log (#0..#9). Update after
  every experiment. Must append a new "验证 #N" section (操作/结果/结论/下一步).
- `SG2000手势识别实现路径与问题咨询.md` — overall path + gap analysis (gaps A/B/C).
- `test_img_hand_cls.c`, `sample_vi_fd.c` — classifier / realtime pipeline code.
- `build_hand_cls.sh` (duo-lib), `build_hand_cls_full.sh` (full chain), `atomic_shim.c`.
- `train_gesture_sg2000.py` — PyTorch training (128×128, MobileNetV2, scale `x/127.5-1`).
- `fix_onnx_reducemean.py` — ONNX fixes (Conv kernel_shape, ReduceMean axes).
- `ai/tpu-mlir-master/` — TPU-MLIR v1.28.1 (sophgo); `ai/models/` — converted cvimodels.

## Conventions & Pitfalls

- **Language/regs**: natural-language docs in Simplified Chinese; code comments Chinese.
- Every tool action on the board must be recorded in the verification log before next step.
- `envsetup.sh` interactive `read` blocks automation — never `source` it in scripts.
- Firmware libs differ from repo libs (ABI drift: libsys.so/libsample.so SHA differs). When
  low-level behavior mismatches, prefer firmware `/mnt/system/*.so` as ground truth.
- musl + C906: atomic ops must be provided by `libatomic`/`atomic_shim.so`, not libgcc hidden.
- `0xC001010x` errors: `0x02`=open-model fail, `0x0B`=NOT_YET_INITIALIZED,
  `0x09`=`0xC0010109`=VPSS_GET_FRAME (HandClassification dead end).
- Board has no A-ext/`libvpu`/readelf; use host cross toolchain for readelf/nm/objdump.
