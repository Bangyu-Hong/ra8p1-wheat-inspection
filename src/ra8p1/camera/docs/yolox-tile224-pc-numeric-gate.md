# YOLOX tile224 PC 数值门

## 结论

2026-08-15 使用固定 RUHMI 2.6.0 Python 环境，分别运行冻结的源 INT8 TFLite
和正式 MCU 生成物中的 pre-Vela NPU 子图。全 `-128` NHWC 输入的两个 6174-byte
输出完全一致：

```text
CRC32:  0x6ACE6AFF
SHA256: a6819df1de201c1bd26d2123855f18eed875aa36cc67219ff7e8952d86d3b17f
range:  -92..19
unique: 110
```

这证明固件的 source/pre-Vela 参考 CRC `0x6ACE6AFF` 来自正确的源模型结果，
也证明 source TFLite 到 RUHMI pre-Vela 分区边界没有改变该输入的数值。板端
post-Vela 路径的独立确定性签名是 `0x04E6D560`；两个 CRC 表示不同执行边界，
不能互相覆盖或合并成一个“通用金值”。

额外的确定性 pattern 输入按 generated C 的四个 Focus `StridedSlice` 顺序送入
pre-Vela 子图，source/pre-Vela 输出也逐字节一致：

```text
CRC32:  0x5B7C997F
SHA256: 42c7cf1486f914e62076bd536e7127a52849f19194ba353a3452dd0f0cdf5959
changed: 0 / 6174
max_abs: 0
```

## 固定产物

```text
source model:
models/8.14/YOLOX_Tiny_GWHD_224_handoff_20260813/models/yolox_tiny_gwhd_224_int8.tflite
SHA256 7f6a058eb5b4cc4cc61942cdac55a6645fbffe9992731f1f6d72d897338f07f3

pre-Vela model:
output/yolox_tile224_ruhmi_external_20260814_01/model_NPU_external/deploy/build/MCU/
compilation/sub_0004__ARM_ETHOS_U55_C_CODEGEN/model.tflite
SHA256 c13c1477365d6fbbfb35eaa8feba3de26caf611db5b0c2004b504f4882092117
```

## 复现命令

从仓库根目录执行：

```powershell
& "F:/Renesas/RUHMI/venv-2.6.0/Scripts/python.exe" `
  workspace/camera/tools/run_yolox_tile224_numeric_gate.py `
  --source-model "models/8.14/YOLOX_Tiny_GWHD_224_handoff_20260813/models/yolox_tiny_gwhd_224_int8.tflite" `
  --pre-vela-model "output/yolox_tile224_ruhmi_external_20260814_01/model_NPU_external/deploy/build/MCU/compilation/sub_0004__ARM_ETHOS_U55_C_CODEGEN/model.tflite" `
  --output-report "output/yolox_tile224_numeric_gate_20260815/pc_numeric_gate.json"
```

脚本会先校验两个固定模型的 SHA-256、shape、dtype 和量化参数，再执行全 `-128`
与 Focus pattern 两组测试。输出报告路径必须不存在，防止覆盖旧证据。

## 覆盖边界

该门只覆盖源 TFLite 和 generated pre-Vela TFLite。普通 TensorFlow Lite runtime
无法执行 `model_vela.tflite` 中的 `ethos-u` custom op，因此该门不覆盖：

- Vela 优化后的 command stream；
- RA8P1 Ethos-U55 驱动与寄存器配置；
- OSPI 到 SDRAM 的运行时数据、arena 地址和 Cache 一致性；
- 板上真实 NPU 输出。

因此本报告本身仍只是主机边界证据，不单独构成板端通过。后续完成的 240 tile
板端门已经证明：虽然 post-Vela raw 输出与源边界不逐字节相同，但在同一
`conf=0.25`、全局 NMS IoU `0.45` 下，20 帧恢复后的检测达到 IoU50 recall
`98.06%`、precision `96.65%`、Count MAE `1.0`。机器可读证据位于
`output/yolox_tile224_board_gate_20260815/board_gate_report.json`。
