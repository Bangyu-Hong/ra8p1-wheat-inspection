# RA8P1 1 MiB MRAM 烧录与边界检查

## 三个不能混淆的量

- RA8P1 内部 Code MRAM 的物理容量是 1 MiB，安全地址范围为
  `0x02000000..0x020FFFFF`。
- TrustZone 的 `Code MRAM Secure` 是芯片当前允许安全工程使用的边界。它可以
  小于物理容量；边界为 64 KiB 时，`0x02010000` 以后的默认 J-Link loader
  操作会失败。
- Release 生成的 `camera.rpd` 描述本次构建的安全分区下载信息，不是芯片容量
  证明。当前不足 64 KiB 的内部镜像会生成 `FLASH_CPU0_S_SIZE=0x10000`。

本工程是 CPU0 Flat 安全工程。链接脚本继续以物理上限 `0x02100000` 阻止真实
越界，但下载前还必须确认芯片的安全边界已经是 1024 KiB。

## 防止 F11 再次缩回 64 KiB

`camera.elf.launch` 必须保持：

```xml
<booleanAttribute key="com.renesas.hardwaredebug.arm.jlink.setTZBoundaries" value="false"/>
```

关闭该项后，e² studio 调试下载只使用现有芯片边界，不再按本次 `.rpd` 自动修改
TrustZone。新板或被其他工程改过的板必须先用 RDPM 显式初始化，不能依赖 F11。

## 命令行检查和一次性初始化

RDPM 是 32 位程序时，必须和 32 位 `JLinkARM.dll` 放在同一隔离目录。只读检查：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\check_ra8p1_mram_boundary.ps1
```

脚本先读取专用环境变量 `RA8P1_RDPM_DIR`，未设置时回退到当前开发机的
`C:\Temp\rdpm_ra8p1_jlink962_x86`。换电脑时应设置该环境变量，或用
`-RdpmDirectory` 指向包含 `RenesasDevicePartitionManagerCmd.exe` 和匹配位数
`JLinkARM.dll` 的目录。

只有确认 J-Link 连接的是本项目 RA8P1 板后，才执行一次性扩展：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\check_ra8p1_mram_boundary.ps1 -Apply1024KiB
```

脚本修改后会再次读取状态，并要求 `Code MRAM Secure = 1024 KiB`。该操作只改变
Secure/Non-secure 内存边界，不应改变 DLM、PL 或 AL；验收记录仍须分别核对这些
安全状态。

## 产品烧录验收

1. Release Clean Build，确认链接器 `FLASH` 结束地址不超过 `0x02100000`。
2. 运行边界只读检查，必须得到 `secure_kib=1024`。
3. 用 `R7KA8P1KF_CPU0`、SWD 4000 kHz 加载完整 `Release/camera.srec`，不能只烧
   内部 MRAM；该 SREC 还包含 `0x80000000` 的 OSPI 模型和 command stream。
4. 内部 MRAM 与配置区按 SREC 地址段回读并逐字节比较。
5. OSPI 内存映射窗口只有在固件初始化控制器后才能由 CPU 直接读取。烧录验收
   同时要求 J-Link loader verify 成功，以及冷启动日志中模型源 CRC/SDRAM CRC、
   command 源 CRC/SDRAM CRC 分别一致。
6. 冷启动后检查 SDRAM、NPU numeric gate、OV5640、CEU、LCD 和 USER 键日志。

若 `0x02010000` 擦写超时，先停止重复烧录并检查 RDPM 边界；不要把它误判为
“芯片只有 64 KiB”，也不要用删除代码或把资产挪出 MRAM来掩盖边界错误。
