# 源码与工程说明

[项目首页](../README.md) · [技术文档](../技术文档.pdf) · [项目答辩](../项目答辩.pptx)

## 工程目录

| 目录 | 内容 | 入口 |
|---|---|---|
| [camera](./camera/) | RA8P1 产品主工程 | [configuration.xml](./camera/configuration.xml)、[app.c](./camera/src/app.c) |
| [camera/tools](./camera/tools/) | 模型部署、量化与数值检查 | [wheat_deployment](./camera/tools/wheat_deployment/) |
| [camera/tests](./camera/tests/) | 原工程已有主机测试 | [C 测试脚本](./camera/tests/run_host_tests.ps1)、[模型测试脚本](./camera/tests/run_wheat_host_tests.ps1) |
| [hmi](./hmi/) | 串口屏工程、字体与素材 | [WHEAT.HMI](./hmi/WHEAT.HMI)、[通信协议](./hmi/通信协议.md) |
| [report_core](./report_core/) | 报告附录 C99 核心代码 | [适配说明](./report_core/README.md) |

工程内部目录保持原样。`camera` 为产品工程，`report_core` 为报告附录，两者分别保留。

## 环境与运行

使用 e² studio 导入 `camera` 中的 Eclipse 工程元数据，配置对应工具链并选择 Release 构建。

| 项目 | 保存工程的环境记录 |
|---|---|
| MCU | RA8P1 `R7KA8P1KFLCAC` / CPU0 |
| 核心 | Cortex-M85、Ethos-U55 |
| e² studio | 2026-04.2 |
| FSP | 6.5.0 |
| 编译器 | GNU Arm Embedded 13.2.1 |
| 视觉输入 | OV5640、640×480 RGB565、YOLOX tile224 |

修改 `configuration.xml` 后通过 FSP 配置器生成工程内容，再运行配置检查并构建。Wi-Fi 配置按 `camera/src/esp_at_credentials.h.example` 生成本地文件；真实配置被 Git 忽略。

已有测试从 `camera` 目录调用：

```powershell
powershell -File .\tests\run_host_tests.ps1
powershell -File .\tests\run_wheat_host_tests.ps1
```

前者依赖 GCC，后者依赖 Python 与对应库。使用匹配型号的淘晶驰编辑器打开 `hmi/WHEAT.HMI`，保持字体与素材目录关系，按 [通信协议](./hmi/通信协议.md) 核对板端实现。

## 版本说明

**技术报告与最终实物存在差异。** 报告及 `report_core` 作为设计方案、参赛文档和附录程序保留，不能直接作为最终实物配置与功能完成情况的依据。实物外观参考答辩 PPT 的实物照片，实际功能与性能以对应版本的实机演示和测试记录为准。

保存的 `camera` 源码为 2026-08-18 产品快照，答辩 PPT 记录的汇报日期为 2026-08-21。源码与后续整机照片对应不同时间点，功能状态应结合版本核对。

快照包含摄像头、LCD、YOLOX/Ethos-U55、环境采集、ESP8266 局域网服务与 SD 链路；当时的 M33 底盘、滑轨与整田任务仍有集成工作。PPT 中的双核与任务闭环方案作为后续展示材料保留，实物照片不单独替代验收记录。

报告附录示例采用 320×320 RGB INT8，产品 `camera` 采用独立的 tile224 部署契约。附录的板级与模型接口需适配，不直接覆盖产品代码。

模型链路可运行、数值一致性与田间识别精度分别判断。本次仓库整理未重新进行产品构建、烧录或实机测试。

## 第三方组件

FSP / CMSIS、RUHMI、Ethos-U、YOLOX、FatFs 与其他组件保留各自声明；自动生成代码与团队其他成员开发内容不作为个人独立原创成果。
