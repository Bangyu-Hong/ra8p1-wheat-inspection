# 小麦麦穗项目环境与运行指引

本文件说明已有工程的入口与恢复前提。本次未连接开发板，未重新执行产品构建、烧录或实机验收。

## 1. RA8P1 产品主工程

工程入口：[camera](../src/ra8p1/camera/)。使用 e² studio 导入其中的 Eclipse 工程元数据，按原交付环境恢复工程。

| 配置项 | 交付记录 |
|---|---|
| MCU | `R7KA8P1KFLCAC` / CPU0 |
| 核心 | Cortex-M85、Ethos-U55 |
| e² studio | 2026-04.2 |
| FSP | 6.5.0 |
| 编译器 | GNU Arm Embedded 13.2.1 |
| 构建配置 | Release |
| 输入路线 | OV5640、640×480 RGB565、YOLOX tile224 |

导入 `camera` 后，按表中版本配置工具链，选择 Release 构建。修改 `configuration.xml` 时通过 FSP 配置器重新生成内容，再运行工程现有配置检查并构建；使用对应板卡的 J-Link 配置烧录和验证。本次未检查本机工具链，也未重新构建或烧录。

本地 Wi-Fi 文件 `camera/src/esp_at_credentials.h` 未放入整理副本，按 `esp_at_credentials.h.example` 或原工程配置工具生成；该文件被 Git 忽略。

历史固件与烧录资料保存在 [firmware](../materials/ra8p1/firmware/)。原说明标注镜像可能包含本地 Wi-Fi 字符串，因此二进制与烧录辅助文件默认不纳入 Git，公开版本应以演示配置重新构建。使用前核对目标器件和内部 / 外部存储段，本次未重新烧录或验证重建一致性。

## 2. 已有主机测试

测试位于 [camera/tests](../src/ra8p1/camera/tests/)。在工具链配置好后，从 `camera` 目录调用：

```powershell
powershell -File .\tests\run_host_tests.ps1
powershell -File .\tests\run_wheat_host_tests.ps1
```

前者依赖 GCC，后者依赖 Python 与对应科学计算库。脚本保留原环境的回退查找逻辑。本次只检查材料完整性，没有重新运行产品测试；主机测试也不能替代板端数值、语义和田间精度验证。

## 3. 报告附录核心代码

[report_core](../src/ra8p1/report_core/) 的 README 记录板级边界。需实现 `board_port.h` 和 `wheat_model_port.h` 对应适配；它不是开箱即可烧录的 FSP 产品工程。

报告附录与实际 `camera` 主工程的输入、量化和模型契约不同，详见 [版本说明](./VERSION_STATUS.md)。

## 4. 串口屏

用匹配型号的淘晶驰编辑器打开 [WHEAT.HMI](../src/ra8p1/hmi/WHEAT.HMI)，字体和素材保持原目录关系。按 [最终通信协议](./ra8p1/hmi/WHEAT_串口屏最终通信协议_20260819.md) 核对波特率、字段、帧结束符与板端实现。
