# 基于 RA8P1 的小麦麦穗智能计数与环境监测系统

**RA8P1 Wheat Inspection | Embedded Vision · Edge AI · YOLOX · Environmental Monitoring**

> 2026 年全国大学生电子设计竞赛信息科技前沿专题赛（瑞萨杯）决赛团队项目。

> **报告与实物差异说明：** 技术报告与最终搭建的实物存在差异。报告作为设计方案和参赛资料保留，不作为最终实物的完整实现说明，也不代表其中全部功能均已在实物中实现或验证。实物外观与搭建形式以答辩 PPT 中的实物照片为参考，具体功能和性能以对应版本的实机演示及测试记录为准。

## 🎬 项目展示 | Demo & Materials

- [📄 技术报告（与最终实物存在差异）](./技术文档.pdf)
- [📋 作品简介](./作品简介.pdf)
- [📊 项目答辩 PPT](./项目答辩.pptx)
- [💻 项目源码与模块导航](./src/README.md)

本地材料包含报告、演示文稿、设备图片和工程源码。具体功能完成情况以对应版本的工程与实测记录为准。

## 1. 项目概览 | Overview

本项目面向小麦育种样本与标准样方调查，围绕麦穗计数、图像复核及环境信息记录，构建基于 **RA8P1 + OV5640 + Ethos-U55 + ESP8266** 的嵌入式视觉系统。

项目涉及摄像头采集、YOLOX 分块推理、坐标恢复与全局去重、本地显示、温湿度与光照采集、局域网交互和 SD 存储。

| 平台 / 模块 | 主要职责 |
|---|---|
| **RA8P1 / Cortex-M85** | 采集、预处理、服务调度、推理后处理与任务接口 |
| **Ethos-U55** | 经 RUHMI 工具链部署的端侧模型推理 |
| **OV5640** | 小麦图像采集 |
| **ST7796S** | 实时画面与检测结果显示 |
| **DHT11 / 光照传感器** | 温湿度与相对光照采集 |
| **ESP8266** | Wi-Fi 接入及局域网 HTTP / WebSocket 数据交互 |
| **TJC 串口屏 / SD** | 人机交互资料、参数输入与数据记录 |


## 2. 系统架构与核心代码 | Architecture & Code

```text
OV5640 → RA8P1 / Cortex-M85
              ├─ 图像采集、分块与预处理
              ├─ Ethos-U55：YOLOX 推理
              ├─ 坐标恢复、全局 NMS 与计数
              ├─ ST7796S：画面和结果显示
              ├─ DHT11 / ADC：环境采集
              ├─ SD：记录与导出
              └─ ESP8266 → HTTP / WebSocket → 局域网网页

HMI、M33 底盘与滑轨：工程资源与集成状态分别记录
```

![系统组成方案](./images/system_architecture.png)

**系统组成方案。** 

### 核心模块

| 模块 | 代码中的主要实现 | 源码 |
|---|---|---|
| 产品主工程 | 初始化、外设服务与协作式调度 | [camera](./src/camera/) / [app.c](./src/camera/src/app.c) |
| 摄像头采集 | OV5640、CEU、帧缓冲管理 | [camera_capture.c](./src/camera/src/camera_capture.c) |
| 模型推理 | RUHMI / Ethos-U55 后端与模型存储 | [wheat_ai_backend.c](./src/camera/src/wheat_ai_backend.c) |
| 图像预处理 | 图像输入与分块预处理 | [wheat_preprocess.c](./src/camera/src/wheat_preprocess.c) |
| 检测后处理 | YOLOX 解码、检测框恢复与筛选 | [wheat_yolox_decode.c](./src/camera/src/wheat_yolox_decode.c) / [wheat_postprocess.c](./src/camera/src/wheat_postprocess.c) |
| 模型部署工具 | 主机侧模型转换、量化与数值检查 | [wheat_deployment](./src/camera/tools/wheat_deployment/) |
| 环境采集 | DHT11、光照与环境服务 | [environment_service.c](./src/camera/src/environment_service.c) |
| 网页与联网 | ESP AT、HTTP / WebSocket、网页资产 | [web_ui_page.inc](./src/camera/src/web_ui_page.inc) |
| 数据存储 | SD、记录队列与恢复服务 | [storage_service.c](./src/camera/src/storage_service.c) |
| 串口屏 | HMI 工程、字体和素材 | [hmi](./src/hmi/) |
| 报告核心代码 | C99 附录程序与板级适配接口 | [report_core](./src/report_core/) |

**已有测试入口：** [主机测试目录](./src/camera/tests/) · [模型部署测试脚本](./src/camera/tests/run_wheat_host_tests.ps1)。

### 视觉模型

![YOLOX-Tiny网络结构](./images/yolox_architecture.png)

![YOLOX-Tiny检测头](./images/yolox_head.png)

**YOLOX-Tiny 网络与检测头结构。** 项目围绕密集小目标采用重叠分块、坐标回映射与全局去重的处理路线。

### 目录结构

```text
.
├─ images/                   PPT 展示图片
├─ src/                      产品源码、模型工具与串口屏工程
├─ README.md                 项目介绍
├─ 技术文档.pdf               完整技术报告
├─ 作品简介.pdf               参赛作品简介
└─ 项目答辩.pptx              答辩演示文稿
```

## 3. 个人贡献 | My Contributions

本项目由团队合作完成。**本人负责模型优化与底盘搭建。**

- **模型优化：** 负责小麦检测模型优化。模型部署与验证工具作为项目技术资料保留；优化前后的模型版本、精度和端侧耗时对比待补充。
- **底盘搭建：** 负责移动底盘的搭建。运动控制固件的具体作者分工以团队记录为准；当前交付快照中的底盘与整田自动巡检仍有集成工作。

第三方框架、模型工具链及团队其他成员开发的模块不作为本人独立原创成果。

## 4. 项目实验与结果 | Experiments & Results

### 4.1 整机实物 | Robot Prototype

![小麦巡检系统整机](./images/robot_overview.png)

**图 1. 小麦巡检系统整机。** 展示麦轮底盘、立式支架与横向采样单元。

### 4.2 田间场景 | Field Scene

![田间移动平台](./images/field_robot.png)

**图 2. 小麦田间场景。** 

### 4.3 底盘与核心控制板 | Chassis & Controller

| 麦克纳姆轮底盘 | RA8P1 核心控制板 |
|---|---|
| ![麦轮底盘](./images/chassis.png) | ![RA8P1核心板](./images/ra8p1_board.png) |

**图 3. 底盘与控制板。**

### 4.4 人机交互 | Human-Machine Interface

![串口屏](./images/hmi.png)

**图 4. 串口屏展示。** 页面工程和板端协议联调分别记录，详见 [最终通信协议](./src/hmi/通信协议.md)。

### 4.5 验证状态 | Verification Status

| 内容 | 当前展示材料与边界 |
|---|---|
| 图像采集与显示 | 产品源码、实物照片、历史交付记录|
| YOLOX / Ethos-U55 推理 | 部署代码与历史链路记录；可运行与田间精度合格分别判断 |
| 模型优化效果 | 待补充相同数据与硬件条件下的前后对比 |
| 环境、局域网与 SD | 源码和历史功能状态表 |
| 串口屏、底盘与滑轨 | 工程与接口资料；集成状态按对应版本说明 |

## 5. 环境与运行 | Setup & Usage

- [环境与运行指引](./src/README.md#环境与运行)：工程入口、开发环境、HMI 与报告核心代码的适配前提。
- [版本与验证说明](./src/README.md#版本说明)：实际快照、提交报告和后续协议的关系。

工程内部目录与业务代码保持原样，开发调试文档和本地历史材料已归档。图片来源见 [图片索引](./images/README.md)。

## 6. 第三方依赖与致谢 | Third-Party Software

工程涉及 Renesas FSP / CMSIS / RUHMI、Ethos-U、YOLOX、FatFs 和相应厂商组件。

---
