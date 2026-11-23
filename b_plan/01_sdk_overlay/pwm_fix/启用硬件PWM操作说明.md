# Duo Module 01 启用硬件 PWM（驱动 cvi_pwm 集成）

> 根因：固件内核源码集成时漏挂 CVITEK 的 PWM 驱动实现。`linux_5.10/drivers/pwm/`
> 只有 `cvi_pwm.h`，没有 `cvi_pwm.c`，Makefile/Kconfig 无相关条目，导致 dts 里的
> `pwm@3060000~3063000` 节点匹配不到任何驱动 → /sys/class/pwm 空、无 pwmchip。
>
> 已在本工作区 SDK (`b_plan/01_sdk/sophpi/linux_5.10`) 完成全部改动并通过单目标编译验证
> (`cvi_pwm.o`, MAKE_EXIT=0)。以下改动对应到你自己的 SDK 环境时照搬即可。

## 需要改动的 3 个文件 + 1 个 defconfig

### 1. 复制驱动源码到内核驱动目录
```bash
cp <你SDK>/osdrv/interdrv/pwm/cv181x/cvi_pwm.c \
   <你SDK>/linux_5.10/drivers/pwm/cvi_pwm.c
# 头文件 cvi_pwm.h 已在 drivers/pwm/ 下（若无一并复制）
```

### 2. drivers/pwm/Makefile —— 追加一行
```make
obj-$(CONFIG_PWM_CVITEK)	+= cvi_pwm.o
```

### 3. drivers/pwm/Kconfig —— 追加（放在文件末尾）
```kconfig
config PWM_CVITEK
	tristate "CVITEK PWM support"
	depends on ARCH_CVITEK
	help
	  This enables support for the CVITEK PWM controller (cv181x/cv180x).
	  Provides 4 channels per controller via sysfs /sys/class/pwm/pwmchip*.
```
> 注意：不要在前面的 `endif`（闭合 menuconfig PWM 的）外再加多余 endif。

### 4. 内核 defconfig —— 追加 2 行
Duo Module 01 实际使用的内核 defconfig：
`build/boards/cv180x/cv1800b_milkv_duo_musl_riscv64_sd/linux/cvitek_cv1800b_milkv_duo_musl_riscv64_sd_defconfig`
```kconfig
CONFIG_PWM=y
CONFIG_PWM_SYSFS=y
CONFIG_PWM_CVITEK=y
```

## 编译 & 烧录
1. 用你的 SDK `build.sh` 选 Duo Module 01 配置（cv1800b_milkv_duo_musl_riscv64_sd）重编内核+dtb。
2. 生成 dtb/fip/upgrade 镜像，刷入 SD 卡（保留原固件便于回退）。
3. 上电后验证：`ls /sys/class/pwm/` 应看到 `pwmchipN`。

## 验证
```bash
# 每个 pwmchipN 含 4 通道，npwm=4
ls /sys/class/pwm/
cat /sys/class/pwm/pwmchip*/npwm   # 应各显示 4
```

## 本工作区已做（可直接引用）
- 驱动源码已复制、Makefile/Kconfig/defconfig 已改、单目标编译通过。
- 改动文件列表（git 可见）：
  - `b_plan/01_sdk/sophpi/linux_5.10/drivers/pwm/cvi_pwm.c`（新增，来自 osdrv）
  - `b_plan/01_sdk/sophpi/linux_5.10/drivers/pwm/Makefile`（追加 1 行）
  - `b_plan/01_sdk/sophpi/linux_5.10/drivers/pwm/Kconfig`（追加 PWM_CVITEK）
  - `b_plan/01_sdk/build/boards/cv180x/cv1800b_milkv_duo_musl_riscv64_sd/linux/cvitek_cv1800b_milkv_duo_musl_riscv64_sd_defconfig`（+PWM_SYSFS/CVITEK）
  - `b_plan/01_sdk/sophpi/linux_5.10/build/sg2000_milkv_duos_musl_riscv64_sd/.config`（duos 验证用）
