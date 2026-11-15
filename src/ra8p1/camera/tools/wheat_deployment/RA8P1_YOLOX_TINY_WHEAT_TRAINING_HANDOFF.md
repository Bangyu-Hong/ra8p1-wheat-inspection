# RA8P1 YOLOX-Tiny 小麦检测模型训练交接书

版本：2026-08-13  
用途：队友训练和完成 PC 端四运行时验证，RA8P1 项目组随后验证
`PyTorch -> ONNX FP32 -> TFLite FP32 -> TFLite INT8 -> RUHMI/MERA
(quantize=false) -> Vela -> Ethos-U55 -> 固件` 完整链路。

> 本文中的 “YOLO-Tiny” 特指 **YOLOX-Tiny**，不是 YOLOv3-Tiny、YOLOv4-Tiny，也不是把任意 YOLO 模型缩小后的统称。

> **最重要的训练边界：真正用于监督训练、验证和测试的数据全部是 GWHD
> V4。COCO 只提供 YOLOX-Tiny 的预训练初始权重，不得把 COCO 图片混入
> 本项目的小麦精度统计，也不得直接使用 COCO 80 类检测头作为最终模型。**

## 0. 队友先读的执行摘要

本轮只接受下面这一条主路线：

```text
Megvii 官方 YOLOX-Tiny COCO 预训练权重（只作为参数初始化）
  -> 用冻结的 GWHD V4 源图划分训练 1 类 wheat_head
  -> 每张 1024 x 1024 GWHD 源图确定性裁剪成 1024 x 768
     （train 按源图 ID 哈希选择纵向偏移，validation/test 固定居中）
  -> 等比例缩放成 640 x 480，并同步变换标注框
  -> 生成 12 个重叠 tile224/stride179 分区及局部标注
  -> 训练固定输入 224 x 224 的 YOLOX-Tiny
  -> 导出静态 ONNX FP32 [1,3,224,224] -> [1,1029,6]
  -> 按瑞萨 YOLOX-Tiny 路线转换 TFLite FP32
  -> 用 200 至 500 个真实 GWHD 训练分区做 PTQ，生成 TFLite INT8
  -> 用相同 validation/test、相同阈值和相同全局 NMS 比较四个运行时
  -> 交付模型、脚本、校准清单、完整指标、日志和 SHA-256
```

项目组收到合格交付物后执行：

```text
TFLite INT8
  -> RUHMI/MERA，target=npu，quantize=false
  -> Vela 编译 Ethos-U55 子图并检查所有 CPU fallback
  -> 重新加载保存的模型做 golden 验证
  -> 集成摄像头冻结、12 分区、全局 NMS、LCD 画框和计数
  -> Release 构建和 RA8P1 板上验收
```

本轮禁止用旧的 `full224` 数据或模型冒充 `tile224/stride179`；禁止把完整
`640 x 480` 画面整体压缩成一张 `224 x 224` 图片训练后直接交付。

## 1. 这次交付的目标

训练一个只检测 `wheat_head` 的 YOLOX-Tiny 模型，并提供足够完整的材料，使项目组能够：

1. 在电脑上复现 224 重叠分区推理、全局合并和每张图片的小麦计数；
2. 检查 PyTorch、ONNX、TFLite FP32 和 TFLite INT8 的结果是否一致；
3. 用固定 RUHMI/MERA 工具链编译并检查 Ethos-U55 NPU 算子分配；
4. 检查权重、激活和运行时内存能否装入 RA8P1 板载存储；
5. 集成到现有“实时预览 -> 按 USER 键冻结完整帧 -> 分区推理 -> 全局合并 -> 画框和计数 -> 再按一次恢复预览”的固件链路。

这次不要求队友修改 RA8P1 固件，也不要求在训练电脑上代替项目组完成板上验收。队友的职责是交付**可复现、可转换、可比较**的模型包，而不是只发一个 `best.pth`。

## 2. 为什么先选 YOLOX-Tiny

### 2.1 三种模型的核心区别

| 项目 | YOLOX-Tiny | YOLO-Fastest 1.1 | YOLOv8n |
|---|---|---|---|
| 定位 | 小型但结构完整的通用检测器 | 极小型、优先追求低资源占用 | 新一代轻量 YOLO，训练生态成熟 |
| 检测方式 | Anchor-free，不预先规定固定锚框 | Anchor-based，需要预设锚框 | Anchor-free，使用 DFL 边框回归 |
| 主要结构 | CSPDarknet + PAFPN + 解耦检测头 | 极简主干 + 两个锚框检测头 | C2f 主干 + PAN/FPN + 解耦 DFL 检测头 |
| 小目标潜力 | 中等，通常明显强于 Fastest | 较弱，密集小麦穗容易漏检 | 通常较强，但依赖工具链兼容和量化质量 |
| 瑞萨公开 RA8P1 路线 | 有完整 224 模型、转换、NPU C 代码和前后处理；实际张量沿用 OpenCV BGR | 当前公开 320 RGB 路线为 CPU-only | 没有与当前项目模型完全相同的官方成功链路 |
| 瑞萨公开板上数据 | NPU 约 73 ms；CPU 约 1914 ms | CPU 约 368 ms；公开路线尚不支持 NPU | 当前项目模型尚未形成合格板上数据 |
| 当前项目风险 | 224 输入可能降低密集小目标召回，需要实测 | 精度上限和小目标召回偏低 | Resize 落到 CPU；替代的 ConvTranspose 也落到 CPU；现有 INT8 召回严重下降 |
| 当前用途 | **第一优先：先打通可信 NPU 链路** | NPU 路线失败时的轻量备选/固件参考 | 保留为后续研究，不再作为本轮最快验证路线 |

说明：表中的速度和公开精度来自瑞萨各自的参考模型，输入尺寸、数据集和执行后端不同，不能把它们当成同一条件下的横向跑分。它们只用于判断部署成熟度。

### 2.2 官方参考模型给出的证据

本交接书在 2026-08-13 对照了 Renesas `ruhmi-model-zoo` main commit
`c4e393c8ae1d54c78bf38efffbfa0f0213113ff4` 和 Megvii YOLOX `0.3.0` tag commit
`419778480ab6ec0590e5d3831b3afb3b46ab2aa3`。瑞萨公开 YOLOX-Tiny 参考模型使用：

- 输入：`224 x 224`、batch 1、数值范围 `[0,255]`；README 标为 RGB，但转换和
  推理代码实际使用 `cv2.imread()` 的 BGR 数组且不交换通道；
- COCO 模型输出：`[1, 1029, 85]`；
- 1029 个候选位置来自 `28 x 28 + 14 x 14 + 7 x 7`；
- COCO AP50：TFLite FP32 `35.95`，TFLite INT8 `35.13`，MERA INT8 `35.21`；
- RA8P1 + OSPI + SDRAM：CPU 约 `1914 ms`，NPU 约 `73 ms`。

官方资料：

- [Renesas RUHMI YOLOX-Tiny（固定审计提交）](https://github.com/renesas/ruhmi-model-zoo/tree/c4e393c8ae1d54c78bf38efffbfa0f0213113ff4/vision/object_detection/yolox_tiny)
- [Renesas object detection tutorial（固定审计提交）](https://github.com/renesas/ruhmi-model-zoo/blob/c4e393c8ae1d54c78bf38efffbfa0f0213113ff4/tutorials/object_detection.ipynb)
- [Megvii YOLOX 0.3.0（固定审计提交）](https://github.com/Megvii-BaseDetection/YOLOX/tree/419778480ab6ec0590e5d3831b3afb3b46ab2aa3)
- [Megvii 自定义数据训练说明（固定审计提交）](https://github.com/Megvii-BaseDetection/YOLOX/blob/419778480ab6ec0590e5d3831b3afb3b46ab2aa3/docs/train_custom_data.md)
- [YOLOX paper](https://arxiv.org/abs/2107.08430)

对单类小麦模型，最后一个维度不是 COCO 的 85，而应为：

```text
4 个框参数 + 1 个 objectness + 1 个 wheat_head 类别分数 = 6
```

因此本项目 Phase A 的预期原始输出是 `[1, 1029, 6]`。必须通过实际导出模型检查确认，不能只按文档猜测。

### 2.3 COCO 预训练与 GWHD 训练不是二选一

`COCO` 是包含 80 类常见目标的通用检测数据集。Megvii 已经用它训练过一份
YOLOX-Tiny 权重。我们使用这份权重，是为了让网络从一个已经学会边缘、纹理、
轮廓等通用视觉特征的起点开始，而不是从随机数开始。

加载预训练权重后，必须把检测头改成一个 `wheat_head` 类，并用 GWHD 的小麦
图片和小麦框继续训练。这叫迁移学习或微调：

```text
COCO 预训练权重
  = 训练开始前的参数初值
  != 本项目训练数据
  != 本项目验证数据
  != 最终 80 类模型

GWHD V4
  = 本项目真正的监督训练、验证和测试数据
  = 决定最终模型是否能检测和计数小麦
```

允许从 COCO 权重初始化；不允许使用 COCO 指标代替 GWHD 指标，也不允许只把
COCO 类别名称改成 `wheat_head` 而不重新训练一类检测头。若预训练权重的 80 类
头与一类头 shape 不同，训练脚本应只加载 shape 匹配的主干/颈部权重，并在日志
中列出缺失键和意外键；不得静默跳过整个主干。

### 2.4 与官方 YOLOX-Tiny 链路的最终对照

以下部分与官方路线保持相同：

- YOLOX-Tiny 架构参数：`depth=0.33`、`width=0.375`、`depthwise=false`；
- 使用 Megvii COCO checkpoint 初始化，再用自定义数据微调；这也是 Megvii 官方
  自定义数据训练文档推荐的方式；
- 固定 `224 x 224`、batch 1、`[0,255]`、无 `/255` 归一化；
- ONNX 使用 `decode_in_inference=false`，输出 YOLOX 原始头，grid decode、阈值和
  NMS 留给 Cortex-M85；
- `PyTorch -> ONNX FP32 -> TFLite FP32 -> TFLite INT8`，随后 RUHMI/MERA 使用
  `target=npu, quantize=false`，再由 Vela 编译 Ethos-U55 子图；
- 官方 NPU 产物允许 YOLOX Focus 切片留在 Cortex-M85，卷积主干、PAFPN 和
  检测头进入 Ethos-U55。本次固定提交的生成 C 代码具体表现为两个 CPU 单元、
  共 6 次 `StridedSlice` 调用，最终形成四路 Focus 张量并复制到一个 NPU 子图；
  因此不能把“CPU 有 6 次切片调用”误判为 6 个神经网络阶段回退。

以下部分是本项目必须做的适配，不代表偏离 YOLOX 算法：

- 官方直接使用已经训练好的 COCO 80 类模型；本项目把检测头改为 1 类并用 GWHD
  `wheat_head` 标注重新训练；
- 官方将一张完整 COCO 图片 letterbox 到 `224 x 224`；本项目先构造与摄像头一致的
  `640 x 480` 参考画面，再生成 12 个原尺寸 `224 x 224` 重叠 tile；
- 官方校准使用 COCO val2017；本项目校准只能使用冻结 train split 的 GWHD tile；
- 官方只做单图检测；本项目要把 12 个 tile 的局部框恢复到完整画面后做一次全局
  NMS，并以保留框数作为小麦计数；
- 官方生成一个额外的 ONNX INT8 旁路文件，但正式 RUHMI 配置读取的是 TFLite INT8；
  本项目不要求交付未进入正式部署链路的 ONNX INT8；
- 官方公开产物使用 MERA `2.6.0+pkg.4293`、FSP 6.2；本项目固件为 FSP 6.5.0，
  因此模型转换数值可复用官方路线，但生成 C 代码仍必须在本机固定 RUHMI 环境中
  重新编译并通过 FSP 6.5.0 Release 构建和板上验收。

结论：本方案的**网络、导出、TFLite INT8 和 NPU 编译主链路与瑞萨官方相同**；
训练数据、类别、分区几何、全局合并和固件交互是为小麦计数增加的项目适配。

## 3. 冻结的产品输入合同

### 3.1 Phase A：必须先完成的 tile224 链路

本轮训练和交付必须固定为：

```text
model family: YOLOX-Tiny
task: object detection
classes: 1
class name: wheat_head
batch at deployment: 1
input size: 224 x 224
training/inference layout: dedicated overlapping 224 x 224 tiles
input color: BGR（与 Megvii 训练代码及 Renesas 实际转换脚本一致）
ONNX input: float32 NCHW [1,3,224,224]
TFLite input: NHWC [1,224,224,3]
shape: static
dynamic axes: disabled
embedded preprocessing: disabled
embedded NMS: disabled
output: raw YOLOX detection tensor before CPU grid decode/NMS
expected one-class output: [1,1029,6]
```

这里的“原始输出”不是最终检测框。板端 Cortex-M85 负责有限的后处理：网格解码、置信度过滤、NMS、坐标还原和计数。让 CPU 做这些有限后处理是正常设计；不允许把整个神经网络回退到 CPU。

**224 是每个分区的模型输入，不是把完整 640 x 480 帧压缩成一张 224 x 224 图片。**产品流程冻结为：

```text
OV5640/CEU 捕获一张完整 640 x 480 RGB565 帧
  -> USER 键触发后把这张完整帧冻结在 SDRAM
  -> 按确定性窗口生成多个 224 x 224 重叠分区
  -> 每个分区从 RGB565 解包并按 B、G、R 顺序量化，送入同一个 YOLOX-Tiny NPU 模型
  -> 每个分区输出局部检测框
  -> 给局部框加上 tile_x/tile_y，恢复到 640 x 480 全局坐标
  -> 收集所有分区候选框，只执行一次全局 NMS 去重
  -> NMS 后框数作为最终小麦数量
  -> 在冻结的完整 640 x 480 LCD 画面上绘制全局框和数量
```

首版 tile224 几何合同为：

```text
tile size: 224 x 224
nominal stride: 179（约 20% 重叠）
x origins for 640: 0, 179, 358, 416
y origins for 480: 0, 179, 256
tiles per VGA frame: 4 x 3 = 12
edge rule: 常规步进不能正好覆盖边缘时，追加 max(image_size - 224, 0)
padding: 只在源图小于分区时使用 BGR 114 补齐（三通道数值相同）
```

最后追加的边缘分区可能与前一分区重叠较多，这是为了不漏掉画面右侧和底部的小麦；重复框由全局 NMS 统一消除。12 个分区复用同一套输入、输出和 NPU 工作内存，不为 12 个分区各留一套模型缓冲区。按瑞萨公开 NPU 版本约 73 ms 的单次 AI-only 数据估算（该路径仍包含 Focus CPU 切片），12 次约为 `12 x 73 = 876 ms`，仍有机会连同预处理和后处理控制在用户要求的 2 秒内，但这只是可行性预算，不是本项目实测结论。

### 3.2 Phase B：只有 Phase A 合格后才允许开始

如果 tile224 模型已经完整通过 PC 精度、INT8、MERA 保存模型、NPU 分配、内存和板上速度验收，但精度或总时延仍不合适，再建立**独立的** tile320 对照实验：

```text
input size: 320 x 320
training/inference layout: dedicated 320 x 320 tiles, stride 256
expected one-class output: [1,2100,6]
candidate grids: 40 x 40 + 20 x 20 + 10 x 10 = 2100
```

禁止把 224 ONNX 的输入节点直接改成 320，或只在导出时传入 320 而不重新训练和验证。模型虽然是全卷积结构，但输入变大后，训练分布、输出网格、内存、算力、量化范围和板上延迟都会变化。

Phase A 和 Phase B 必须使用不同目录、不同 checkpoint、不同 manifest 和不同哈希，不能覆盖。

## 4. 数据集合同

使用 Global Wheat Head Dataset 2020 V4（GWHD V4）：

- 3,422 张有标注图片；
- 145,411 个有效小麦穗框；
- 7 个有标注来源域；
- 46 张零目标图片属于合法负样本，必须保留；
- 当前冻结划分：2,735 train、339 validation、348 test；
- 随本文附带的源图成员清单：`GWHD_V4_FROZEN_SPLIT_MANIFEST.csv`；
- CSV 的 `split` 字段固定使用 `train`、`val`、`test`，其中 `val` 就是本文所称的
  validation，不得把它解释成新的第四个集合；
- 该 CSV 共 3,422 行数据，文件 SHA-256：
  `e60a54fd1a8c9d3e0aecb5fe2c94bf1ad4e00db645ffdef2462f4f43fed79262`；
- 先前完整数据物化报告的 aggregate SHA-256：
  `d2c4d7dac7f2c91a0f3a75358e76c05215be22037a1b40d91a91f7200299a627`。

数据集本身已经有框标注，不需要重新逐张人工标注。需要做的是格式转换、划分冻结和标注审计。

旧版文档曾引用 full224 诊断包的另一组人数，那不对应上述冻结成员清单。本轮必须
使用 `2,735/339/348`，不得自行重新随机划分。
CSV 只包含 `split,source_id` 两列；`source_id` 是规范化的 `domain__uuid`，用于
在 GWHD V4 标注和图片中恢复原始成员。重新执行本轮裁剪、缩放和 tile 生成后，
必须产生并记录新的图片/标签哈希。

如果队友没有同时收到本文和 `GWHD_V4_FROZEN_SPLIT_MANIFEST.csv`，应停止训练并
先索取清单。只凭人数、随机种子或 aggregate hash 无法可靠重建同一成员划分。

### 4.1 必须遵守的划分规则

1. 先按原始图片 ID 划分 train/validation/test，再做裁剪或增强。
2. 同一张原图产生的任何视图不得跨越不同 split，防止数据泄漏。
3. 训练、验证和测试的图片 ID 清单必须保存并计算 SHA-256。
4. 不得把 1,276 张无公开标注的 challenge 图片当成“没有小麦”的负样本。
5. COCO JSON 中类别固定为一个：`id=1, name=wheat_head`。
6. 负样本图片要保留在 `images` 中，但对应 `annotations` 为空。

### 4.2 生成专用 tile224 训练数据

比赛运行时不会把完整 640 x 480 帧整体缩成 224，而是按重叠窗口逐块送入模型。因此训练方必须生成专用 `tile224` 数据，不能只训练 full224 模型后假定分区输入一定同样有效。

在冻结 train/validation/test 源图划分之后，每张 `1024 x 1024` GWHD 源图先按
固定规则映射成与摄像头一致的 `640 x 480` 参考画面，然后再按板端相同的位置
切分区。这里既有裁剪，也有等比例缩放，顺序不能交换：

```text
原图：1024 x 1024
  -> 裁剪 4:3：保留 x=[0,1024)，y=[crop_y,crop_y+768)，得到 1024 x 768
     train: crop_y = int(SHA256("42:" + source_id)[0:16], 16) mod 257
     validation/test: crop_y = 128
  -> 等比例缩放 0.625：得到 640 x 480
  -> 在 640 x 480 上按固定起点切 12 个 224 x 224 分区

x origins: 0, 179, 358, 416
y origins: 0, 179, 256
```

这不是把 `1024 x 1024` 直接压扁成 `640 x 480`。validation/test 使用固定中心
裁剪，即去掉顶部和底部各 128 行；train 的 `crop_y` 由源图 ID 和种子 42 的
SHA-256 确定，因此每次运行结果相同，同时可覆盖更多纵向位置。裁剪后宽高同时
乘 `0.625`，因此物体比例不会被拉伸。这里的 `source_id` 必须是 manifest 中
冻结的规范化文件名，不能使用不同电脑上的绝对路径，否则哈希结果会变化。

首轮基线中，每张源图只生成上述一个 `640 x 480` 参考画面，不生成顶部/底部
双视图。这样一张源图对应一次产品摄像头取景，validation/test 的每图计数也只有
一个明确真值；train 已通过 2,735 个源图的确定性 `crop_y` 覆盖不同纵向位置。
双视图会重复大量重叠目标、改变每张源图的训练权重，并把本轮 tile 数从 41,064
继续扩大，因此只可在首轮基线完整保留后作为独立对照实验，不能混入本次交付。

按冻结规则对本地 145,411 个标注框做连续浮点几何复算，裁剪后保留 117,737 个
参考画面框，丢弃 27,443 个完全位于裁剪窗口外的框和 231 个缩放后最短边不大于
1 像素的片段。其中 train/validation/test 分别保留 93,546/11,911/12,280 个框。
这与 YOLOX `0.3.0` `TrainTransform` 只保留最短边大于 1 像素框的边界一致；本轮
共有 41,064 个 tile。队友生成数据后必须复现这些计数；不一致时先停止并检查
坐标、裁剪和标注版本，不得用修改阈值凑数。

标注框必须与图像执行完全相同的几何变换：

1. 先用裁剪窗口 `[0,crop_y,1024,crop_y+768)` 裁剪原始框；
2. 完全不与裁剪窗口相交的框丢弃；只要仍相交，就保留裁剪后的可见框，不使用
   原框 50% 面积阈值，也不要求原框中心仍在窗口内；否则仍出现在画面里的边缘
   麦穗会被错误地当成背景；
3. 裁剪坐标转换为 `x' = x`、`y' = y - crop_y`；
4. 所有坐标和宽高再乘 `0.625`，得到 `640 x 480` 坐标；
5. 坐标裁剪到合法边界；缩放后最短边不大于 1 像素的亚像素片段丢弃，其他边缘
   框保留，并记录被丢弃框的数量和原因。

完成 `640 x 480` 参考画面后，再执行确定性分区：

```text
tile size: 224 x 224
stride: 179
x origins: 0, 179, 358, 416
y origins: 0, 179, 256
edge origin: 常规步进未覆盖边缘时追加 max(image_size - 224, 0)
```

每个训练框按以下规则分配到分区：

1. 先把框裁剪到分区边界；
2. 裁剪后可见面积不少于原框的 50% 才保留，不额外要求中心点位于分区内；
3. 若一个参考画面框与 tile 相交、但在全部 12 个 tile 中都未达到 50%，只把它
   分配给可见面积比例最大的一个 tile；比例并列时选择 row-major `tile_index`
   最小者。该记录标记 `assignment_reason=max_visible_fallback`，不能把 50% 阈值
   整体降低；
4. 框坐标减去 `tile_x/tile_y`，转为分区局部坐标；
5. 没有保留框的分区仍可作为合法负样本，但要记录其来源；
6. 在独立的 `box_assignment_manifest.csv` 中保存源图 ID、split、源标注 ID、
   tile ID、原框、参考画面框、tile 裁剪框、可见比例和分配原因。

`tile_manifest.csv` 还必须记录源图尺寸、`crop_x=0`、实际 `crop_y`、
`crop_w=1024`、`crop_h=768`、`scale=0.625`、参考画面尺寸、分区起点、保留框数、
丢弃框数、fallback 框数、图片哈希和标签哈希。所有交集面积、可见比例和分配
判断都使用未舍入的连续浮点坐标；只在最终生成图像或序列化标注时执行一次明确的
坐标转换。项目组必须能够仅凭 manifest 和脚本逐字节重建 validation/test 分区。
`tile_manifest.csv` 必须恰有 41,064 个 tile 数据行，包括没有标签的 tile；
`box_assignment_manifest.csv` 必须恰有 223,052 个框分配数据行。两种粒度不能
混在一张表中，否则空 tile 会消失，重复框和 tile 数也无法分别审计。

“至少 50% 可见面积”规则用于过滤 tile 中只剩很小一角的残缺小麦；它不是在
源图转 4:3 参考画面时删除边缘框的规则。低于 50% 的相交片段不会在该 tile 中
作为正标签，但同一参考目标必须通过重叠窗口或唯一 fallback 至少在一个 tile 中
成为正标签。由于分区存在重叠，同一个目标只要在多个视图中都满足可见面积要求，
就允许它出现在多个 tile 标签中。这与真实推理一致，重复检测随后由完整画面坐标
上的一次全局 NMS 去重。数据审计必须报告“每个参考框进入几个 tile”的分布以及
低于阈值的相交次数，出现重复不是数据泄漏，但同一源图的 tile 跨 split 才是数据泄漏。

本地复算发现只有 2 个 train 框需要上述 fallback：它们在参考画面中约为
`197 x 232` 和 `137 x 163` 像素，最佳 tile 可见比例分别为 49.218% 和 47.868%。
加入 fallback 后，117,737 个参考画面框全部至少进入一个 tile，局部标签分配总数
为 223,052，单 tile 最大标签数为 21。`geometry_audit.json` 必须至少报告
`source_boxes=145411`、`reference_boxes=117737`、`fully_outside_boxes=27443`、
`subpixel_rejected_boxes=231`、`tile_label_assignments=223052`、`fallback_assignments=2`、
`unassigned_reference_boxes=0` 和实际单 tile 最大标签数。按当前连续浮点规则，
低于 50% 的有效相交次数为 73,482，另有 3,642 个 tile 交集最短边不大于 1 像素；
这些计数也应记录，以便发现坐标舍入或阈值实现发生变化。

完整画面检测 AP 和计数指标的 ground truth 必须使用 117,737 个裁剪/缩放后的
参考画面框，每个真实目标只出现一次；不得把同一目标在多个 tile 中的重复局部
标签当成多个全局真值。tile 局部指标可作为诊断，但最终模型选择、全局 AP 和计数
必须先恢复到 `640 x 480` 坐标、执行一次全局 NMS 后再与参考画面真值比较。

训练集只允许在分区生成后使用本交接书第 5.2 节冻结的颜色和水平翻转增强；首轮
关闭 Mosaic、MixUp、旋转、平移、剪切和随机多尺度。validation/test 的分区位置
和像素必须完全确定。禁止训练脚本继承 YOLOX 默认的强几何增强而未显式覆盖参数。

### 4.3 颜色和数值范围不能靠猜

瑞萨 README 把输入称为 RGB，但 Megvii 训练代码、瑞萨 `download_model.py` 和
`inference.py` 都使用 `cv2.imread()` 的 BGR 数组且没有交换 R/B；瑞萨嵌入式
预处理源码也按 B、G、R 顺序写模型输入。为了与 COCO 预训练权重及官方已跑通
NPU 产物的真实语义一致，本项目把模型张量合同冻结为 **BGR**：

- 数据集图片用 `cv2.imread()` 读取后直接保持 BGR；
- ONNX NCHW 三个通道依次是 B、G、R；
- TFLite NHWC 最后一个维度依次是 B、G、R；
- 固件从 RGB565 解包得到 R/G/B 数值后，按 B、G、R 写入模型张量。

队友必须用固定张量证明这一合同，提供一张固定完整图片及其中一个固定 tile 在每个阶段的输入张量：

- 从完整原图裁剪并在必要时用 114 补齐后的固定 tile 图片；
- RGB/BGR 通道顺序检查图；
- ONNX NCHW float32 输入 `.npy`；
- TFLite NHWC 输入 `.npy`；
- INT8 量化后的输入 `.npy`；
- 每个张量的 shape、dtype、min/max 和 SHA-256。

另外提供一张不对称红/蓝测试图：左半纯红、右半纯蓝。PyTorch、ONNX、TFLite
FP32、TFLite INT8 和固件参考输入必须证明通道 0 是 B、通道 2 是 R。禁止因为
瑞萨 README 中的 RGB 字样而在转换阶段额外交换一次通道。

不允许只写“做了归一化”。必须明确是 `[0,255]` 还是 `[0,1]`、先换通道还是先量化、padding 值是 114 还是其他值。

## 5. 训练环境和代码要求

### 5.1 使用的代码库

训练以 Megvii 官方 YOLOX 为基础，网络必须保持 YOLOX-Tiny 结构：

```text
depth = 0.33
width = 0.375
depthwise = false
num_classes = 1
```

优先从官方 COCO YOLOX-Tiny 预训练权重微调，不要从随机权重开始。这里的
“用 COCO 权重”只表示执行一次 `load_state_dict` 初始化，之后每个训练 batch、
验证指标和模型选择都来自 GWHD tile。训练仓库必须固定 Git commit，不能只写
“最新版 YOLOX”。瑞萨导出脚本中的自包含模型结构也必须与训练结构逐层对照，
尤其是类别数、检测头和 `decode_in_inference`。

训练日志必须记录：

- COCO 预训练权重的下载 URL、文件名、字节数和 SHA-256；
- 加载预训练权重时的 missing keys、unexpected keys 和 shape mismatch；
- 一类 `wheat_head` 检测头确实重新初始化并参与训练；
- 训练/验证数据加载器只读取冻结的 GWHD split 和由它生成的 tile；
- 第一个和最后一个 epoch 的真实 batch shape、类别 ID 范围和样本来源统计。

瑞萨公开 YOLOX-Tiny 转换脚本只能作为结构和命令流程参考，不能原样运行后直接
交付。队友必须明确完成以下四项适配，并把差异保存在脚本和 `README.md` 中：

1. 把官方 COCO `80` 类配置改为本项目 `1` 类 `wheat_head`，同步检查输出末维为 `6`；
2. 加载本轮 GWHD 训练得到的 `best_ckpt.pth`，不得重新下载官方 COCO checkpoint
   并把它误当成已经训练完成的小麦模型；
3. 模型张量语义固定为 BGR，沿用 OpenCV 的 BGR 读取顺序，并用红蓝不对称测试图
   验证 PyTorch、ONNX、TFLite 和固件参考输入的通道一致；
4. TFLite INT8 的 representative dataset 只使用冻结 train split 生成的 GWHD
   `tile224` 张量，不得继续使用官方 COCO 校准图片。

### 5.2 自定义 Exp 必须固定的字段

从官方 `exps/default/yolox_tiny.py` 建立单独的 `gwhd_yolox_tiny_tile224.py`，至少显式记录：

```text
num_classes = 1
depth = 0.33
width = 0.375
input_size = (224, 224)
test_size = (224, 224)
enable_mixup = false
mosaic_prob = 0.0
degrees = 0.0
translate = 0.0
shear = 0.0
flip_prob = 0.5
hsv_prob = 0.5
max_epoch = 150
warmup_epochs = 5
no_aug_epochs = 15
ema = true
seed = 42
```

训练基线使用 YOLOX 官方优化器、学习率缩放、损失、EMA 和最后 15 epoch 的 L1
阶段，但首轮从一开始就关闭 Mosaic。YOLOX `0.3.0` 的 Mosaic 会把 4 个
`224 x 224` tile 放到 `448 x 448` 画布后再缩回 `224 x 224`，通常会把每个麦穗
再次缩小约一半；这与板端直接输入原尺寸 tile 的分布不一致。若首轮出现明显过拟合，
后续可以建立独立 Mosaic 对照实验，但不能覆盖本轮基线。若 150 epoch 结束时
validation 指标仍持续提升，可以在保留第一阶段完整产物后续训到 300 epoch。

YOLOX `0.3.0` 默认会把普通训练样本截断到 50 个标签。队友必须在生成 tile 后
统计单 tile 框数；自定义 data loader 的 `max_labels` 必须不低于观测最大值并至少
留 10% 余量。即使 Mosaic 已关闭，也要同步提高其包装层的上限，防止配置切换后
重新出现截断。日志必须报告最大值、两个配置上限和 `truncated_labels=0`，禁止
密集麦穗标签被静默截断。本地冻结几何当前观测最大值为 21，因此官方普通样本
默认上限 50 在本次数据上足够，但仍必须由队友的实际生成结果重新证明，不能把
21 当作以后数据版本的永久上限。

训练期间也应关闭随机多尺度，保证所有 batch 都是 224。不同 YOLOX commit 的配置名不同：新版本通常使用 `multiscale_range=0`，旧版本可把 `random_size` 固定为 `(7,7)`。最终以训练日志中每个 batch 的实际 shape 为准，不能只看 Exp 文件。

### 5.3 推荐的训练启动方式

下面是命令结构示例，实际路径和 batch 按训练电脑显存调整，并把最终命令原样保存到 `commands.txt`：

```bash
python tools/train.py \
  -f exps/custom/gwhd_yolox_tiny_tile224.py \
  -d 1 \
  -b 8 \
  --fp16 \
  -o \
  -c /path/to/yolox_tiny.pth
```

- `-d 1` 表示使用 1 张 GPU；
- `-b 8` 是总 batch，可根据显存改为 4、8、16，但必须记录；
- `--fp16` 只用于电脑训练加速，不代表最终部署为 FP16；
- `-o` 启用可用的显存优化；
- `-c` 指向官方 YOLOX-Tiny COCO 预训练权重。

选择最佳 checkpoint 必须依据冻结 validation 集上的检测和计数指标，不能依据训练 loss 最低，也不能在 test 集上反复挑模型。

## 6. 必须报告的训练指标

FP32 PyTorch 模型至少报告：

- precision；
- recall；
- AP50；
- AP50-95；
- 每图计数 MAE；
- 每图计数 RMSE；
- 平均计数偏差 `mean(predicted_count - ground_truth_count)`；
- 负样本误报率；
- 推理时间的 P50、P95 和最大值，注明 GPU/CPU 型号。

另外分别报告以下子集，不能只给一个总 mAP：

- 小麦穗数量稀疏、中等和密集图片；
- 小目标占比较高图片；
- 遮挡明显图片；
- 边缘截断目标图片；
- 46 张合法负样本。

保存至少 30 张可视化结果，必须包含正确案例、漏检、重复框、误检和计数偏差最大的案例。检测模型的“计数”就是 NMS 后有效 `wheat_head` 框的数量；如果框本身不可靠，只隐藏框显示数字并不能让计数变可靠。

## 7. 固定 ONNX 导出合同

训练方必须提供一个适用于 tile224 部署验证的 FP32 ONNX：

```text
input name: images
input dtype: float32
input shape: [1,3,224,224]
output dtype: float32
output shape: [1,1029,6]（以实际检查结果为准）
opset: 18（与本次审计的 Renesas YOLOX-Tiny 转换脚本一致）
dynamic axes: none
NMS: not embedded
TopK: not embedded
preprocessing: not embedded
decode_in_inference: false
```

必须使用与瑞萨 YOLOX-Tiny 模型包兼容的构图和导出路径。不要通过 Netron 手工改节点，不要在导出后静默改输入尺寸，也不要为了让编译器通过而删除学习得到的节点。

ONNX 交付前必须完成：

1. `onnx.checker.check_model()`；
2. 静态 shape inference；
3. 列出所有输入输出名称、shape、dtype；
4. 用 ONNX Runtime 跑完整 validation；
5. 与 PyTorch 使用相同分区、相同前处理和相同后处理比较局部框、全局框、分数、NMS 索引和计数；
6. 保存至少 20 张固定 golden 原图的所有分区原始输出 `.npy`、合并前候选和全局 NMS 后检测 JSON。

PyTorch 和 ONNX 结果不一致时，禁止继续 INT8 或 RUHMI。

## 8. TFLite 和 INT8 量化要求

本轮优先复用瑞萨 YOLOX-Tiny 的官方路线：FP32 ONNX 转 TFLite FP32，再生成 TFLite INT8。先做 PTQ（训练后量化）；只有 PTQ 明确无法维持召回率时，才单独评估 QAT，不要一开始同时更改训练和量化方案。

### 8.0 名词、文件和工具的职责

以下几个名字不在同一层，不能互相替代：

| 名称 | 本项目中的含义 | 是否已经量化 | 本轮用途 |
|---|---|---:|---|
| PyTorch `.pth` | 训练框架的 checkpoint，可包含权重、优化器和 epoch | 否，FP32 训练基线 | 继续训练和保存最佳模型 |
| ONNX FP32 | 跨框架交换格式，保存静态计算图和 FP32 权重 | 否 | 结构检查、ONNX Runtime 验证、转换源 |
| TFLite FP32 | TensorFlow Lite 运行格式，仍使用 FP32 | 否 | 检查 ONNX 到 TFLite 的转换是否改变结果 |
| TFLite INT8 | 权重和激活按校准范围表示为 8 位整数的部署模型 | 是 | 交给 RUHMI/MERA 和 Ethos-U55 的正式输入 |
| RUHMI | 瑞萨提供的 MCU AI 编译工具链入口 | 取决于输入和配置 | 调用底层导入、编译和代码生成流程 |
| MERA | RUHMI 使用的模型编译/运行组件 | 可量化，也可只编译既有 INT8 | 导入、分图、生成 CPU/NPU 区域和保存模型 |
| Vela | Arm 为 Ethos-U55/U65 提供的 NPU 编译器 | 要求量化网络 | 把受支持的 INT8 NPU 子图编译成命令流和权重布局 |

ONNX 和 TFLite 都能描述神经网络，但侧重点不同。ONNX 适合作为训练框架之间的
通用交换图；TFLite 更接近轻量运行时和嵌入式部署。`TFLite FP32` 中的 FP32
仍是 32 位浮点数，它不是“半量化模型”。建立这一中间模型的目的，是在量化前
先证明 `ONNX -> onnx2tf -> TFLite` 的算子、布局和数值语义没有变坏。

典型布局为：

```text
ONNX FP32 input:   NCHW [1,3,224,224]
TFLite FP32 input: NHWC [1,224,224,3]
TFLite INT8 input: NHWC [1,224,224,3]
```

布局变化不代表图片被旋转；它只改变张量维度的排列。转换脚本和验证代码必须
显式转置，不能把同一块内存不经转换同时交给 NCHW 和 NHWC 模型。

`PTQ` 是 Post-Training Quantization，即“训练后量化”。网络先按 FP32 训练完成，
再使用一批有代表性的真实输入测量各层激活范围，为浮点值计算 INT8 的 `scale`
和 `zero point`。这批图片叫校准集，不参与梯度更新，也不能使用 test 集。

本轮的正式量化只允许发生一次：

```text
ONNX FP32
  -> onnx2tf / TensorFlow Lite 转换
  -> 使用 GWHD tile 校准做 PTQ
  -> TFLite INT8（已经量化完成）
  -> RUHMI/MERA：quantize=false（跳过 MERA 的第二次 PTQ）
```

`quantize=false` 的准确含义是：“输入模型已经是带量化参数的 TFLite INT8，MERA
只导入和编译，不再用另一批图片重新计算 scale/zero point。”它不表示使用 FP32
推理，也不表示禁用 Ethos-U55。对应的项目组配置必须类似：

```yaml
model_path: <yolox_tiny_gwhd_224_int8.tflite>
target: npu
quantize: false
calib_data: ''
```

旧实验采用的是另一条路线：`FP32 ONNX -> MERA quantize=true -> MERA INT8`。
那条路线虽然能生成代码，但当前 full224 小麦模型量化后严重漏检。本轮不同时
保留两条 PTQ 主路线，不把已经量化的 TFLite INT8 再送入 MERA 做第二次 PTQ。

Vela 位于 MERA 之后。MERA 先判断哪些算子属于 Ethos-U55 子图，再把这些量化
子图交给 Vela。Vela 负责为 NPU 安排算子、张量内存、权重布局和命令流；不支持
的算子仍可能生成 Cortex-M85 `C_CODEGEN` 路径。因此“Vela 编译成功”不等于
整网都在 NPU，必须检查最终分图报告。

### 8.1 校准集

使用 200 至 500 个有代表性的 train split `tile224` 张量，不得使用 validation/test
集调量化参数。校准集必须覆盖：

- 稀疏和密集小麦；
- 大、小麦穗和不同拍摄距离；
- 强光、阴影、偏色和轻微模糊；
- 合法负样本；
- 后续补拍的真实 OV5640 场景，包括实际镜头、曝光、白平衡和 LCD/环境干扰条件。

校准输入必须使用与固件完全相同的 tile224、颜色、数值范围和布局。提供 `calibration_manifest.csv`，记录图片 ID、tile_x/tile_y、来源、split、预处理张量路径和 SHA-256。

本轮建议先固定 300 个校准 tile；若显存、内存或转换工具限制，可在 200 至 500
范围内调整，但必须记录选择算法。校准样本只能来自 `train`，并按来源域、密度、
目标尺寸和负样本分层抽取；不得从 validation/test 挑出“效果好”的图片参与校准。
校准脚本必须确定随机种子并在 manifest 中保存选择顺序。

队友必须保存实际送入 TFLite 转换器 representative dataset 的 NHWC FP32 张量，
而不是只保存原始 JPEG/PNG。每个张量记录：

```text
shape: [1,224,224,3]
dtype: float32
color order: BGR；OpenCV 读取后保持 BGR，不做通道交换
range: [0,255]，不得除以 255
padding: BGR 114（三通道数值相同，仅边界确实需要补齐时）
sha256: <实际张量文件哈希>
```

生成后必须检查 TFLite INT8 的输入输出 dtype、shape、scale 和 zero point，并把
解释器查询结果保存为 `models/tflite_contract.json`。不要手工假定输入一定是
`q = pixel - 128`；只有实际模型报告 `scale=1.0, zero_point=-128` 时才能采用该
快速公式，否则固件必须使用实际量化参数。

### 8.2 量化后必须重新评估

分别评估：

```text
PyTorch FP32
ONNX FP32
TFLite FP32
TFLite INT8
```

所有模型使用同一个 validation/test 清单、同一组阈值、同一个 NMS 实现。不得为了掩盖 INT8 漏检而只降低 INT8 置信度阈值。

量化报告至少包含：

- 输入和输出量化 scale、zero point、dtype；
- 每层/每张量量化方式；
- INT8 相对 FP32 的 precision、recall、AP 和计数指标变化；
- 20 张 golden 图的候选数、NMS 后框数、框匹配率和计数差；
- 分数分布直方图或分位数；
- 所有异常饱和、全零、常量输出或 NaN/Inf 检查。

量化文件能够生成不代表量化成功。若 INT8 对 FP32 出现系统性漏检，必须如实标记失败，不能直接交给固件组。

### 8.3 队友必须给出明确的通过或失败结论

队友的报告必须包含一个机器可读 `qualification_summary.json`，至少列出：

```text
dataset_geometry_ok
split_leakage_check_ok
pytorch_to_onnx_ok
onnx_to_tflite_fp32_ok
tflite_fp32_to_int8_ok
global_tile_merge_ok
recommended_for_ruhmi
blocking_reasons[]
```

最低工程门建议固定为：

- PyTorch FP32、ONNX FP32、TFLite FP32 使用相同输入时，完整 validation 的
  AP50-95 和 recall 绝对差各不超过 `0.01`；
- TFLite INT8 相对 TFLite FP32 的 AP50-95 和 recall 绝对下降各不超过 `0.03`；
- TFLite INT8 的计数 MAE 相对 TFLite FP32 增加不超过 `1.0`；
- 20 张 golden 完整图中不得出现全零、常量、NaN/Inf、系统性框偏移或候选框
  数量数量级坍塌；
- 相同置信度 `0.25`、NMS IoU `0.45`、相同全局合并实现下完成比较，禁止只为
  INT8 单独调低阈值。

这些是进入 RUHMI 的工程门，不等于比赛精度一定足够。任何一项失败时，必须把
`recommended_for_ruhmi` 设为 `false` 并保留失败产物和日志；不要让项目组直接
部署一个已经在 PC 上证明量化失真的模型。

## 9. 队友必须交付的目录

建议按下列结构打包：

```text
YOLOX_Tiny_GWHD_224_handoff_YYYYMMDD/
|-- README.md
|-- commands.txt
|-- provenance.json
|-- environment/
|   |-- requirements-lock.txt
|   `-- system_info.txt
|-- source/
|   |-- yolox_git_commit.txt
|   |-- gwhd_yolox_tiny_tile224.py
|   |-- dataset_converter.py
|   |-- export_fixed_onnx.py
|   |-- convert_onnx_to_tflite.py
|   |-- evaluate_all_runtimes.py
|   `-- global_tile_merge.py
|-- dataset/
|   |-- train.json
|   |-- val.json
|   |-- test.json
|   |-- source_split_manifest.csv
|   |-- tile_manifest.csv
|   |-- box_assignment_manifest.csv
|   |-- geometry_audit.json
|   `-- dataset_hashes.json
|-- checkpoints/
|   |-- pretrained_yolox_tiny.sha256
|   |-- best_ckpt.pth
|   `-- latest_ckpt.pth
|-- models/
|   |-- yolox_tiny_gwhd_224_fp32.onnx
|   |-- yolox_tiny_gwhd_224_fp32.tflite
|   |-- yolox_tiny_gwhd_224_int8.tflite
|   `-- tflite_contract.json
|-- calibration/
|   |-- calibration_manifest.csv
|   |-- tensors/
|   `-- tensor_hashes.json
|-- metrics/
|   |-- pytorch_fp32.json
|   |-- onnx_fp32.json
|   |-- tflite_fp32.json
|   |-- tflite_int8.json
|   |-- runtime_comparison.json
|   |-- qualification_summary.json
|   `-- count_metrics.csv
|-- golden/
|   |-- manifest.csv
|   |-- inputs/
|   |-- raw_outputs/
|   `-- detections/
|-- plots/
`-- predictions/
```

`latest_ckpt.pth` 必须包含继续训练所需的优化器、学习率调度器、epoch 和 AMP 状态；只保存推理权重的文件不能称为“可恢复 checkpoint”。

`provenance.json` 至少记录：

- 模型家族、类别、输入输出合同；
- 数据集名称、split 和成员哈希；
- YOLOX 仓库 URL 和 Git commit；
- Python、PyTorch、CUDA/cuDNN、ONNX、ONNX Runtime、TensorFlow/onnx2tf 版本；
- 完整训练和导出命令；
- 随机种子；
- 预训练权重来源和 SHA-256；
- 每个交付文件的相对路径、字节数和 SHA-256；
- 训练是否完整结束、是否发生 resume、已知失败和限制。

## 10. 以下交付一律不接收

- 只提供 `best.pt` 或 `best.pth`；
- 只提供一个无法复现来源的 ONNX；
- 使用动态 batch 或动态图片尺寸；
- 把 NMS、TopK、图片解码或 resize 塞进网络；
- 不说明 RGB/BGR、归一化、letterbox 和 padding；
- 只提供 INT8，不提供对应 FP32 参考；
- 在 COCO 80 类模型上只改类别名称而不重新训练检测头；
- 把 224 模型静默改成 320；
- 直接把 `1024 x 1024` 拉伸为 `640 x 480`，造成目标形变；
- 跳过 `640 x 480` 参考画面，直接在 `1024 x 1024` 上按另一套位置切 tile；
- 把 challenge 无标注图片当负样本；
- 只报告训练 loss 或几张“看起来不错”的预测图；
- 为了让 INT8 看起来正常，单独降低其置信度阈值；
- 只生成 TFLite INT8 而不在完整 validation 上与 TFLite FP32 比较；
- 把已量化的 TFLite INT8 再交给 MERA 设置 `quantize=true` 做第二次 PTQ；
- 删除失败日志或声称“转换成功”但没有重新加载模型验证。

## 11. 项目组收到模型后的验收顺序

队友交付后，项目组按以下顺序验证，前一关失败就停止后续集成：

```text
交接包和 SHA-256 审计
  -> 数据划分、tile224 几何、类别和前处理审计
  -> PyTorch FP32 基线复现
  -> 固定 ONNX checker 和 ONNX Runtime 精度
  -> PyTorch/ONNX golden 对齐
  -> TFLite FP32 精度
  -> TFLite INT8 精度和计数保持率
  -> RUHMI/MERA 读取 TFLite INT8，target=npu，quantize=false
  -> Vela 编译 Ethos-U55 子图
  -> 在全新进程重新加载保存的 model.mera
  -> 对照官方分图：允许两个 CPU Focus 单元共 6 次 StridedSlice 调用及必要拷贝，卷积主干、PAFPN 和检测头必须进入 Ethos-U55
  -> 检查权重、激活、arena、OSPI 和 SDRAM 内存计划
  -> 集成完整帧冻结、12 分区顺序推理、全局 NMS 和按键恢复状态机
  -> Release 固件编译
  -> 板上推理时延、画框、计数和 20 轮按键循环测试
```

特别注意：

- MERA 命令返回成功不等于模型精度合格；
- 生成 C 代码不等于所有网络算子都在 NPU；
- Vela 子图内部显示零 CPU 算子，不代表子图外没有 `C_CODEGEN` 神经网络区域；
- 必须从磁盘重新加载生成的 `model.mera` 后再跑 golden，不能只验证编译前的内存对象；
- 首轮允许 CPU 执行官方 NPU 产物同样存在的两个 Focus 切片单元（固定参考产物
  共 6 次 `StridedSlice` 调用）、必要张量拷贝、
  图像前处理和有限后处理；不接受 CPU 执行任何卷积主干、PAFPN 或检测头。若出现
  额外 CPU Resize 或其他网络区域，必须单独报告算子、张量尺寸和实测耗时，并将
  `recommended_for_ruhmi` 保持为 `false`，直到项目组复核。

## 12. 队友提交前勾选清单

- [ ] 确认训练的是 YOLOX-Tiny，不是其他 Tiny 模型。
- [ ] 类别只有 `wheat_head`，类别 ID 和名称已审计。
- [ ] 使用随附 CSV 冻结的 2,735/339/348 源图划分，并核对 CSV SHA-256。
- [ ] 保留 46 张合法负样本，没有误用 challenge 图片。
- [ ] COCO 只作为预训练权重来源，所有监督训练、验证和测试均使用 GWHD。
- [ ] 已记录 COCO 预训练权重 URL、SHA-256 及加载时 missing/unexpected keys。
- [ ] `train` 按 `int(SHA256("42:" + source_id)[0:16], 16) mod 257` 生成 `crop_y`，validation/test 使用 `crop_y=128`，随后等比例缩放为 `640 x 480`。
- [ ] 图像和所有标注框使用同一裁剪/缩放变换，没有直接拉伸为 4:3。
- [ ] 首轮每张源图只生成一个冻结参考画面；几何审计复现 117,737 个参考框、27,443 个完全在裁剪外的框、231 个亚像素丢弃片段和 41,064 个 tile。
- [ ] 在源图划分之后生成专用 `tile224/stride179` 数据，没有跨 split 泄漏。
- [ ] 分区标签不要求中心点：正常保留可见面积至少 50% 的框，对全部 tile 均未达 50% 的框执行唯一最大可见比例 fallback。
- [ ] 几何审计复现 223,052 个局部标签分配、73,482 个低于 50% 的有效相交、3,642 个亚像素 tile 交集、2 个 fallback、0 个未分配参考框和单 tile 最大 21 个标签。
- [ ] 全局 AP 和计数使用去重后的 640 x 480 参考画面真值，不把重复 tile 标签重复计数。
- [ ] 记录 YOLOX Git commit、环境版本、随机种子和所有命令。
- [ ] 保存预训练权重来源和 SHA-256。
- [ ] 实际训练 batch shape 固定为 `224 x 224`。
- [ ] 提供 `best_ckpt.pth` 和可恢复的 `latest_ckpt.pth`。
- [ ] 提供 precision、recall、AP50、AP50-95 和计数 MAE/RMSE/bias。
- [ ] 提供密集、稀疏、小目标、遮挡、边缘和负样本子集结果。
- [ ] 固定 ONNX 为 `[1,3,224,224]`、batch 1、无动态轴、无 NMS。
- [ ] 实际检查并记录单类输出 shape，预期为 `[1,1029,6]`。
- [ ] PyTorch 和 ONNX 在完整 validation 分区及 golden 原图全局合并结果上对齐。
- [ ] 提供 TFLite FP32 和 TFLite INT8；若未生成，明确记录原因和日志。
- [ ] INT8 使用 200 至 500 张代表性校准图并提供 manifest。
- [ ] 校准样本只来自 train，保存了实际 NHWC FP32 representative tensor 和 SHA-256。
- [ ] 保存 TFLite INT8 实际 input/output dtype、shape、scale 和 zero point。
- [ ] FP32/INT8 使用相同阈值和 NMS 做完整比较。
- [ ] `qualification_summary.json` 给出每个门的布尔结果、失败原因和是否建议进入 RUHMI。
- [ ] 交付说明明确要求项目组对 TFLite INT8 使用 `target=npu, quantize=false`。
- [ ] 提供至少 30 张完整原图的全局合并可视化结果，不能只展示单个分区。
- [ ] `provenance.json` 包含所有交付文件的 SHA-256 和字节数。
- [ ] README 中写清成功项、失败项、已知限制和下一步，不隐瞒失败。

完成以上交付后，先把整个目录原样放入项目的 `models/` 目录。项目组会先做只读审计和 PC 端验证，不会未经验证就把生成代码写入固件。
