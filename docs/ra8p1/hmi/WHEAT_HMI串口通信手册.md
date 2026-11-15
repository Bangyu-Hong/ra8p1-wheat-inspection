# WHEAT 小麦智能检测系统：串口屏与单片机通信开发手册

> 文档版本：V2.1  
> 更新日期：2026-08-17  
> 串口屏工程：`WHEAT.HMI`  
> 适用主控：Renesas RA8P1（其他 MCU 也可按本文移植）  
> 串口屏型号：`TJC4827X243_011`，480×272，UTF-8

## 1. 文档目的与唯一协议基准

本文档用于指导单片机端完整实现：

1. 接收串口屏发送的自动巡检、采集识别和紧急停止命令。
2. 校验并保存小车、导轨参数。
3. 根据统一任务帧区分“自动巡检”和“仅导轨采集识别”。
4. 向小车底盘发送速度命令。
5. 使用 PUL/DIR/ENA 驱动导轨 42 步进电机。
6. 接收小车位置、速度和 IMU 航向角遥测。
7. 将传感器、任务状态、检测结果和样本编号回写串口屏。
8. 正确处理急停、串口超时、里程计漂移、限位和断电恢复。
9. 接收网络连接、断开和状态查询指令，并通过 ESP8266 完成 Station + DHCP 联网。

### 1.1 当前正式采用的上行协议

串口屏到主控统一使用 ASCII 文本帧：

```text
@A,巡检速度,单行距离,行间距,转向角度,巡检行数,扫描距离,拍照间隔,导轨速度,起点拍照\n
```

紧急停止：

```text
@E\n
```

网络控制：

```text
@N,SSID,PASSWORD\n
@D\n
@G\n
```

- `@N`：保存网络参数并连接；
- `@D`：断开当前 Wi-Fi；
- `@G`：查询并回写当前网络状态；
- 所有 HMI 上行帧均以单字节 LF（`0x0A`）结束，不追加 `FF FF FF`。

### 1.2 已废弃协议

以下协议不得再写入新代码：

```text
A5 CMD 5A
@Q
@AUTO#
@CARCFG,...#
@SCANCFG,...#
```

如果串口抓包仍出现 `@Q`、`@C@A` 或重复帧，应检查按钮的按下事件、弹起事件和定时器，删除旧发送代码。

## 2. 系统通信结构

```text
串口屏（参数和人机交互）
          │ UART-HMI，115200 8N1
          │ @A,...\n / @E\n / @N,...\n / @D\n / @G\n
          ▼
RA8P1 主控（协议解析、安全保护、任务状态机）
    ├── UART-CAR ──> 小车控制器：$vx,vy,wz\n
    ├── UART-CAR <── 小车遥测：$x,y,yaw,vx,vy,wz\r\n
    ├── GPIO/TIMER ─> 导轨驱动器：PUL/DIR/ENA
    ├── GPIO <─────── 左/右限位、原点开关
    ├── 传感器接口 <─ 温度、湿度、光照
    ├── UART-ESP ───> ESP8266：AT命令 + CR LF
    └── UART-HMI ───> 串口屏原生控件命令 + FF FF FF
```

关键原则：

- 串口屏只负责显示、参数输入和任务触发，不直接驱动电机。
- 主控是唯一的任务状态机和安全仲裁者。
- 串口屏到主控的 `@A` 是任务配置帧。
- 主控到小车的 `$vx,vy,wz\n` 是实时速度控制帧，两者不能混用。
- 导轨步进电机通过定时器脉冲驱动，不使用小车速度协议。
- HMI、小车控制器和 ESP8266 建议使用三个独立 UART。
- `@N/@D/@G` 只在 HMI 与主控之间使用；ESP8266 接收的是 AT 命令，不能把 `@N` 直接发送给 ESP8266。

## 3. 串口与硬件连接

### 3.1 串口屏通信参数

| 参数 | 当前值 |
|---|---:|
| 波特率 | `115200` |
| 数据位 | `8` |
| 校验位 | `None` |
| 停止位 | `1` |
| 流控 | `None` |
| 字符编码 | `UTF-8` |
| 串口协议模式 | `recmod=0` |

接线：

```text
串口屏 TX  -> MCU HMI_UART_RX
串口屏 RX  <- MCU HMI_UART_TX
串口屏 GND <-> MCU GND
```

必须确认双方 IO 电平兼容；不兼容时使用电平转换器。不要只连接 TX/RX 而不共地。

### 3.2 当前 Program.s 基础设置

```c
int sys0=0,sys1=0,sys2=0

int gPatSpd=30,gSegDist=120,gRowGap=60
int gTurnAng=90,gRowCnt=4
int gScanDist=300,gShotGap=50
int gRailSpd=20,gStartShot=1

baud=115200
dim=100
recmod=0
bkcmd=0
page 0
```

说明：

- `baud=115200` 是屏幕运行串口波特率。
- 下载窗口中的“下载使用的波特率”是下载握手参数，不等于运行协议定义。
- `bkcmd=0` 为正式运行模式，不主动返回每条命令结果。
- 调试时可临时发送 `bkcmd=3`，成功返回 `01 FF FF FF`。
- 不要保留原来的上电 `printh 00 00 00 ...`，否则主控会收到无意义二进制数据。

## 4. 串口屏页面和真实控件名称

当前工程包含四个业务页面：

```text
main
carset
scanset
netset
```

### 4.1 main 页面

| 功能 | 控件名 | 类型 | MCU 是否更新 |
|---|---|---|---|
| 页面标题 | `tTitle` | 文本 | 否 |
| 系统状态 | `tSysState` | 文本 | 是 |
| 麦穗数量 | `nCount` | 数字 | 是 |
| 温度 | `xTemp` | 虚拟浮点数 | 是 |
| 湿度 | `xHum` | 虚拟浮点数 | 是 |
| 相对光照 | `nLight` | 数字 | 是 |
| 检测状态标题 | `tDetTit` | 文本 | 否 |
| 检测状态背景 | `tDetectBg` | 文本背景 | 否 |
| 主任务状态 | `tDetectState` | 文本 | 是 |
| 副任务状态 | `tDetSub` | 文本 | 是 |
| 检测信息标题 | `tInfoTitle` | 文本 | 否 |
| 检测信息背景 | `tInfoBg` | 文本背景 | 否 |
| 图像质量 | `tQuality` | 文本 | 是 |
| 检测耗时 | `tTime` | 文本 | 是 |
| 样本编号 | `tSample` | 文本 | 是 |
| 上传状态 | `tUpload` | 文本 | 是 |
| 自动巡检 | `bPatrol` | 按钮 | 发送完整 `@A` |
| 采集识别 | `bSearch` | 按钮 | 发送小车参数全零的 `@A` |
| 小车参数入口 | `bCapture` | 按钮 | `page carset`，不发串口 |
| 扫描参数入口 | `bReturn` | 按钮 | `page scanset`，不发串口 |
| 紧急停止 | `bStop` | 按钮 | 发送 `@E\n` |

如果按钮显示文字仍为“小车遥控/导轨控制”，但工程没有独立 `car/rail` 页面，应将其实际功能理解为小车参数入口和扫描参数入口。

### 4.2 carset 页面

| 参数 | 控件名 | 单位 | 范围 | 步长 |
|---|---|---:|---:|---:|
| 巡检速度 | `nPatSpd` | `%` | 10～100 | 5 |
| 单行/单段距离 | `nSegDist` | `cm` | 20～500 | 10 |
| 行间距 | `nRowGap` | `cm` | 20～150 | 5 |
| 转向角度 | `nTurnAng` | `°` | 45～180 | 15 |
| 巡检行数 | `nRowCnt` | 行 | 1～20 | 1 |
| 保存状态 | `tSaveSt` | 文本 | 已保存/未保存 | — |

保存按钮 `bCarSave` 只把页面值复制到全局变量，不立即启动任务。

### 4.3 scanset 页面

| 参数 | 控件名 | 单位 | 范围 | 步长 |
|---|---|---:|---:|---:|
| 扫描距离 | `nScanDist` | `mm` | 50～300 | 10 |
| 拍照间隔 | `nShotGap` | `mm` | 10～150 | 10 |
| 拍照张数 | `nShotCnt` | 张 | 自动计算 | 不允许直接修改 |
| 导轨速度 | `nRailSpd` | `mm/s` | 5～50 | 5 |
| 起点拍照 | `btStartShot` | 0/1 | 关闭/开启 | — |
| 保存状态 | `tScanSt` | 文本 | 已保存/未保存 | — |

保存按钮 `bScanSave` 只把页面值复制到全局变量。`nShotCnt` 不放入任务帧，主控必须根据原始参数重新计算。

### 4.4 netset 页面

ESP8266 第一版固定使用 Station + DHCP，不设置静态 IP。若已有 `btDhcp`，应固定开启或作为只读状态显示。

| 功能 | 控件名 | 类型 | 必要设置 |
|---|---|---|---|
| 返回主页 | `bNetBack` | 按钮 | `page main` |
| 网络状态 | `tNetState` | 文本 | 全局，MCU异步更新 |
| Wi-Fi名称 | `tSSID` | 文本输入 | `txt_maxl=32`，绑定英文键盘 |
| Wi-Fi密码 | `tPwd` | 文本输入 | `txt_maxl=64`，`pw=1`，绑定英文键盘 |
| 当前IP | `tIPAddr` | 文本 | 全局，`txt_maxl>=20` |
| 信号强度 | `tRSSI` | 文本 | 全局 |
| 保存并连接 | `bNetConn` | 按钮 | 发送 `@N` |
| 断开网络 | `bNetDisc` | 按钮 | 发送 `@D` |
| 刷新状态 | `bNetGet` | 按钮 | 发送 `@G` |
| 网络帧缓冲 | `vaNetFrm` | 字符串变量 | **`txt_maxl=128`** |

实机验证发现：`vaNetFrm.txt_maxl` 太小时，SSID 和协议前缀会先占用缓冲区，密码只能追加剩余的少数字符。例如密码输入 `123456789` 却只发送 `1234`。因此必须把 `vaNetFrm.txt_maxl` 设置为 `128`。该值足以容纳32字节 SSID、63字节密码、帧头、分隔符和帧尾。

### 4.5 动态控件必要属性

以下控件建议设置 `vscope=全局`：

```text
nCount xTemp xHum nLight
tSysState tDetectState tDetSub
tQuality tTime tSample tUpload
tNetState tIPAddr tRSSI
```

建议文本最大长度：

```text
tDetectState.txt_maxl >= 24
tDetSub.txt_maxl      >= 32
tQuality.txt_maxl     >= 32
tTime.txt_maxl        >= 32
tSample.txt_maxl      >= 32
tUpload.txt_maxl      >= 32
tSSID.txt_maxl         = 32
tPwd.txt_maxl          = 64
tIPAddr.txt_maxl      >= 20
vaNetFrm.txt_maxl      = 128
```

当前扩充字库路径：

```text
字库\WHEAT_14px_utf8.zi
字库\WHEAT_16px_utf8.zi
字库\WHEAT_18px_utf8.zi
字库\WHEAT_20px_utf8.zi
```

## 5. HMI 到 MCU：统一任务协议

### 5.1 帧格式

```text
@A,PAT_SPD,SEG_DIST,ROW_GAP,TURN_ANG,ROW_CNT,SCAN_DIST,SHOT_GAP,RAIL_SPD,START_SHOT\n
```

| 序号 | 字段 | 类型 | 单位 | 合法范围 |
|---:|---|---|---|---:|
| 1 | `PAT_SPD` | 整数 | % | 10～100；仅扫描模式为0 |
| 2 | `SEG_DIST` | 整数 | cm | 20～500；仅扫描模式为0 |
| 3 | `ROW_GAP` | 整数 | cm | 20～150；仅扫描模式为0 |
| 4 | `TURN_ANG` | 整数 | ° | 45～180；仅扫描模式为0 |
| 5 | `ROW_CNT` | 整数 | 行 | 1～20；仅扫描模式为0 |
| 6 | `SCAN_DIST` | 整数 | mm | 50～300 |
| 7 | `SHOT_GAP` | 整数 | mm | 10～150 |
| 8 | `RAIL_SPD` | 整数 | mm/s | 5～50 |
| 9 | `START_SHOT` | 整数 | 布尔 | 0或1 |

协议要求：

- 帧头严格为 ASCII `@A,`。
- 参数之间只有英文逗号，不允许空格。
- 帧尾为单字节 LF，即 `0x0A`。
- 推荐 MCU 同时兼容并忽略 LF 前的可选 CR `0x0D`。
- 每帧只允许 9 个整数参数。
- 当前 V2.1 不使用校验和；UART 线路应短、可靠并具有超时保护。

### 5.2 自动巡检示例

```text
@A,30,120,60,90,4,300,50,20,1\n
```

含义：

```text
小车速度 30%
每行行驶 120cm
换行距离 60cm
转向 90°
共巡检 4行
导轨扫描 300mm
每 50mm 拍照
导轨速度 20mm/s
起点拍照
```

### 5.3 仅采集识别示例

```text
@A,0,0,0,0,0,300,50,20,1\n
```

识别规则：

```text
前5个小车字段全部为0 -> 仅导轨扫描
前5个小车字段全部合法非0 -> 自动巡检
前5个字段部分为0 -> 非法帧，拒绝执行
```

仅扫描模式必须先执行 `Car_Stop()`，再启动导轨。不能因为小车参数为零而进行除法、里程计算或启动小车状态机。

### 5.4 紧急停止

```text
@E\n
```

对应字节：

```text
40 45 0A
```

收到后必须立即：

1. 小车速度清零并发送 `$0.0,0.0,0.0\n`。
2. 关闭导轨脉冲输出。
3. 取消拍照、识别和自动巡检的后续步骤。
4. 设置急停锁存标志。
5. 更新 HMI 为红色急停状态。

当前协议没有远程解除急停命令。建议通过物理复位或明确的本地复位操作解除，不能由新的 `@A` 自动解除急停。

### 5.5 网络控制协议

连接帧：

```text
@N,SSID,PASSWORD\n
```

示例：

```text
@N,Wheat_WiFi,Wheat123456\n
```

字段限制：

| 字段 | 最大长度 | 当前约束 |
|---|---:|---|
| `SSID` | 32字节 | 不允许英文逗号、CR、LF |
| `PASSWORD` | 63字节 | 不允许英文逗号、CR、LF；当前版本不处理开放网络空密码 |

当前 V2.1 使用英文逗号分隔，不支持字段转义。若现场 Wi-Fi 名称或密码必须包含逗号，应升级为长度字段协议，不能直接沿用本帧。

断开和查询：

```text
@D\n
@G\n
```

串口屏属性必须满足：

```text
tSSID.txt_maxl=32
tPwd.txt_maxl=64
vaNetFrm.txt_maxl=128
```

`bNetConn` 弹起事件：

```c
if(tSSID.txt!="")
{
  if(tPwd.txt!="")
  {
    vaNetFrm.txt="@N,"
    vaNetFrm.txt+=tSSID.txt
    vaNetFrm.txt+=","
    vaNetFrm.txt+=tPwd.txt
    prints vaNetFrm.txt,0
    printh 0A

    tNetState.txt="连接中"
    tNetState.pco=65098
  }
}
```

`bNetDisc` 弹起事件：

```c
prints "@D",0
printh 0A
tNetState.txt="断开中"
tNetState.pco=65098
```

`bNetGet` 弹起事件：

```c
prints "@G",0
printh 0A
```

协议字符串中不得在逗号后增加空格。正确结果为：

```text
@N,yanqin,123456789\n
```

而不是：

```text
@N, yanqin, 123456789\n
```

## 6. 串口屏事件代码参考

### 6.1 carset 保存按钮

```c
gPatSpd=nPatSpd.val
gSegDist=nSegDist.val
gRowGap=nRowGap.val
gTurnAng=nTurnAng.val
gRowCnt=nRowCnt.val

tSaveSt.txt="已保存"
tSaveSt.pco=13971
```

### 6.2 scanset 保存按钮

```c
gScanDist=nScanDist.val
gShotGap=nShotGap.val
gRailSpd=nRailSpd.val
gStartShot=btStartShot.val

tScanSt.txt="已保存"
tScanSt.pco=13971
```

### 6.3 自动巡检 bPatrol 弹起事件

要求 `main` 页面存在字符串变量 `vaTmp` 和 `vaFrame`，其中 `vaFrame.txt_maxl>=100`。

```c
covx gPatSpd,vaTmp.txt,0,0
vaFrame.txt="@A,"+vaTmp.txt

covx gSegDist,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gRowGap,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gTurnAng,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gRowCnt,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gScanDist,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gShotGap,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gRailSpd,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gStartShot,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

prints vaFrame.txt,0
printh 0A
```

### 6.4 采集识别 bSearch 弹起事件

```c
vaFrame.txt="@A,0,0,0,0,0"

covx gScanDist,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gShotGap,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gRailSpd,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

covx gStartShot,vaTmp.txt,0,0
vaFrame.txt+=","+vaTmp.txt

prints vaFrame.txt,0
printh 0A
```

### 6.5 紧急停止 bStop 按下事件

```c
prints "@E",0
printh 0A
```

注意：

- `bPatrol` 和 `bSearch` 的按下事件必须为空，避免一按产生两帧。
- `bStop` 放在按下事件，不等待弹起。
- 保存按钮只保存参数，不发送任务帧。
- `bCapture`、`bReturn` 仅用于页面跳转，不发送 `@Q`。

## 7. MCU 上行接收器设计

### 7.1 数据结构

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HMI_RX_FRAME_MAX       160U
#define HMI_SSID_MAX            32U
#define HMI_WIFI_PASSWORD_MAX   63U

typedef enum
{
    HMI_MSG_NONE = 0,
    HMI_MSG_AUTO,
    HMI_MSG_SCAN_ONLY,
    HMI_MSG_ESTOP,
    HMI_MSG_NET_CONNECT,
    HMI_MSG_NET_DISCONNECT,
    HMI_MSG_NET_STATUS,
    HMI_MSG_INVALID
} hmi_message_type_t;

typedef struct
{
    uint16_t patrol_speed_pct;
    uint16_t segment_distance_cm;
    uint16_t row_gap_cm;
    uint16_t turn_angle_deg;
    uint16_t row_count;
    uint16_t scan_distance_mm;
    uint16_t shot_gap_mm;
    uint16_t rail_speed_mm_s;
    uint8_t  start_shot;
} hmi_task_config_t;

typedef struct
{
    char ssid[HMI_SSID_MAX + 1U];
    char password[HMI_WIFI_PASSWORD_MAX + 1U];
} hmi_network_config_t;

typedef struct
{
    hmi_message_type_t type;
    hmi_task_config_t config;
    hmi_network_config_t network;
} hmi_message_t;

typedef struct
{
    char buffer[HMI_RX_FRAME_MAX];
    size_t length;
    bool collecting;
} hmi_rx_parser_t;
```

### 7.2 参数解析和校验

```c
#include <stdlib.h>
#include <string.h>

static bool in_range(long value, long min_value, long max_value)
{
    return (value >= min_value) && (value <= max_value);
}

static bool parse_nine_u16(const char *text, uint16_t value[9])
{
    const char *cursor = text;

    for (size_t i = 0; i < 9U; ++i)
    {
        if ((*cursor < '0') || (*cursor > '9'))
        {
            return false;
        }

        char *end = NULL;
        long parsed = strtol(cursor, &end, 10);

        if ((end == cursor) || (parsed < 0L) || (parsed > 65535L))
        {
            return false;
        }

        value[i] = (uint16_t) parsed;

        if (i < 8U)
        {
            if (*end != ',') return false;
            cursor = end + 1;
        }
        else
        {
            if (*end != '\0') return false;
        }
    }

    return true;
}

static hmi_message_type_t validate_task(const uint16_t v[9],
                                        hmi_task_config_t *cfg)
{
    const bool car_all_zero =
        (v[0] == 0U) && (v[1] == 0U) && (v[2] == 0U) &&
        (v[3] == 0U) && (v[4] == 0U);

    const bool car_all_valid =
        in_range(v[0], 10, 100) &&
        in_range(v[1], 20, 500) &&
        in_range(v[2], 20, 150) &&
        in_range(v[3], 45, 180) &&
        in_range(v[4], 1, 20);

    const bool scan_valid =
        in_range(v[5], 50, 300) &&
        in_range(v[6], 10, 150) &&
        in_range(v[7], 5, 50) &&
        (v[8] <= 1U);

    if (!scan_valid || (!car_all_zero && !car_all_valid))
    {
        return HMI_MSG_INVALID;
    }

    cfg->patrol_speed_pct    = v[0];
    cfg->segment_distance_cm = v[1];
    cfg->row_gap_cm          = v[2];
    cfg->turn_angle_deg      = v[3];
    cfg->row_count           = v[4];
    cfg->scan_distance_mm    = v[5];
    cfg->shot_gap_mm         = v[6];
    cfg->rail_speed_mm_s     = v[7];
    cfg->start_shot          = (uint8_t) v[8];

    return car_all_zero ? HMI_MSG_SCAN_ONLY : HMI_MSG_AUTO;
}

static hmi_message_type_t parse_network_config(
    const char *payload,
    hmi_network_config_t *network)
{
    const char *separator = strchr(payload, ',');
    if ((separator == NULL) || (strchr(separator + 1, ',') != NULL))
    {
        return HMI_MSG_INVALID;
    }

    const size_t ssid_length = (size_t) (separator - payload);
    const size_t password_length = strlen(separator + 1);

    if ((ssid_length == 0U) || (ssid_length > HMI_SSID_MAX) ||
        (password_length < 8U) ||
        (password_length > HMI_WIFI_PASSWORD_MAX))
    {
        return HMI_MSG_INVALID;
    }

    memcpy(network->ssid, payload, ssid_length);
    network->ssid[ssid_length] = '\0';
    memcpy(network->password, separator + 1, password_length);
    network->password[password_length] = '\0';

    return HMI_MSG_NET_CONNECT;
}

static hmi_message_type_t parse_complete_frame(const char *frame,
                                               hmi_message_t *message)
{
    if (strcmp(frame, "@E") == 0)
    {
        return HMI_MSG_ESTOP;
    }

    if (strcmp(frame, "@D") == 0)
    {
        return HMI_MSG_NET_DISCONNECT;
    }

    if (strcmp(frame, "@G") == 0)
    {
        return HMI_MSG_NET_STATUS;
    }

    if (strncmp(frame, "@N,", 3U) == 0)
    {
        return parse_network_config(frame + 3, &message->network);
    }

    if (strncmp(frame, "@A,", 3U) != 0)
    {
        return HMI_MSG_INVALID;
    }

    uint16_t value[9];
    if (!parse_nine_u16(frame + 3, value))
    {
        return HMI_MSG_INVALID;
    }

    return validate_task(value, &message->config);
}
```

### 7.3 逐字节接收状态机

```c
bool hmi_rx_feed(hmi_rx_parser_t *parser,
                 uint8_t byte,
                 hmi_message_t *message)
{
    if (byte == (uint8_t) '@')
    {
        parser->length = 0U;
        parser->collecting = true;
        parser->buffer[parser->length++] = '@';
        return false;
    }

    if (!parser->collecting)
    {
        return false;
    }

    if (byte == (uint8_t) '\r')
    {
        return false;
    }

    if (byte == (uint8_t) '\n')
    {
        parser->buffer[parser->length] = '\0';
        parser->collecting = false;

        message->type = parse_complete_frame(parser->buffer, message);
        return true;
    }

    if (parser->length >= (HMI_RX_FRAME_MAX - 1U))
    {
        parser->length = 0U;
        parser->collecting = false;
        message->type = HMI_MSG_INVALID;
        return true;
    }

    parser->buffer[parser->length++] = (char) byte;
    return false;
}
```

注意：UART 中断中只应把字节放入环形缓冲区，不能在中断中调用 `strtol`、控制电机、写 Flash 或发送大量 HMI 文本。

### 7.4 消息分发

```c
void hmi_dispatch(const hmi_message_t *message)
{
    if (message->type == HMI_MSG_ESTOP)
    {
        emergency_stop_latch();
        return;
    }

    if (emergency_stop_is_latched())
    {
        hmi_show_error("急停未复位");
        return;
    }

    switch (message->type)
    {
        case HMI_MSG_AUTO:
            task_start_auto(&message->config);
            break;

        case HMI_MSG_SCAN_ONLY:
            Car_Stop();
            task_start_scan_only(&message->config);
            break;

        case HMI_MSG_NET_CONNECT:
            network_connect_request(message->network.ssid,
                                    message->network.password);
            break;

        case HMI_MSG_NET_DISCONNECT:
            network_disconnect_request();
            break;

        case HMI_MSG_NET_STATUS:
            network_report_status();
            break;

        case HMI_MSG_INVALID:
            Car_Stop();
            Rail_Stop();
            hmi_show_error("参数错误");
            break;

        default:
            break;
    }
}
```

`network_connect_request()` 不应在 UART 中断里阻塞等待 ESP8266。它只复制经过校验的参数并启动网络状态机；状态机再依次发送 `AT`、`AT+CWMODE=1`、`AT+CWDHCP=1,1` 和 `AT+CWJAP`。调试日志不得输出明文密码。

## 8. 小车底盘协议

### 8.1 MCU 到小车控制器

```text
$vx,vy,wz\n
```

| 字段 | 单位 | 正方向 |
|---|---|---|
| `vx` | m/s | 正值前进 |
| `vy` | m/s | 正值左移 |
| `wz` | rad/s | 正值逆时针 |

示例：

```text
$0.5,0.0,0.0\n       前进
$0.0,0.3,0.0\n       左移
$0.0,0.0,1.57\n      原地逆时针旋转
$0.0,0.0,0.0\n       停止
```

### 8.2 小车到 MCU 遥测

```text
$x,y,yaw,vx,vy,wz\r\n
```

周期：50ms（20Hz）。

| 字段 | 单位 | 说明 |
|---|---|---|
| `x` | m | X坐标 |
| `y` | m | Y坐标 |
| `yaw` | rad | IMU/融合航向角 |
| `vx` | m/s | 当前X线速度 |
| `vy` | m/s | 当前Y线速度 |
| `wz` | rad/s | 当前角速度 |

MCU 必须记录最后一次有效遥测时间。如果超过 200ms 未收到有效遥测，应停止小车并暂停任务。

### 8.3 根据巡检速度百分比生成速度

设系统允许的最大巡检速度为 `CAR_MAX_PATROL_MPS`：

```c
float vx = CAR_MAX_PATROL_MPS * config.patrol_speed_pct / 100.0f;
```

实际最大速度必须在现场标定，不应直接把 `30%` 理解为 `0.30m/s`。

### 8.4 行驶距离判断

每段开始时保存：

```c
start_x = odom.x;
start_y = odom.y;
start_yaw = odom.yaw;
```

沿起始航向的进度：

```c
float dx = odom.x - start_x;
float dy = odom.y - start_y;
float progress_m = dx * cosf(start_yaw) + dy * sinf(start_yaw);
```

当：

```c
progress_m >= config.segment_distance_cm / 100.0f
```

停止本行运动。不要只判断全局 `x`，否则换向后会失效。

## 9. 导轨步进电机控制

### 9.1 硬件接口

```text
MCU TIMER/PWM -> 驱动器 PUL
MCU GPIO      -> 驱动器 DIR
MCU GPIO      -> 驱动器 ENA
左限位开关     -> MCU GPIO/外部中断
右限位开关     -> MCU GPIO/外部中断
```

禁止 MCU GPIO 直接连接 42 步进电机绕组。

### 9.2 脉冲换算

```text
pulses_per_mm = motor_steps_per_rev × microstep / screw_lead_mm
target_pulses = distance_mm × pulses_per_mm
pulse_frequency_hz = speed_mm_s × pulses_per_mm
```

示例：1.8°电机、16细分、4mm导程：

```text
motor_steps_per_rev = 200
pulses_per_mm = 200 × 16 / 4 = 800 pulse/mm
20mm/s -> 16000 pulse/s
```

实际必须以驱动器细分拨码和丝杆真实导程为准。

### 9.3 拍照张数和位置

```c
uint16_t segment_count =
    (scan_distance_mm + shot_gap_mm - 1U) / shot_gap_mm;

uint16_t shot_count = segment_count + (start_shot ? 1U : 0U);
```

必须先确认 `shot_gap_mm>0`。

若 `300mm/50mm` 且起点拍照：

```text
0, 50, 100, 150, 200, 250, 300mm，共7张
```

若不能整除，最后一个位置固定为扫描终点。例如 `300/80`：

```text
0, 80, 160, 240, 300mm，共5张
```

### 9.4 回零要求

每次自动任务首次扫描前建议执行：

1. 低速向原点运动。
2. 触发原点限位立即停止。
3. 当前位置设为 0。
4. 必要时反向退出开关并二次低速靠近，提高重复精度。
5. 回零超时或运动距离超限时进入故障。

## 10. 自动巡检状态机

推荐状态：

```c
typedef enum
{
    TASK_IDLE = 0,
    TASK_CAR_MOVE,
    TASK_CAR_STABILIZE,
    TASK_RAIL_HOME,
    TASK_RAIL_SCAN,
    TASK_DETECT,
    TASK_SAVE_SAMPLE,
    TASK_CHANGE_ROW,
    TASK_COMPLETE,
    TASK_ERROR,
    TASK_ESTOP
} task_state_t;
```

### 10.1 自动巡检流程

```text
接收合法完整 @A
-> 小车按单行距离移动
-> 停车并等待稳定
-> 导轨回零
-> 分段拍照
-> YOLO识别和汇总
-> 保存样本
-> 未达到行数：换行并继续
-> 达到 row_count：导轨回零、停车、任务完成
```

当前 V2.1 将 `nSegDist` 定义为每行巡检距离。任务在完成 `nRowCnt` 行后正常停止。

### 10.2 采集识别流程

```text
接收小车前5字段全0的 @A
-> 强制小车速度为0
-> 导轨回零
-> 执行一次完整分段扫描
-> 识别并保存一个样本
-> 导轨回零
-> 返回空闲
```

### 10.3 正常停止和异常停止条件

正常停止：

- 自动巡检完成规定行数。
- 仅采集识别完成一次导轨扫描并回零。

立即停止：

- 收到 `@E\n`。
- 左/右限位方向与命令冲突。
- 导轨回零超时。
- 小车遥测超过 200ms 未更新。
- 参数帧非法。
- 电机、摄像头或识别模块报告严重错误。
- 横向偏差或航向偏差超过安全阈值。

## 11. MCU 到串口屏：原生控件命令

### 11.1 命令结束符

每条命令必须追加三个原始字节：

```text
FF FF FF
```

不是字符串 `"FF FF FF"`，也不能使用换行代替。

### 11.2 发送函数

```c
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HMI_TX_MAX 160U

bool hmi_uart_write_blocking(const uint8_t *data, size_t length);

bool hmi_send_command(const char *format, ...)
{
    uint8_t frame[HMI_TX_MAX];
    va_list args;

    va_start(args, format);
    int length = vsnprintf((char *) frame,
                           HMI_TX_MAX - 3U,
                           format,
                           args);
    va_end(args);

    if ((length < 0) || ((size_t) length > HMI_TX_MAX - 4U))
    {
        return false;
    }

    frame[length]     = 0xFFU;
    frame[length + 1] = 0xFFU;
    frame[length + 2] = 0xFFU;

    return hmi_uart_write_blocking(frame, (size_t) length + 3U);
}
```

FSP 使用 `R_SCI_UART_Write()` 时，发送缓冲区必须保持有效直到收到 `UART_EVENT_TX_COMPLETE`。建议实现静态发送队列，不要连续复用栈缓冲区启动异步发送。

### 11.3 传感器数据

| 数据 | 控件命令 | 换算 |
|---|---|---|
| 麦穗数量126 | `main.nCount.val=126` | 整数 |
| 温度25.6℃ | `main.xTemp.val=256` | 温度×10 |
| 湿度60.5% | `main.xHum.val=605` | 湿度×10 |
| 光照78% | `main.nLight.val=78` | 0～100整数 |

要求：

```text
xTemp.vvs1=1
xHum.vvs1=1
```

示例：

```c
void hmi_update_environment(int32_t temp_x10,
                            int32_t hum_x10,
                            uint32_t light_pct)
{
    if (temp_x10 < -400) temp_x10 = -400;
    if (temp_x10 >  850) temp_x10 =  850;
    if (hum_x10 < 0)     hum_x10 = 0;
    if (hum_x10 > 1000)  hum_x10 = 1000;
    if (light_pct > 100U) light_pct = 100U;

    hmi_send_command("main.xTemp.val=%ld", (long) temp_x10);
    hmi_send_command("main.xHum.val=%ld", (long) hum_x10);
    hmi_send_command("main.nLight.val=%lu", (unsigned long) light_pct);
}
```

建议传感器显示刷新周期 500～1000ms，仅在数值变化时发送，避免占满串口。

### 11.4 状态更新

当前颜色值：

| 用途 | RGB565十进制 |
|---|---:|
| 正常绿色 | `13971` |
| 数据浅绿色 | `30519` |
| 次级灰色 | `44437` |
| 黄色提示 | `65098` |
| 红色故障/急停 | `62123` |
| 柔和白色 | `63422` |

示例：

```c
void hmi_show_scanning(uint16_t current, uint16_t total)
{
    hmi_send_command("main.tSysState.txt=\"系统运行\"");
    hmi_send_command("main.tSysState.pco=13971");
    hmi_send_command("main.tDetectState.txt=\"分段扫描中\"");
    hmi_send_command("main.tDetectState.pco=13971");
    hmi_send_command("main.tDetSub.txt=\"%u / %u\"", current, total);
}

void hmi_show_error(const char *message_utf8)
{
    hmi_send_command("main.tSysState.txt=\"系统故障\"");
    hmi_send_command("main.tSysState.pco=62123");
    hmi_send_command("main.tDetectState.txt=\"%s\"", message_utf8);
    hmi_send_command("main.tDetectState.pco=62123");
}
```

`message_utf8` 必须是本机可信、长度受限的 UTF-8 文本，不能直接把外部未校验字符串拼接为 HMI 指令。

### 11.5 检测结果和样本显示

```c
void hmi_show_result(uint32_t count,
                     uint32_t elapsed_ms,
                     const char *sample_id,
                     bool uploaded)
{
    if (count > 999U) count = 999U;

    hmi_send_command("main.nCount.val=%lu", (unsigned long) count);
    hmi_send_command("main.tDetectState.txt=\"检测完成\"");
    hmi_send_command("main.tDetectState.pco=13971");
    hmi_send_command("main.tDetSub.txt=\"结果已更新\"");
    hmi_send_command("main.tQuality.txt=\"质量：良好\"");
    hmi_send_command("main.tTime.txt=\"耗时：%lu ms\"",
                     (unsigned long) elapsed_ms);
    hmi_send_command("main.tSample.txt=\"样本：%s\"", sample_id);

    if (uploaded)
    {
        hmi_send_command("main.tUpload.txt=\"上传：已上传\"");
        hmi_send_command("main.tUpload.pco=13971");
    }
    else
    {
        hmi_send_command("main.tUpload.txt=\"上传：未上传\"");
        hmi_send_command("main.tUpload.pco=44437");
    }
}
```

中文源文件和编译器执行字符集必须使用 UTF-8。若工具链处理中文字符串不稳定，可将固定中文提示预置在 HMI 中，只由 MCU 更新数字和状态码。

### 11.6 发送节流

- 每条 HMI 命令必须等待 UART 发送完成。
- 命令队列建议间隔 20～100ms；调试阶段使用 100ms 最稳妥。
- 不要在 1ms 定时器中持续发送相同文本。
- `bkcmd=3` 调试返回也会占用接收带宽，正式运行恢复 `bkcmd=0`。

## 12. 样本 A001 的记录规范

### 12.1 编号含义

`A001` 表示一个完整采样点，而不是一张照片。一次导轨扫描的多张图片命名为：

```text
A001_01.jpg
A001_02.jpg
...
A001_07.jpg
```

下一采样点使用 `A002`。

### 12.2 推荐记录字段

```csv
sample_id,time,row,segment,x,y,yaw,temp,hum,light,wheat_count,quality,infer_ms,upload
A001,2026-08-16 21:10:30,1,2,1.234,0.567,0.785,25.6,60.5,78,126,good,328,0
```

建议同时保存：

- 任务离散位置：第几行、第几段。
- 融合后的 `x/y/yaw`。
- 环境传感器快照。
- 麦穗数量、质量、推理耗时。
- 图片路径和上传状态。

### 12.3 编号生成

```c
static uint32_t g_sample_seq = 1U;

void sample_make_id(char *id, size_t size)
{
    snprintf(id, size, "A%03lu", (unsigned long) g_sample_seq);
}
```

只有当图片和记录全部保存成功后：

1. 更新 HMI 为当前样本号。
2. `g_sample_seq++`。
3. 将下一编号保存到 SD 卡、文件系统或具备磨损均衡的数据 Flash。

不要每次都擦写同一个 Flash 地址。断电恢复时应扫描最后一条有效记录或读取带 CRC 的双槽计数器，避免编号重复。

## 13. 里程计漂移处理

轮式里程计在实验田松软地面必然累计漂移。不能把 `x/y/yaw` 当作永不出错的绝对位置。

推荐：

```text
轮编码器 + IMU航向融合
        +
每行端点视觉标志/AprilTag/UWB/RTK绝对校正
```

最低要求：

1. 上电静止标定 IMU 零偏。
2. 标定轮径、轮距、编码器脉冲比例和运动方向。
3. 每段使用起点相对位移判断距离。
4. 每行端点重新校正横向位置和航向。
5. 采集前确认车辆稳定 300～500ms。

建议初始保护阈值：

```text
横向偏差 > 0.15m
或航向误差 > 8°
或里程计超过 200ms 未更新
```

满足任意条件时停车并进入重新定位状态。阈值必须通过现场试验调整。

采集稳定条件示例：

```c
bool car_is_stable(const odom_t *odom)
{
    return (fabsf(odom->vx) < 0.02f) &&
           (fabsf(odom->vy) < 0.02f) &&
           (fabsf(odom->wz) < 0.03f);
}
```

如果 `x/y` 只是对“指令速度”积分而不是编码器实测，则不能用于可靠定距，必须增加轮编码器或绝对定位基准。

## 14. FSP UART 接入提示

建议配置两个 UART 实例：

```text
g_uart_hmi：串口屏，115200 8N1
g_uart_car：小车控制器，按小车协议设置
```

FSP 回调参考：

```c
void hmi_uart_callback(uart_callback_args_t *p_args)
{
    if (p_args->event == UART_EVENT_RX_CHAR)
    {
        uint8_t byte = (uint8_t) p_args->data;
        hmi_rx_ring_push_from_isr(byte);
    }

    if (p_args->event == UART_EVENT_TX_COMPLETE)
    {
        hmi_tx_notify_complete_from_isr();
    }
}
```

主循环或 RTOS 任务中：

```c
void hmi_protocol_task(void)
{
    uint8_t byte;
    hmi_message_t message;

    while (hmi_rx_ring_pop(&byte))
    {
        if (hmi_rx_feed(&g_hmi_parser, byte, &message))
        {
            hmi_dispatch(&message);
        }
    }
}
```

实际 UART 名称以 FSP 配置器生成实例为准。不要直接复制不存在的 `g_uart_hmi` 名称而不修改配置。

## 15. 安全与故障处理表

| 故障 | 必须动作 | HMI提示 |
|---|---|---|
| `@E` 急停 | 停小车、停导轨、锁存急停 | 紧急停止/等待复位 |
| 非法 `@A` | 拒绝任务、保持停止 | 参数错误 |
| HMI帧溢出 | 丢弃到下一个 `@` | 通信异常，可计数 |
| 小车遥测超时 | 小车速度清零、暂停任务 | 小车通信异常 |
| 导轨原点超时 | 关闭脉冲 | 回零失败 |
| 左/右限位触发 | 禁止继续向该方向 | 已限位 |
| 摄像头失败 | 停止当前采样步骤 | 采集失败 |
| 识别失败 | 保留图片，记录错误 | 检测失败 |
| 存储失败 | 不增加样本号 | 保存失败 |
| 上传失败 | 本地记录保留 | 上传：未上传 |

所有故障处理应先保证执行器安全，再更新屏幕。不要因 HMI 发送失败而延迟电机停止。

## 16. 联调测试用例

### 16.1 XCOM 基础设置

```text
115200，8N1，None
接收“16进制显示”：按需开启
发送 HMI 原生命令时：勾选“16进制发送”
发送新行：关闭
```

更新数量为126：

```text
6D 61 69 6E 2E 6E 43 6F 75 6E 74 2E 76 61 6C 3D 31 32 36 FF FF FF
```

更新温度25.6℃：

```text
6D 61 69 6E 2E 78 54 65 6D 70 2E 76 61 6C 3D 32 35 36 FF FF FF
```

### 16.2 HMI 到 MCU 正常帧

| 输入 | 预期 |
|---|---|
| `@A,30,120,60,90,4,300,50,20,1\n` | 自动巡检 |
| `@A,0,0,0,0,0,300,50,20,1\n` | 小车停止，仅扫描一次 |
| `@E\n` | 立即急停并锁存 |
| `@N,yanqin,123456789\n` | 保存网络配置并启动连接 |
| `@D\n` | 断开 Wi-Fi |
| `@G\n` | 查询并回写网络状态 |

### 16.3 必须拒绝的帧

```text
@A,30,0,60,90,4,300,50,20,1\n      部分小车参数为0
@A,30,120,60,90,4,300,0,20,1\n    拍照间隔为0
@A,101,120,60,90,4,300,50,20,1\n  速度越界
@A,30,120,60,90,4,300,50,20,2\n   布尔字段错误
@A,30,120\n                            参数数量错误
随机乱码                                   无帧头
@N,,123456789\n                          SSID为空
@N,yanqin,1234\n                        密码过短
@N,yan,qin,123456789\n                    分隔符数量错误
```

### 16.4 急停测试

在以下每个阶段分别发送 `@E\n`：

- 小车前进中。
- 转向中。
- 导轨回零中。
- 导轨扫描中。
- 拍照/识别中。

所有情况都必须立即停止执行器，且新 `@A` 不得自动解除急停。

## 17. MCU 开发验收清单

- [ ] HMI UART 为 115200 8N1，TX/RX交叉并共地。
- [ ] 小车控制器使用独立 UART。
- [ ] ESP8266 使用独立 UART，供电和逻辑电平符合模块规格。
- [ ] HMI 接收器可从任意字节位置重新同步到 `@`。
- [ ] 接收器能处理分包、粘包、CRLF和超长帧。
- [ ] `@A` 严格解析9个整数参数。
- [ ] 前5个字段全零只启动导轨。
- [ ] 前5个字段部分为零被拒绝。
- [ ] `@E` 在任何状态均具有最高优先级。
- [ ] `@N` 能解析1～32字节 SSID 和8～63字节密码。
- [ ] `@D/@G` 能分别触发断开和状态回写。
- [ ] `vaNetFrm.txt_maxl=128`，密码不会因拼接缓冲区不足而截断。
- [ ] `@N` 的逗号后无空格，帧尾只有 `0x0A`。
- [ ] 导轨计算前确认 `shot_gap>0`。
- [ ] 导轨具有原点限位、运动超时和右端保护。
- [ ] 小车遥测超时200ms后安全停止。
- [ ] 每条 MCU->HMI 指令追加原始 `FF FF FF`。
- [ ] 中文字符串使用 UTF-8，字库包含全部动态字符。
- [ ] `xTemp/xHum` 使用10倍定点数并设置 `vvs1=1`。
- [ ] 样本保存成功后才增加 A001 编号。
- [ ] 自动巡检按规定行数正常结束。
- [ ] 仅采集识别完成一次扫描后正常结束。
- [ ] 急停解除必须有明确人工复位流程。

## 18. 推荐源码模块划分

```text
hmi_protocol.c/.h      @A、@E、@N、@D、@G接收解析
hmi_display.c/.h       MCU到HMI控件命令
car_protocol.c/.h      $vx,vy,wz 和里程计帧
rail_control.c/.h      PUL/DIR/ENA、回零、限位
esp8266_at.c/.h        ESP8266 AT命令和响应解析
network_manager.c/.h   Wi-Fi配置、DHCP、连接状态和HMI回写
task_state_machine.c   自动巡检和仅扫描流程
sample_store.c/.h      A001编号、CSV/JSON、图片关联
safety_manager.c/.h    急停、超时、故障锁存
sensor_manager.c/.h    温湿度和光照采集
```

协议解析、显示、执行器控制和存储不要全部堆在 UART 回调中。UART 回调只搬运字节和通知发送完成，业务逻辑由主循环或 RTOS 任务执行。

---

本手册是当前 `WHEAT.HMI + main/carset/scanset/netset + @A任务协议 + @N/@D/@G网络协议` 的单片机实现基准。如工程控件名、参数范围或任务帧字段发生变化，必须先更新本文档，再修改 MCU 代码和串口屏事件，避免各端定义不一致。
