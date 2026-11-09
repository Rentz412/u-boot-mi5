# U-Boot for Xiaomi Mi 5 (gemini)

面向小米 5（gemini / MSM8996）的二阶段 U-Boot 移植。支持直接从手机 `userdata` 分区的 ext4 文件系统中读取内核、initramfs 和 DTB，并通过标准的 `extlinux.conf` 引导主线/类主线 Linux。

最大的好处是**换内核不用再反复重打包、重刷 `boot.img`**：U-Boot 本身只作为引导器打包进 `boot.img`，平时升级内核或改启动参数，直接在手机的 Linux 系统里改 `/boot` 下的文件就行。

## 当前进度

- **JDI 屏幕机型**：已于 2026-11-09 实机 `fastboot boot` 测试通过，可正常从 ext4 分区加载配置并引导进入 Ubuntu（测试内核为 `6.3.1-gemini-rentz-v2.6`）。
- **LGD 屏幕机型**：镜像已编译并通过静态检查，暂未上机实测。
- **架构状态**：这是一个由原厂 `aboot`（LK）链式加载的二阶段 U-Boot，**不替代 XBL 或原厂 bootloader**。屏幕（MDP）、UFS/UniPro 和 USB/PHY 直接复用原厂 bootloader 初始化好的硬件状态；extlinux 解析与 `booti` 跳转使用 U-Boot 上游实现，UFS 读取、屏幕日志输出和 DTB 修补则由板级代码（`gemini_boot`）负责。
- **刷入方式**：目前仅通过 `fastboot boot` 临时引导验证，尚未测试直接刷写到 `boot` 分区常驻。

> **注意：** 目前尚未实现屏幕型号自动识别，JDI 与 LGD 面板需要分别使用对应的镜像。

## 引导流程

```text
原厂 aboot (LK) ──> U-Boot (boot.img) ──> 读取 ext4 分区 /boot/extlinux/extlinux.conf
                                      ──> 加载 kernel + initramfs + DTB ──> 启动 Linux
```

为了让原厂 `aboot` 能识别并加载，U-Boot 本体仍然需要用 `mkbootimg` 封装成旧版（v0）Android `boot.img` 格式。但这个镜像里只装了 U-Boot，不包含 Linux 内核和 initramfs。

## 编译与打包

在 Linux（或 WSL）下准备好 AArch64 交叉编译工具链和 U-Boot 编译依赖（实测环境：GCC 13.3.0 + binutils 2.42）：

```sh
git clone https://github.com/Rentz412/u-boot-mi5.git
cd u-boot-mi5
make O=../build-mi5 CROSS_COMPILE=aarch64-linux-gnu- qcom_gemini_defconfig
make O=../build-mi5 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) u-boot-nodtb.bin
```

编译完成后，需要配合当前手机内核对应的 JDI/LGD DTB 文件以及 AOSP 的 `mkbootimg.py`，把 U-Boot 打包成可引导的 `boot.img`：

```sh
python3 board/qualcomm/gemini/mkimage.py \
  --uboot ../build-mi5/u-boot-nodtb.bin \
  --jdi-dtb /path/to/msm8996-xiaomi-gemini-jdi.dtb \
  --lgd-dtb /path/to/msm8996-xiaomi-gemini-lgd.dtb \
  --mkbootimg /path/to/mkbootimg.py \
  --output ../images-mi5
```

> **说明：** 本仓库仅包含 U-Boot 源码。Linux 内核、initramfs、`mkbootimg.py` 以及两份面板 DTB 需自行准备。请务必使用与你当前内核/固件匹配的适配 DTB，主线原版 DTB 未必能直接点亮。

## 配置与实机引导

1. 参考 [extlinux.conf.example](board/qualcomm/gemini/extlinux.conf.example)，在手机 ext4 分区创建 `/boot/extlinux/extlinux.conf`，按实际情况修改内核、initramfs 路径以及 rootfs 的 UUID。
2. 将对应面板的 DTB（`msm8996-xiaomi-gemini-jdi.dtb` 或 `msm8996-xiaomi-gemini-lgd.dtb`）放入手机的 `/boot/gemini/` 目录（对应配置中的 `FDTDIR /boot/gemini`）。
3. 手机重启到 Fastboot 模式，临时引导对应屏幕的镜像（以 JDI 为例）：

```sh
fastboot boot ../images-mi5/u-boot-mi5-jdi.img
```

加载后屏幕上会直接打印 U-Boot 引导日志，并自动按 `extlinux.conf` 启动 Linux。当前配置未开启 USB 串口/终端交互；如果引导失败，U-Boot 会在停留约 30 秒后自动重启手机。

## 已知限制与注意事项

- **文件系统要求**：ext4 仅支持只读挂载，不支持写盘和日志回放（journal replay）。因此文件系统必须处于正常卸载的干净状态，块大小需为 4 KiB，且不能开启 U-Boot 不支持的 ext4 新特性。
- **配置限制**：所有引导文件必须放在 `/boot` 目录下；暂不支持 `INCLUDE` 指令、设备树插件（DT overlays）和自定义启动 Logo。
- **体积与内存上限**：
  - 未压缩的 ARM64 `Image` 内核最大支持 **128 MiB**；若使用 gzip 压缩（`Image.gz`），受限于上游 `booti` 10 倍解压预估策略，压缩包不能超过 **12 MiB**。
  - `initramfs` 最大 **128 MiB**，DTB 最大 **2 MiB**。
  - 单次启动的 UFS 累计读取上限为 128 MiB（超时时间 120 秒）。
- **板级实现限制**：目前硬件外设完全依赖原厂固件初始化后的遗留状态，不支持冷启动重初始化；引导逻辑走专用的 `gemini_boot`，尚未对接 U-Boot 通用的 `bootflow scan`。

更多底层细节可参考 [板级文档 (board/qualcomm/gemini/README.md)](board/qualcomm/gemini/README.md)，U-Boot 上游原始说明保留在 [README](README)。

## 致谢与许可证

本项目基于上游 [U-Boot](https://github.com/u-boot/u-boot)（基线 commit `934995f642a21ca7f35fa9aed5f061394fcba243`）开发，完整保留上游提交历史与开源许可。具体文件授权以源码内的 SPDX 标识及 [Licenses](Licenses) 目录为准。
