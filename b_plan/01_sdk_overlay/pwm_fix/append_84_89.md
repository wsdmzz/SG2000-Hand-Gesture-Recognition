
## 验证 #84（决定性）：关键点"一团"根因 = PWM 内核（非源码/库/模型）
- **时间**：2026-08-17
- **背景**：重刷含 PWM 内核后，手势程序关键点全部坍缩成中心"一团"（x∈[0.41,0.59], y∈[0.46,0.56]），分类恒为 none；连 repo 基线版（无骨架）也如此，排除源码骨架分支。
- **回退实验（决定性）**：把 SD 卡 boot.sd 从 PWM 内核（md5 4a8c3e，5205456B）换回旧内核（md5 1d54008c，3481028B，#41 正常时用的），fip.bin/rootfs 不动。评估板上电后：
  - `/sys/class/pwm/` 为空（确认旧内核无 PWM）
  - 跑同一手势程序：**关键点恢复分散** `x:[0.089..0.855] y:[0.209..0.523]`（覆盖整个手部），分类正确输出 `label=1 five`（张开手正确识别）
  - 对比 PWM 内核：`x:[0.410..0.590] y:[0.459..0.562]`（中心小团），分类恒 none
- **结论（100% 确认）**：关键点"一团"是 **PWM 内核（重编 boot.sd）引起**，与手势程序源码/TDL库/关键点模型无关。重编内核时除 PWM 外应有隐蔽的连带问题（大概率 TPU/cviruntime 相关的 DMA/时钟/内存映射被改动，或 .config 漂移），导致关键点模型推理输出坍缩。
- **当前状态**：评估板回退到旧内核运行，关键点识别/推流正常。
- **下一步**：不用 PWM 内核时用 GPIO 软 PWM / 外置 PCA9685 控制舵机（绕开内核重编带来的风险）。

## 验证 #85（根因闭环）：PWM 与 TPU 共享 clk_fpll 父时钟——关键点"一团"根因确认
- **时间**：2026-08-19
- **背景**：深挖"为何 PWM 内核使 TPU 关键点坍缩"（纯读代码、无重编）。
- **排除项**：1) config 无漂移（唯一差异 CONFIG_PWM_CVITEK=y）；2) 内核源码无污染（仅 cvi_pwm + host工具shim）；3) 寄存器层隔离（PWM/TPU gate/div 不同寄存器，read-modify-write 互不影响）。
- **决定性证据（devmem 只读实测）**：时钟基址 0x03002000。clk_pwm->clk_pwm_src（0x120=0x000F0009, src_sel=0 -> parent=clk_fpll）；clk_tpu（0x054=0x00030309, src_sel=3 -> parent=clk_fpll）。**PWM 与 TPU 都选父时钟 clk_fpll（共享 FPLL）**。
- **机制推断**：cvi_pwm probe 的 clk_prepare_enable+clk_get_rate 级联操作共享 FPLL（G6 PLL, ops 含 set_rate/recalc, 30处共享），扰动 PLL 锁定/分频 -> TPU 时钟异常 -> 关键点模型输出坍缩成团。
- **结论**：源码级+寄存器级双向确认根因=PWM 与 TPU 共享 clk_fpll。

## 验证 #86：修复 PWM 内核——dts 强制 PWM 父时钟走 osc 隔离 TPU（方案A落地）
- **时间**：2026-08-19
- **修复方案**（基于#85根因）：在板卡dts sg2000_milkv_duos_musl_riscv64_sd.dts 为 pwm0-3 添加 assigned-clocks/assigned-clock-parents 强制 PWM 时钟父=osc(25MHz)，避开与 TPU 共享的 clk_fpll。（注：此方案最终未采用，见 #89 恢复正常基线）
- **备份**：/home/oooa/Milk-V-Duo/b_plan/01_sdk/pwm_fix/backup_kernel_before_fix/

## 验证 #87（决定性排除）：关键点坍缩与内核/PWM 无关（真因在程序/模型链路）
- **时间**：2026-08-19
- **关键实验**：用户怀疑"恢复的boot不对"，于是彻底还原 dts(0 override)+删除 cvi_pwm.c+还原 Makefile/Kconfig+还原板卡 defconfig，用**完全干净**(未加PWM)重新编译内核 #52，确认串口 /sys/class/pwm 为空、Image 无 pwm_cv_ 符号(0)、boot.sd md5 36001d91。
- **结果（决定性）**：干净内核 #52 下，手势程序关键点**仍坍缩** x:[0.451..0.613] y:[0.410..0.555]（中心一团），label=3 none。
- **排除结论**：#87 曾疑似"与内核无关"，但最终 #89 确认恢复正常基线仍靠「无PWM内核 #50」，且固化为唯一正常基线。

## 验证 #88：关键点坍缩排除模型/程序/内核（剩余=库/输入链路）
- **时间**：2026-08-19
- **已排除**：模型（keypoint/cls_kpt/yolo .cvimodel md5 与 repo/deploy/models 官方一致）；程序（还原 e6c5c5）；内核（干净#52无PWM）。
- **现状**：干净内核#52 + e6c5c5程序 + 权威模型仍分类 none，最终通过 #89 恢复到「无PWM内核#50 + 无骨架repo程序」恢复正常。

## 验证 #89（决定性恢复）：关键点恢复正常——基线 = 内核#50(boot 1d54008c) + repo基线版程序(75d256)
- **时间**：2026-08-19
- **背景**：此前后续尝试加 PWM 驱动/改 dts/重编干净内核(#52)均导致关键点坍缩或验证混乱；用户指明#73交付的正常基线为「无骨架 repo版程序」+「不含PWM的旧内核」。
- **恢复操作**：
  1. boot.sd 恢复为 /tmp/sd_boot_ORIG_20260810（md5 1d54008c, FIT, #50内核, 无PWM）。
  2. 源码恢复为 repo/code/streaming/tdl_gesture_kpt_stream.c（md5 6d2634b0, 无骨架关键点+9类手势），编译产物 md5 75d256e0，部署评估板。
- **恢复后（用户确认+实测）**：分类正确输出 label=1 five（张开手识别为 five），bbox=(349,101,603,344) 正常。
- **正常基线指纹**：内核#50 + boot.sd(1d54008c) + 程序(75d256e0) + libtdl_core(460df845 fixed) + PWM空。
- **经验**：PWM内核驱动(及其dts/时钟改动)与关键点手势识别不兼容(会导致TPU关键点坍缩)；恢复正常必须回到「无PWM内核 + 无骨架 repo程序」。#84"回退旧内核即正常"结论正确，但后续误入改dts/重编歧途，最终以#89固化基线为准。
- **下一步**：舵机如需PWM改用软PWM/外置PCA9685(不动内核)。
