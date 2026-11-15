# YOLOX tile224 板端验收清单

## 当前状态

当前固件已完成主机两级数值门、240 tile 板端语义门、Release Clean Build、完整
烧录和冷启动门。板端 raw 输出受 Vela 固定点优化影响，不要求与 source TFLite
逐字节相同；固件同时保存 source/pre-Vela CRC 和已资格化的 RA8P1 board CRC。
当前剩余门是按最新 Release 镜像重新烧录后的真实 USER 键冻结/推理/恢复和 20 轮
稳定性测试；旧镜像的硬件结果不能证明新镜像的按键路径。

> 2026-08-15 现场状态：Code MRAM Secure 边界已经扩展到完整 1 MiB，NPU command
> stream 仍在 OSPI，链接器按物理 1 MiB MRAM 上限检查。J-Link V9.62 已对本页当前
> SREC 的内部、配置和 OSPI 内容完成接受/一致性检查；冷启动通过 SDRAM、双 CRC、
> NPU numeric gate、OV5640 和 LCD 门。最终 USER/20 轮验收仍待执行。

固定产物：

```text
workspace/camera/Release/camera.srec
SHA-256: 0C8C282110962433961A7073CA89DA5C89AC8AAC08BB505158C8619C7344E958

内部 linker end: 0x0200EA00（SREC 主程序数据最后字节 0x0200E837）
外部模型: 0x80000000..0x8046948F (4,625,552 bytes)
外部 command stream: 0x80469490..0x8046F457 (24,520 bytes)
OCTACLK: PLL2P 600 MHz /6 = 100 MHz
OM_n_SCLK: 50 MHz
```

该 SREC 同时包含内部程序和外部 QSPI 模型，不能只烧内部地址段。CPKHMI-RA8P1
本地 QSPI 示例确认 JDBG、J-Flash Lite 和 OSPI0 CS0 的 `0x80000000` 地址路径。

## 1. 烧录

1. 关闭占用 J-Link 的调试会话，通过板上 `JDBG` Type-C 口连接电脑。
2. 打开 J-Flash Lite，选择 `R7KA8P1KF_CPU0`、SWD、4000 kHz。
3. 加载上面的 `camera.srec`，先检查日志中同时出现 `0x02000000` 内部程序段和
   `0x80000000` 外部模型段。
4. 执行 `Program Device`，等待擦除、写入和校验全部成功。
5. 打开 J-Link CDC 串口：115200、8N1、无流控，然后复位或重新上电。

如果烧录器只显示内部程序段，立即停止；这会让启动时的外部模型 CRC 门失败。

## 2. 冷启动门

保存完整的上电串口日志。必须同时满足：

```text
[sdram] status=pass code=0 ...
[ai] backend=ruhmi-2.6-ethosu55-tile224
     model=yolox_tiny_gwhd_224_int8
     available=yes
     model_src=0x80000000
     model_dst=0x680e57d0
     src_crc=0x297619a7
     dst_crc=0x297619a7
     cmd_src=0x80469490
     cmd_dst=0x680df800
     cmd_src_crc=0x1b6f13e6
     cmd_dst_crc=0x1b6f13e6
     sanity_source_crc=0x6ace6aff
     sanity_board_crc=0x04e6d560
     sanity_actual_crc=0x04e6d560
     numeric_gate=pass
     requests=0
     npu_invocations=1
     init_status=0
```

实际 `[ai]` 是一行输出；上面为了阅读进行了换行。任一地址、存储 CRC、board
CRC、`available`、`numeric_gate`、`npu_invocations` 或状态不匹配都算板端门失败。
启动日志还必须在 LCD 初始化前出现
`[time] epoch=runtime max_service_gap_us=2000000 status=ok`。它表示耗时的
SDRAM、模型和摄像头上电初始化已结束，2 s 协作主循环服务间隔门从
新的运行期计时纪元开始，不会把正常的阻塞式 bring-up 误判成主循环卡死。

故障定位：

| 现象 | 优先检查 |
|---|---|
| `src_crc` 错误或为 0 | SREC 的 `0x80000000` 模型段没有烧入或校验失败 |
| `src_crc` 正确、`dst_crc` 错误 | SDRAM 复制、地址或内存稳定性 |
| 模型/command 双 CRC 正确、`sanity_actual_crc` 不等于 `sanity_board_crc` | 板端 Ethos-U/Vela 路径漂移，不得改 expected CRC 绕过 |
| `sanity_source_crc` 与 `sanity_board_crc` 不同 | 当前为已资格化的 Vela 固定点差异，不是单独故障 |
| `available=no` 或 `init_status!=0` | 查看同一份完整启动日志中的首个错误状态 |

## 3. 240 tile 板端语义门

2026-08-15 已把 20 个完整验证图像的 240 个冻结 tile 通过 JDBG 注入板端，并对
每个 tile 调用 `RunModel(false)`：

```text
calls:                 240/240 returned 0
source detections:     823
board detections:      835
IoU50 matches:         807
IoU50 recall:          98.06%
IoU50 precision:       96.65%
mean/min matched IoU:  0.8934 / 0.5264
Count MAE/max error:   1.0 / 3
```

该结果说明 `0x04E6D560` 不是随意替换的 CRC：它先被证明为可重复的板端签名，
再由真实 tile 的坐标恢复和全局 NMS 结果证明语义质量可接受。机器可读报告为
`output/yolox_tile224_board_gate_20260815/board_gate_report.json`。

## 4. 单轮功能门

1. 等实时画面稳定后按一次 USER 键。
2. 画面应冻结，依次完成 12 个 `224x224` tile 推理，显示检测框并保持结果。
3. 检查 `[ai-cycle]`：

```text
tiles=12/12 tile=224x224 stride=179 edge_x=416 edge_y=256
layout=NHWC type=INT8 order=BGR scale=1 zp=-128 bytes=150528
backend_status=0 result_valid=yes overlay_status=0 cycle_status=0
```

4. 记录 `preprocess_us`、`inference_us`、`postprocess_us`、`overlay_us` 和
   `total_us`；板端延迟预算以实测 `total_us <= 2000000` 为准。
5. 再按一次 USER 键，确认恢复实时画面。

没有目标时 `retained=0` 可以是正常结果；但 `tiles`、状态和 numeric gate 仍必须通过。

## 5. 20 轮稳定性门

连续执行 20 轮“按键冻结/推理/结果保持，再按键恢复”，共 40 次有效按键。最后
保存一行完整 `[ai-hb]`，要求：

```text
freeze=20 inference=20 results=20 resumes=20
requests=20 npu_invocations=241
snapshot_errors=0 release_errors=0 stale_release=0
acquire=20 release=20
```

`npu_invocations=241` 来自 1 次冷启动 golden 加 20 × 12 次 tile 推理。按键去抖
导致的额外物理触碰不按预期计数；若出现 `busy`，保留日志并确认最终有效事务计数
仍完全配对。还需观察：

- 实时画面每次都能恢复，无撕裂、卡死或旧检测框残留；
- 检测框位置与小麦穗一致，跨 tile 边界没有明显成倍重复框；
- 20 次 `[ai-cycle]` 全部 `tiles=12/12` 且状态为 0；
- 记录最小、典型和最大 `total_us`，任何一次超过 2 秒都保留原始日志。

## 6. 回传证据

请回传：

1. 从冷启动到 20 轮结束的原始串口文本；
2. 至少一张有小麦穗场景和一张无目标场景的屏幕照片；
3. 烧录日志中两个地址段及校验成功的部分；
4. 若失败，回传首次失败前后的完整日志，不要只截最后一行。

只有单轮功能门和 20 轮稳定性门也通过后，当前固件任务才可整体验收并归档。
