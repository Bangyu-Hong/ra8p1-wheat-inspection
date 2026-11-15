# RA8P1 小麦检测模型训练交接书

版本：2026-08-09，基于固件提交 `f3a2e60` 和已经冻结的六输出 QAT 候选失败证据。

## 1. 先看结论

这次需要的模型不能仅靠一条标准 Ultralytics YOLO 训练命令完成。

标准 YOLO 可以直接完成：

- 单类别 `wheat_head` 目标检测；
- 以 `imgsz=320` 训练普通 FP32 YOLOv8n；
- 使用已有标注计算 precision、recall 和 mAP；
- 导出固定尺寸、batch 1、FP32 ONNX。

标准 YOLO 不能直接保证：

- 训练图中的 fake quantization 与固定版本 MERA 最终量化参数一致；
- 两个上采样算子全部进入 Ethos-U55，而不是在 Cortex-M85 上执行；
- ONNX 恰好输出六个原始检测头张量；
- 保存后的 `model.mera` 仍保留足够的检测召回率；
- 模型的内存、算子分配和板上总延迟满足工程要求。

因此，完整工作不是“再训练一个 YOLO”，而是：

```text
算子小模型预检
  -> 自定义无 CPU-Resize 的 YOLOv8n-320
  -> 普通 FP32 训练
  -> 量化感知强化训练
  -> 固定 FP32 ONNX
  -> 六个原始检测头输出
  -> 固定 MERA INT8 编译
  -> 重新加载 model.mera 做精度门
  -> 通过后才进入 RA8P1 固件
```

当前状态仍是 `replacement-model-pending`。现有 QAT 模型没有集成到固件，也没有烧录到板上。

## 2. 为什么上一版没有通过

上一版不是因为 RA8P1 内存完全不够，也不是因为 YOLOv8n 完全不能编译。

已经得到的事实如下：

| 项目 | FP32 | 保存后的 MERA INT8 | 结论 |
|---|---:|---:|---|
| 固定 golden 候选数，置信度 `0.25` | 245 | 8 | 召回严重下降 |
| NMS 后检测数，IoU `0.45` | 32 | 2 | 只剩 2 个框 |
| NMS 索引重合数 | 32 个参考结果 | 0 | 检测内容已经明显漂移 |
| 最大分数 | 0.714540899 | 0.353022367 | 分数范围被明显压缩 |
| `model.mera` 大小 | 不适用 | 3,636,616 字节 | 模型可以生成和保存 |

六个原始输出都保持有限值，也没有全部变成零。因此这次不是简单的输出顺序错误，而是量化后检测头召回率严重损失。

编译器把三个区域分别分配了 156、27 和 135 个 NPU 算子，但仍留下四个 CPU C 区域。其中两个 CPU 区域执行：

- `256 x 10 x 10 -> 256 x 20 x 20` 的最近邻上采样；
- `128 x 20 x 20 -> 128 x 40 x 40` 的最近邻上采样。

这两个 `Resize/UpsamplingNearestNeighbor` 属于神经网络主干/颈部计算，不满足“学习得到的神经网络全部进入 NPU”的项目约束。

上一版 QAT 脚本对卷积权重和卷积输出加入了 fake quantization，但最终仍由 MERA 重新进行静态量化。训练期量化范围并不会原样传给 MERA，所以上一版只能称为“QAT 强化后的 FP32 模型”，不能称为 MERA 原生 QAT 模型。

## 3. 冻结的产品输入和显示关系

训练方不得自行改变以下产品输入合同：

```text
OV5640/CEU 完整帧：640 x 480 RGB565-BE
  -> 转为 RGB888
  -> 双线性缩放到 320 x 240
  -> 上方填充 40 行、下方填充 40 行，RGB 填充值 114
  -> 除以 255，得到 float32 RGB NCHW [1,3,320,320]
  -> MERA 根据生成的输入量化参数转为 INT8
```

模型输入 320 x 320 不等于摄像头只能采集 320 x 240。摄像头和 LCD 仍使用清晰的 640 x 480 主画面；只有按下 USER 键冻结一帧后，才从该 VGA 帧生成模型输入。检测框最后会解除上下 letterbox，并映射回 640 x 480 LCD 画面。

训练/验证预处理必须模拟上述 4:3 摄像头几何。仅用 1024 x 1024 原图直接缩放成 320 x 320，会让模型训练时看不到产品输入中固定的上下黑边/灰边分布，也不能代表真实摄像头视野。

## 4. 数据集合同

使用本地 Global Wheat Head Dataset 2020 V4 的 3,422 张有标注图像：

- 145,411 个有效 `wheat_head` 框；
- 46 张零目标图像是合法负样本，必须保留；
- 冻结划分：2,735 train、339 val、348 test；
- 冻结成员聚合 SHA-256：`d2c4d7dac7f2c91a0f3a75358e76c05215be22037a1b40d91a91f7200299a627`；
- 另外 1,276 张本地无标注挑战图像不能当作“没有小麦”的负样本，也不能用于监督训练或本地精度评分。

数据集已经有框标注，不需要重新逐张人工标注。需要做的是正确转换和审计：

1. 先按冻结的源图 ID 划分 train/val/test，禁止先切图再随机划分，避免同一源图泄漏到训练集和验证集。
2. 从 1024 x 1024 源图生成 4:3 训练视图时，同步裁剪、截断或剔除边界框。
3. 只有框中心仍在裁剪区、并且可见面积达到预先冻结比例的框才保留。
4. validation/test 使用确定性裁剪；随机裁剪和颜色增强只能用于 train。
5. 保存源图 ID、裁剪窗口、每个框变换前后坐标和最终标签哈希。

320 输入下，小麦穗仍属于小目标。现有审计显示，源图缩放到 320 后，中位框约为 `23.4 x 21.3` 像素，约 32.56% 的框至少有一边小于 16 像素。因此不能只看 mAP50，还必须检查小目标召回率、计数误差和密集区域漏检。

## 5. 推荐主路线：自定义 YOLOv8n-320

### 5.1 为什么继续从 YOLOv8n 开始

- 已有 FP32 模型在相同数据上达到约 `mAP50=0.918`、`mAP50-95=0.506` 的最佳材料化验证结果；
- 现有 host 端已经实现六输出组装、DFL、坐标恢复、阈值、全局 NMS 和 LCD 坐标映射；
- 当前模型的大部分卷积已经能够进入 Ethos-U55；
- YOLO11 的 C2PSA/MatMul 路线已经被当前工具链阻塞，不应回退到 YOLO11n/11s；
- YOLOv8s 会明显增加计算量和权重，不会自动修复 INT8 范围或 CPU Resize 问题。

### 5.2 必须修改的网络部分

不得把现有 ONNX 中的两个 Resize 节点简单删除或在导出后替换。上采样是特征融合的一部分，改变后必须重新训练。

首选待验证结构是把两处最近邻上采样替换为可学习的 `ConvTranspose2d`，保持 2 倍空间尺寸：

| 输入 | 输出 | 首个预检算子 |
|---|---|---|
| `[1,256,10,10]` | `[1,256,20,20]` | kernel 2、stride 2 的 ConvTranspose |
| `[1,128,20,20]` | `[1,128,40,40]` | kernel 2、stride 2 的 ConvTranspose |

瑞萨公开算子表列出了 ONNX `ConvTranspose`，量化表也列出了 MCU Ethos 的 A8W8 支持；但“前端支持”不等于“最终被 Vela 分配到 NPU”。正式训练前必须用完全相同形状和属性的两个极小 ONNX 做一次固定工具链编译预检，并查看所有 C/Vela 分区。

如果 ConvTranspose 仍落到 CPU，则停止该结构，不进行完整训练。下一选择应是无自顶向下上采样的轻量检测颈部，或直接切换 NanoDet/SSDLite，而不是继续猜测多种上采样写法。

### 5.3 固定检测头接口

训练期间可以继续使用标准 YOLO Detect loss，但部署导出必须停在三个尺度的最终回归卷积和分类卷积，输出顺序固定为：

```text
regression_s8  [1,64,40,40]
class_s8       [1, 1,40,40]
regression_s16 [1,64,20,20]
class_s16      [1, 1,20,20]
regression_s32 [1,64,10,10]
class_s32      [1, 1,10,10]
```

部署端按 NCHW 行优先展平，按 `s8 -> s16 -> s32` 合并，共 2,100 个候选位置。DFL softmax/投影、网格坐标重建、sigmoid、阈值和 NMS 在 Cortex-M85 上做有界后处理，不属于 CPU 运行整张神经网络。

六输出不是一种新的标签格式，也不需要把一张图分成六份训练。它只是把网络最后三个尺度的回归结果和分类结果直接交给板端，避免把不同数值范围过早拼在一起。

## 6. 训练方必须先做的三个小门

### Gate A：算子编译冒烟测试

在完整训练前交付两个静态 FP32 ONNX 小模型，分别包含上述两种真实形状的 ConvTranspose。使用固定 RUHMI `Release-2026-06-19`、MERA `2.6.0+pkg.4815`、Vela `4.2.0` 做 `--npu --quantize` 编译。

通过条件：

- 两个 ConvTranspose 都进入 Vela/NPU 区域；
- 不出现执行该 ConvTranspose 的 `CPU_C_CODEGEN`；
- 保存的 `.mera` 可以在新的 Interpreter 中重新加载；
- 相同输入下 FP32 与保存模型输出保持结构和数值可用。

### Gate B：一轮训练和导出冒烟测试

自定义 YAML 只训练 1 个 epoch，确认：

- forward、loss、反向传播、EMA 和 checkpoint 保存都正常；
- 标准 FP32 checkpoint 可重新加载；
- ONNX 是固定 `[1,3,320,320]`；
- 六个输出均由最终 Conv 直接产生；
- 图中没有 Resize、Upsample、MatMul、FP16、动态轴和嵌入 NMS；
- ONNX Runtime 能在 zero、uint8 ramp 和一张固定 wheat golden 上运行。

### Gate C：短 QAT 冒烟测试

只运行一个训练 batch 和一个验证 batch，确认 observer、fake quant、EMA、保存、恢复、材料化 FP32 和六输出 ONNX 全部可用。任何 NaN、Inf、observer 范围未初始化或 checkpoint 无法恢复都必须在完整训练前解决。

## 7. 完整训练阶段

### 7.1 FP32 基线

建议固定以下起点，实际命令和配置必须随交接包保存：

```text
architecture: custom YOLOv8n, no Resize/Upsample/MatMul
task: detect
classes: 1 (wheat_head)
imgsz: 320
batch: 8（RTX 3060 6 GiB 的保守起点，不得静默自动修改）
epochs: 150
patience: 30
optimizer: AdamW
seed: 42
deterministic: true
amp: true（只用于普通 FP32 训练阶段）
multi_scale: false
```

允许从 YOLOv8n 预训练权重加载形状兼容的 backbone/head 参数；新增加的 ConvTranspose 必须明确记录初始化方法和未加载参数列表。禁止把“部分加载成功”写成“完整继承权重”。

### 7.2 QAT 强化

普通 FP32 模型通过后，再进行 QAT 强化。建议以现有 20 epoch 配置为基线，而不是直接从随机权重做 QAT：

```text
epochs: 20
optimizer: AdamW
lr0: 1e-4
lrf: 0.1, cosine
amp: false
observer: 前 5 epoch 更新，第 6 epoch 起冻结
BN statistics: 前 10 epoch 更新，第 11 epoch 起冻结
fake quant: 全程启用
parameters: 不冻结
```

最低要求：

- 所有卷积权重进入 weight fake quant；
- 所有会进入 NPU 的卷积/激活边界都有 activation fake quant；
- 三个 class final Conv 和三个 regression final Conv 必须被覆盖；
- QAT checkpoint 中保留 observer/fake-quant 状态用于审计；
- 同时生成移除 hook/observer 后、可由标准 Ultralytics 重新加载的 FP32 checkpoint；
- 训练报告不得声称其量化参数就是 MERA 最终参数。MERA 仍会使用自己的 calibration 和量化流程。

当前 `qat_hardened_train.py` 可作为失败复现和基础实现参考，但不能原样作为新模型已经合格的证明。新结构、所有 fake-quant 插入点、EMA 行为和导出路径都要重新做 Gate C。

## 8. 验证要求

### 8.1 数据集精度

FP32 和 fake-quant 验证必须在同一个冻结 validation 集上分别报告：

- precision、recall、mAP50、mAP50-95；
- 小目标 AP/recall；
- 每图计数 MAE、RMSE、平均偏差；
- 负样本误报率；
- 密集、稀疏、遮挡、边缘截断场景的可视化；
- P50、P95、P99 和最大 PC 推理时间。

建议的训练交接门是：材料化 FP32 的 mAP50-95 相对新 FP32 基线下降不超过 0.03，recall 下降不超过 0.03。它只决定是否值得送入 MERA，不代表比赛最终精度已经合格。

### 8.2 固定 golden 结构门

必须使用项目现有固定 320 golden 输入，保持：

- confidence threshold `0.25`；
- NMS IoU threshold `0.45`；
- 不因为量化结果差而降低阈值；
- 保存每个输出的 dtype、shape、min/max、均值、标准差和 SHA-256；
- 保存候选索引、NMS 索引、框和分数。

一张 golden 只能证明接口和结构没有明显崩坏，不能代替完整 validation 精度。

### 8.3 MERA 保存模型门

MERA 编译完成不等于模型通过。必须在全新的进程/Interpreter 中通过 `ModelLoader.from_quantized_mera()` 重新加载磁盘上的 `model.mera`，再运行同一批 golden。

建议的 21 张固定 golden 工程门：

- 输出全部 finite，不得整头塌缩为常数；
- 对 FP32 中存在目标的图，INT8 不能系统性降到 `0.25` 以下；
- 以 IoU `0.5` 匹配 FP32 检测，检测召回率至少 80%；
- 相对 FP32 的每图计数误差中位数不超过 2，P95 不超过 `max(5, 20% FP32 count)`；
- 所有学习算子都在 NPU，CPU 只允许有界预处理、格式转换、输出反量化和 DFL/NMS 后处理；
- 生成内存计划能装入 RA8P1，之后还要经过 Release map 和板上小于 2 秒总延迟验收。

这些是工程部署门，不是对真实 ground truth 的最终比赛精度指标。

## 9. ONNX 导出合同

必须同时保留一个标准组合输出 FP32 ONNX 用于训练方复核，以及一个部署六输出 FP32 ONNX。部署模型必须满足：

- input：`images`, float32, `[1,3,320,320]`；
- batch 1、固定尺寸、RGB、NCHW；
- opset 固定并记录，当前参考为 opset 13；
- 不使用 dynamic axes；
- 不使用 FP16；
- 不嵌入 NMS、TopK 或端到端预处理；
- 不输出已经拼接的 `[1,5,2100]` 作为唯一部署接口；
- 不在输出处添加 Split/Identity 来绕过编译器；
- 六个输出直接由对应最终 Conv 产生；
- 不包含 Resize/Upsample/MatMul；
- ONNX checker 和静态 shape inference 通过。

如果仍从标准组合输出图派生六输出图，必须由守卫式脚本验证最终六个 Conv、权重哈希和消费者拓扑，只允许删除 DFL/坐标解码/拼接等后处理节点，不得删除任何学习得到的祖先节点。

## 10. 训练交接包清单

只发一个 `best.pt` 或一个 ONNX 不算完成交接。训练方必须提供：

1. `README.md`：环境、阶段、命令、成功/失败说明。
2. 自定义模型 YAML 和所有自定义 PyTorch/Ultralytics 模块源码。
3. 数据集 YAML，使用相对路径或可配置根目录，不能只留下训练电脑的 `D:\...` 绝对路径。
4. `dataset_manifest.csv`：split、源域、源图 ID、图片/标签哈希和裁剪信息。
5. 环境清单：Python、PyTorch、CUDA、Ultralytics、ONNX、ONNX Runtime 和 QAT 库版本。
6. 随机种子、所有训练参数、基础权重 SHA-256 和参数加载报告。
7. FP32 完整训练的 `best.pt`、`last.pt`、`args.yaml`、`results.csv`、曲线和验证图片。
8. 含 observer 的 QAT 审计 checkpoint，以及材料化后的标准 FP32 `best.pt`。
9. 标准组合输出 FP32 ONNX 和部署六输出 FP32 ONNX。
10. 每个模型和配置文件的 SHA-256。
11. Gate A/B/C 原始日志和结果 JSON。
12. validation 指标、计数指标、固定 golden 输出和失败图可视化。
13. 如果训练中断，提供可恢复的优化器/调度器状态；不得把剥离优化器的 `last.pt` 写成可 resume。

模型、数据集、训练输出和转换产物继续放在根目录 `models/`、`output/` 或外部目录，不提交到 Git。Git 只提交可复用的 `workspace/camera` 工具、测试、配置和文档。

## 11. 明确禁止的“修复”

- 不降低 `0.25` 阈值来制造量化通过；
- 不接受整个神经网络在 Cortex-M85 上运行的 CPU fallback；
- 不把普通 `yolo export int8=True` 或通用 QDQ INT8 ONNX 直接交给当前 RUHMI；
- 不把 YOLO11 C2PSA/MatMul 模型重新送入同一固定工具链；
- 不在旧 checkpoint 上仅修改 ONNX 输入尺寸并称为 320 专用模型；
- 不在导出后替换学习层而不重新训练；
- 不把训练日志中的一次最高值当作完整可复现证据；
- 不在 MERA 保存模型通过前集成生成的 C 源码、烧录固件或发布 NPU 结果；
- 不把 GWHD 的无标注挑战图像当作负样本；
- 不用阈值扫参、observer 反复试错或大量编译变体掩盖根因。

## 12. 非 YOLO 备选模型

| 路线 | 如何训练 | 优点 | 主要风险 | 建议级别 |
|---|---|---|---|---|
| NanoDet-Plus | MMDetection/NanoDet 工具链，用同一 wheat 框训练 | 瑞萨公开已测试模型列表包含 `nanodet-plus-m-1.5x_416`；属于真正的非 YOLO 检测器 | 416 版本较大，换成 320 后仍需重新验证精度、输出解码和 NPU 分配 | 第一备选 |
| MobileNetV2-SSDLite/SSD | torchvision、TensorFlow 或 MMDetection | 卷积/深度可分离卷积为主，INT8 生态成熟，输出结构较稳定 | anchor 匹配和密集小目标召回需要专门调参；瑞萨公开列表只证明部分 MobileNet/人员检测模型，不证明本模型 | 第二备选 |
| FOMO/中心热图 | Edge Impulse 或自建轻量全卷积网络，把框转换为中心热图 | 模型和输出很小，适合“计数优先” | 相邻麦穗中心容易粘连，通常不能提供可靠完整框；本地 FOMO 包目前只是 CPU TFLM 包，没有 NPU 和精度证据 | 计数降级路线 |
| 密度图计数 | MobileNet 小骨干加单通道密度图，框中心生成高斯监督 | 直接优化总数，对极密目标可能优于逐框 NMS | 很难在 LCD 上给每个麦穗画准确框；需要新的标签生成、loss、指标和 NPU 算子预检 | 只在比赛只看数量时考虑 |
| 图块分类/回归 | 把画面分块，预测每块是否有麦穗或数量 | 网络最简单、量化容易 | 边界重复、密集计数误差大，无法给可靠框 | 不作为主路线 |

不建议 DETR 类模型。它依赖 attention/MatMul，和已经失败的 YOLO11 C2PSA 风险方向相同；在当前 MCU/NPU 工具链和两秒目标下没有优势。

## 13. 瑞萨官方是否有小麦模型

截至 2026-08-09，在瑞萨公开资料中没有找到已经训练好的 RA8P1 小麦检测/计数模型。

公开的 [RUHMI application examples](https://github.com/renesas/ruhmi-framework-mcu/tree/main/application_examples) 包含：

- YOLO-Fastest 人脸检测；
- MobileNetV1 图像分类；
- 手掌检测、手部关键点和手势识别。

公开的 [Models Tested](https://github.com/renesas/ruhmi-framework-mcu/blob/main/docs/models_tested.md) 列出 EfficientNet、MobileNet、NanoDet-Plus、person detection 和 YOLO-Fastest 等 16 个模型，也没有 wheat。

Global Wheat Head Dataset 2020 V4 是公开研究数据集，Zenodo DOI 为 [10.5281/zenodo.4298502](https://doi.org/10.5281/zenodo.4298502)。它不是瑞萨训练好的模型，也不代表模型已经针对 RA8P1 量化。

瑞萨官方资料仍然很有价值：

- [RUHMI Framework MCU](https://github.com/renesas/ruhmi-framework-mcu) 证明 RA8P1/Ethos-U55 的正式编译和 C 代码生成路径；
- [Operator Support](https://github.com/renesas/ruhmi-framework-mcu/blob/main/docs/operator_support.md) 用于筛选模型结构，但不能替代实际 Vela 分配报告；
- 官方 NanoDet、YOLO-Fastest、MobileNet 和 palm 示例可以借鉴模型调用、内存布局、摄像头、LCD 和后处理框架，不能直接替代小麦训练。

如果瑞萨技术支持、比赛群或未公开培训包后续提供 wheat checkpoint，应先记录来源、版本、许可、输入输出、训练数据和哈希，再按本文相同 gate 检验，不能因“官方”二字跳过保存模型精度和板上验收。

## 14. 可行性评估

方案在工程上可行，但当前还不能说“已经可部署”。

支持可行性的证据：

- RA8P1 的 Ethos-U55 已经接受当前 YOLOv8n 的绝大多数卷积；
- 当前模型已经生成约 3.64 MB 的 `model.mera` 和 C 源码；
- 三个 Vela 区域均报告内部 CPU operator 为 0；
- 板上 16 MiB SDRAM 已完整通过测试；
- VGA 摄像头约 24.98 FPS、LCD 约 23.98 FPS 的实时基础链路已经通过；
- USER 键冻结/结果保持/恢复状态机已经完成多轮硬件测试；
- 产品只要求按键后处理一帧、总延迟小于 2 秒，不要求每帧实时推理。

尚未解决的风险：

- MERA INT8 后检测召回从 32 个 NMS 结果降到 2 个；
- 两个 Resize 仍在 CPU；
- 训练 QAT 参数不能直接控制 MERA 的最终量化参数；
- 320 输入对密集小麦是小目标任务，模型变轻可能继续损伤召回；
- 新结构的真实 SRAM、OSPI/MRAM 放置和板上延迟只能由最终编译和 Release 板测确认。

所以正确判断是：硬件能力和整体流水线足以支持继续开发；具体模型能否通过，必须由算子预检和保存模型精度门决定。它不是低风险“一次训练必成”，但也不是应当放弃 NPU 的不可行方案。

## 15. 推荐决策顺序

1. 先编译两个真实形状的 ConvTranspose 小模型，不开始完整训练。
2. 通过后制作无 Resize 的自定义 YOLOv8n YAML，运行 1 epoch 导出冒烟。
3. 冒烟通过后跑完整 FP32 训练，并与现有 YOLOv8n-320 基线比较。
4. FP32 合格后跑一次完整 QAT 强化，交付本文清单中的全部文件。
5. 项目端先做 FP32/golden 审查，再批准一个新的固定 MERA 候选 gate。
6. 保存模型通过精度和全 NPU 分配后，才生成固件模型后端并做 Release/板测。
7. 如果 ConvTranspose 预检失败，直接转 NanoDet-Plus 或 SSDLite；如果连续两个经过完整审查的新训练资产仍在 MERA 中严重掉召回，则停止 YOLO QAT 试错，优先评估 NanoDet 和计数热图路线。

这套顺序把最便宜、最快发现的工具链风险放在训练之前，也保留了现有 YOLO 后处理和固件基础的最大复用价值。

### 2026-08-09 执行结果

第 1 步已经使用真实特征和固定工具链完成，并判定失败。两种 shape 的 FP32 和保存后 MERA 数值检查均通过，但学习算子 `TFLiteTransposeConv2dBias` 在两次编译中都位于 `C_CODEGEN`；两个 Ethos-U55 区域仅执行前后 `TFLiteTranspose` 布局转换。

因此第 2 至第 6 步不得继续：不要制作或训练本文原先建议的 ConvTranspose YOLOv8n。完整证据在 `manifests/convtranspose_operator_gate.json`。当前下一条路线改为先审查 NanoDet-Plus，再审查 MobileNetV2-SSDLite；两者仍必须先做固定输入输出、算子分配、保存模型精度和内存门，不能因为出现在官方测试列表中就直接烧录。
