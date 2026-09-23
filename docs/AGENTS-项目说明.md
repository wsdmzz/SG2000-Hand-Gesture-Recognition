# AGENTS.md — Milk-V Duo (SG2000) Gesture Recognition

## Project Purpose

Deploy a **custom-trained hand-pose gesture model** (7 classes: fist, 1–5, thumbs-up)
to a Milk-V Duo (SG2000, RISC-V CV181x) driving a 5-finger servo hand via PCA9685 (I2C).

Pipeline: PyTorch (YOLOv8-pose, 21 keypoints + 7 classes) → ONNX → TPU-MLIR cvimodel
→ on-board realtime inference → `FA/SA` finger angles → `G <id>` pipe → `pca9685_servo`
→ MJPEG stream (:8881).

**Final product form (stated by user 2026-09-23)**: the device is **wall-mounted** and
will be used by **many different people** who walk up to it. This is the key constraint
driving the current work — see "Current State".

Boards: `192.168.50.1` (wired, currently used) / `192.168.42.1` (USB-NCM), root/milkv,
V2.0.1 firmware (musl), GC2083 MIPI camera. Remote:
`sshpass -p milkv ssh root@192.168.50.1 '...'`

**Scp does not work** (board has no `/usr/libexec/sftp-server`; both `scp` and `scp -O`
fail). Use a pipe: `cat file | ssh root@ip 'cat > /path'`.

## Current State (verified 2026-09-23)

### Working

- **Full realtime pipeline runs**: `tdl_gesture_kpt_stream` (VI/VPSS → BGR → ROI crop →
  letterbox 480 → INT8 cvimodel → DFL decode → 21 kpts → `FA` angles → `G <id>`) piped to
  `pca9685_servo --pipe follow` → PCA9685 (0x40 @ /dev/i2c-4, 50 Hz) → 5 servos.
- **Close range works**: hand inside the ROI, `bsc` 0.78–0.83, `G` lines emitted, servos
  track. Verified for G0/G1/G2/G3/G5.
- Chain of fixes that made this work (all verified, see log #116–#127):
  1. **#116 input quantisation bug**: `kf=1/(255*qscale)` quantised whole frames to **0**.
     Correct is `kf=qscale/255`. This alone explained "抖而不跟随".
  2. **#123 fixed-ROI crop**: full-frame inference gave `bsc≈0.005`; cropping to a
     hand-sized window gives `bsc≈0.92`.
  3. **#125 rotation alignment**: model only saw "arm left→right / fingers pointing right".
     `--rot 0|90|270` rotates the ROI to the training orientation (coords are
     back-mapped to full frame).
  4. **#126 model swap**: rotation-augmented retrain `best_rot.onnx` → 480 cvimodel
     (see "640→480 conversion" below).
  5. **#127 adaptive ROI**: multi-scale ROI `{280,170,100}` with last-frame memory.

### Not working / open

- **Far distance fails**: at distance, `roi=100` yields `bsc≈0.0000`; `[ROI] 切换` never
  fires. Root cause is **scene mismatch, not model capacity**:
  - Training data: single person, close, hand centred, single lighting.
  - Real use: **wall-mounted, many people, far, arbitrary hand position/lighting**.
  - Fixed-centre ROI cannot track both size *and* position changes; small ROI tolerates
    only ±30 px position error, while the TDL hand detector's box is off by 83 px.
- Rejected alternatives (evaluated in #127/#128, do not retry):
  - Position×scale full scan (3×3×3) → 27 inferences/frame ≈ 0.3 fps.
  - TDL hand detector for localisation → box offset 83 px, too coarse for small ROI.
  - 640×640 input → only 1.33× linear gain, costs 1.78× latency.

**Next step is retraining on data captured in the real scene** (wall-mounted, multiple
people, multiple distances/positions/lighting). Collection needs several people so it
will take a while. Collection checklist is in log **#128 §五**.

## Reusable Assets (built during this work)

- `tdl_gesture_kpt_stream.c` + `build_tdl_gesture_kpt_stream.sh` — main realtime program.
  CLI: `./tdl_gesture_kpt_stream <port> [roi_cx roi_cy roi_side] [--rot 0|90|270] [--dump]`
  (`--dump` writes `/tmp/roi_*.jpg` + `/tmp/lb_*.jpg` every 20 frames = the exact image
  fed to the model; extremely useful for offline diagnosis).
- `make_rot_480_full.py` — **YOLOv8-pose 640→480 conversion**. Rebuilds the 5 grid/stride
  constants (`Constant_264/266/284` grid, `Constant_272/286` stride) + 2 hard-coded
  Reshape anchors + graph in/out shapes + Slice steps. Validated semantically.
  **So 640 ONNX exports no longer need re-exporting from the training side.**
- `make_rot_raw2.py` — exposes `det_raw`/`kpt_raw` (tensors `440`/`372`) as graph outputs.
- `build_rot480_cvimodel.sh` — full TPU-MLIR conversion. **`model_deploy.py` uses
  `--model`, there is no `-o`.**
- `tdl_bbox_diag.c` — hand-detector bbox diagnostics (`off`/`live`/`cascade`).
- `kpt_model_probe.c` + `build_probe.sh` — camera-free model load/forward/layout probe.
- `analyze_run_log.py` — parses run logs (bsc distribution, FA/GEST stats, per-class).
- `test_distance_roi.py`, `test_tight_crop_by_class.py` — scale/distance response studies.
- `pca9685_servo` — 5-channel servo driver. `ch_map={0,1,2,3,4}` (s1→CH0 … s5→CH4),
  `pulse_max_us={889,1500,1500,1500,1333}`. `--pipe [follow|imitate|rock]`.

**一次性探索脚本（保留作参考，非工具）**：`verify_grid_logic.py` / `list_8400_consts.py` /
`diag_add285.py` / `analyze_640_consts.py` / `make_rot_480.py`（旧版，已被
`make_rot_480_full.py` 取代）/ `make_rot_raw.py` / `check_rot_onnx.py` /
`chk_dump.py` / `draw_roi_guide.py` / `_*.sh`（板上临时测试）/ `append_log_*.py`
（日志追加脚本）。`models/probe_in_*.bin|npy` 是量化验证的临时产物，可删。

## How to Build

**Toolchain** (musl, RISC-V c906):
```
AI=/home/oooa/Milk-V-Duo/ai
TC=$AI/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-
```
Main build: `bash build_tdl_gesture_kpt_stream.sh` (g++ for the main file, gcc for
`sample_common_*.c`; links firmware-matched libs). Do **not** `source envsetup.sh` in the
duo-tdl-examples tree — it blocks on interactive `read` prompts and checks `host-tools/.git`.

**TPU-MLIR conversion** runs in podman (`sophgo/tpuc_dev:v3.1`) with the host install at
`ai/tpu-mlir` (v1.3.228 — matches the container's `mlir_core`; `ai/tpu-mlir-master`
v1.28.1 has no prebuilt binaries). **Must pass `--security-opt label=disable`** (SELinux
Enforcing; `:z` fails on the large image files in this repo).

## Evaluation-Board Runtime

```
LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd
```
- TDL stack + OpenCV: `/mnt/system/lib/`; media framework: `/mnt/system/usr/lib/`;
  `libini.so`: `/mnt/system/usr/lib/3rd/`
- Gesture model path in code: `/mnt/data/models/cv181x/best480_int8_cv181x.cvimodel`
  (currently the rotation-augmented build; old one backed up as `.bak`)
- Sensor cfg `/mnt/data/sensor_cfg.ini` → GC2083 (bus 3, i2c 0x37, mipi 0)
- Run: `cd /root && ./tdl_gesture_kpt_stream 8881 2>&1 | tee /tmp/gest.log \
  | /root/pca9685_servo --pipe follow`  (stream: `http://192.168.50.1:8881/`,
  **single client only** — close other tabs first)

## Key Files

- `SG2000手势识别验证日志.md` — **append-only** verification log (#0…#128). Append a new
  `验证 #N` section (操作/结果/结论/下一步) after every experiment.
- `tdl_gesture_kpt_stream.c` — main program (ROI crop, rotation, adaptive scale, pipeline).
- `HANDOFF_交接文档.md` — handover doc (see also its 2026-09-23 addendum).
- `models/` — `best.onnx` (640), `best_rot.onnx` (rotation-augmented), `best_rot_raw_fixed.onnx`
  (480 raw), `best_rot_480_int8_cv181x.cvimodel`.
- `ai/tpu-mlir-master/tmp/bestrot480/` — conversion workspace (mlir / cali_table / cvimodel).

## Conventions & Pitfalls

- **Language**: natural-language docs and code comments in Simplified Chinese.
- Every tool action on the board goes into the verification log before the next step.
- **TDL/VI repeated init corrupts state**: after several program start/stops,
  `TDL_ReadImage` segfaults, then `VI_INIT 0xffffffff` + `ioctl SYS_ION_ALLOC failed`
  (ION pool, not general memory — `MemFree` stays ~260 MB). **Reboot the board to recover.**
  Long single-instance runs are unaffected.
- `model_transform.py` (v1.3.228) needs `--mlir`; `--pad_value` is single-valued.
  `model_deploy.py` uses `--chip` and `--model` (no `-o`, no `--processor`).
- TPU-MLIR v1.3.228 crashes on 4-input `Slice` (steps omitted) →
  `SliceAxisOp ... got 'none'`. Fix: append explicit `steps=[1]` (see
  `fix_onnx_slice_steps.py`).
- Only **one** `.c` per sample dir in duo-tdl-examples (`$(wildcard *.c)`).
- musl + C906: atomic ops must come from `libatomic`/`atomic_shim.so`.
- Board has no readelf/nm/A-ext; use the host cross toolchain.
- `0xC001010x` legacy TDL errors (old notes, #0–#50 era): `0x02`=open fail,
  `0x0B`=NOT_YET_INITIALIZED, `0x09`=VPSS_GET_FRAME (`CVI_TDL_HandClassification` dead end —
  the official sample silently ignores this code).
