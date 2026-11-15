# 可烧录固件说明

本目录保存 2026-08-18 已在实板运行的 M85/CPU0 Release 恢复镜像：

- `camera.srec`：完整烧录文件，包含内部 MRAM 和 OSPI 地址段。
- `camera.elf`：调试符号和程序镜像。
- `camera.map`：链接映射，用于检查各存储区域占用。
- `flash_current_release.ps1`：Windows 命令行烧录入口。
- `flash_camera_release.jlink`：J-Link Commander 命令脚本。

连接 J-Link、目标板供电并关闭 e2 studio 调试会话后，在 PowerShell 执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\flash_current_release.ps1
```

脚本会使用 `R7KA8P1KF_CPU0`、SWD 4000 kHz 烧录并复位运行。烧录前先用 `SHA256SUMS.txt`
验证文件；烧录后仍需观察 LCD、调试串口和 USER 按键，不能只凭命令退出码判断实板正常。

注意：该镜像可能包含构建时的本地 Wi-Fi 字符串，不应公开发布到互联网。交给固定队友联调可以，
公开版本应使用演示凭据重新构建。
