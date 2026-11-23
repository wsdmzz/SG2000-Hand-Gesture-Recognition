# B 方案排查指南（Troubleshooting）

> 配套 `b_plan/00_docs/README.md`。当 B 方案在「拉 SDK → 集成 tdl_sdk → 重编固件 → 烧录 → 改写应用」任一环节出问题时，按本指南逐层定位。

---

## 0. 排查顺序总原则

按「链路」从上到下查，先看日志、先确认"是否到达该环节"，再深挖：

```
编译环境 → SDK 拉取 → tdl_sdk 集成 → 固件编译 → 固件烧录 → 应用编译 → 板上推理 → RTSP推流
```

每步先问三个问题：
1. **是否到达这一步**（日志/产物在哪）
2. **这一步的输入是否正确**（路径/宏/库在不在）
3. **这一步的输出是否符合预期**（API 符号/镜像存在/模型能跑）

---

## 1. SDK 拉取相关

| 症状 | 可能原因 | 排查/解决 |
|------|---------|----------|
| `git clone` 慢/失败 | 网络对大仓库慢 | 用浅克隆 `--depth 1` 或国内镜像/代理；SDK 较大（duo-buildroot-sdk-v2 ~GB级） |
| clone 后缺 submodule | buildroot 常用 submodule | `git submodule update --init --recursive`（在 SDK 目录内） |
| tdl_sdk clone 到 `master` | 主分支 | 确认 `git branch -a`；如要稳定版查 tag |

## 2. toolchain（交叉编译工具链）

| 症状 | 排查 |
|------|------|
| gcc 找不到 | 确认路径：`riscv64-unknown-linux-musl-gcc`；现有可复用 `../ai/host-tools/gcc/riscv64-linux-musl-x86_64/bin/` |
| 编译出的二进制 host 跑不了 | 确认用的是交叉工具链而非 x86 gcc |
| 平台宏 | buildroot 编译统一加 `-DCV181X -D__CV181X__`（SG2000 属 CV181x），TDK CMakeLists 用 `CVI_PLATFORM=CV181X` |

## 3. tdl_sdk 集成/编译

| 症状 | 排查 |
|------|------|
| tdl_sdk 库编不出来 | 看 `02_tdl_sdk/` 的 `build_tdl_sdk.sh` / `CMakeLists.txt`，确认平台分支 CV181X；缺少依赖库（cvikernel/cvimath/cviruntime）须先编译 |
| 符号未导出（`TDL_Classification` 找不到） | 编译成 `.so` 后 `nm -D <libcvi_tdl.so> | grep TDL_Classification` 看是否导出 |
| 链接时缺库 | 确认 NEEDED 依赖链（`readelf -d`），把缺的库一并放到链接路径 |
| 与固件 ABI 不符 | B 方案核心：所有库必须**同一次 buildroot 编译**，不能混用独立编译的 .so |

## 4. 固件编译（buildroot）

| 症状 | 排查 |
|------|------|
| 首次编很久/卡住 | 属正常（GB级源码+全量），用 `make -j<N>`；日志在 `06_build/` |
| 编译错误 | 看具体报错在哪层（linux/buildroot/middleware）；补依赖或改配置 |
| 平台/板卡选错 | duo-buildroot-sdk-v2 默认 duo；确认配置文件里 Duo S 相关 defconfig（sg200x） |
| 镜像产出 | 成功后 `.img` 在 SDK 的 `out/`（拷贝到 `07_deploy/`） |

## 5. 烧录新固件（变砖风险）

| 症状 | 排查 |
|------|------|
| 新固件起不来（无 IP/黑屏） | 说明新固件有问题。**回退**：用原厂镜像 `../ai/milkv-duos-musl-riscv64-sd_v2.0.1.img` 重新烧 SD 卡 |
| 烧录工具 | Windows 用 Rufus/Win32DiskImager；Linux 用 `dd if=x.img of=/dev/sdX`（务必选对盘符！） |
| 烧录前 | 先 `md5sum` 校验镜像完整性，再烧 |

## 6. 新固件上分类 API 是否真的可用

```bash
# 板上确认 TDL 库符号
sshpass -p milkv ssh root@192.168.42.1 'nm -D /mnt/system/lib/libcvi_tdl.so | grep -iE "TDL_Classification|TDL_OpenModel" | head'
# 若出现 TDL_* = 新 API 已生效；若只有 CVI_TDL_* = tdl_sdk 未升级成功（回第 3/4 节）
```

## 7. 应用改写（新版 TDL_* API）

参考标准示例：`02_tdl_sdk/sample/c/sample_classification.c`
- 手势分类：`TDL_MODEL_CLS_HAND_GESTURE`（0拳头/1五指/2无/3二）
- 关键 API：`TDL_CreateHandle` / `TDL_OpenModel` / `TDL_ReadImage` / `TDL_Classification` / `TDL_DestroyHandle`
- 输入：`TDLImage`（由 `TDL_ReadImage` 得到），非旧版 `VIDEO_FRAME_INFO_S`

```bash
# 板上确认头文件是否存在（编译应用需要）
sshpass -p milkv ssh root@192.168.42.1 'find / -name "tdl_sdk.h" 2>/dev/null | head'
```

## 8. 板上 RTSP 推流（B 方案成功后）

见 `b_backup/milestone_det/` 旧版程序（`CVI_TDL_*`），改写为 `TDL_*` 后结构不变：
- VI→VPSS→(TDL 推理)→画框→VENC→RTSP
- 排查：先确认 `rtsp://<ip>/h264` 能用 `ffprobe` 探测；再确认推理输出

---

## 9. 回退策略（重要）

- **当前可用状态**：`b_backup/milestone_det/`（手部检测+RTSP 推流，旧固件 `/root/`）+ 原固件镜像 `../ai/milkv-duos-musl-riscv64-sd_v2.0.1.img`
- **B 方案任何一步失败/变砖** → 重烧原固件镜像 → 部署 `b_backup/milestone_det/` 程序 → 回到可用状态
- **别删**：原固件镜像、`b_backup/`、旧版可运行二进制（板上 `/root/sample_vi_gesture_rtsp`）
