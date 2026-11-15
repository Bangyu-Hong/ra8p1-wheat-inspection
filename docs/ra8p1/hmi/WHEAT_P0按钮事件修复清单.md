# WHEAT P0 按钮事件修复清单

适用于当前四个业务页面：`main`、`carset`、`scanset`、`netset`。

本阶段目标是先删除旧 `@A/@E`和屏幕直接控制运动的代码，让已有按钮进入安全的新协议过渡状态。

## 1. 操作规则

- 普通按钮只在“弹起事件”执行一次。
- 紧急停止只在“按下事件”执行一次。
- 一个按钮不能在按下和弹起两处同时发帧。
- 每个可能引起运动的请求必须带新的 `SEQ`。
- 所有新任务帧使用 ASCII，帧尾为单字节 LF：`printh 0A`。
- 屏幕只显示“请求已发送”，不能在本地改成“已完成”或“已到位”。

## 2. 全局序号和字符串变量

在 `Program.s` 中增加：

```c
int gSeq=0
```

在 `main` 页面创建两个“变量”控件，类型选字符串：

```text
objname=vaTmp
vscope=全局
txt_maxl=16
```

```text
objname=vaFrame
vscope=全局
txt_maxl=256
```

每次发送前使用以下序号代码：

```c
gSeq+=1
if(gSeq>2000000000)
{
  gSeq=1
}
covx gSeq,vaTmp.txt,0,0
```

## 3. main 页按钮

### 3.1 `bPatrol` 开始整田巡检

按下事件：清空。

弹起事件：

```c
gSeq+=1
if(gSeq>2000000000)
{
  gSeq=1
}
covx gSeq,vaTmp.txt,0,0
vaFrame.txt="@C1,"
vaFrame.txt+=vaTmp.txt
vaFrame.txt+=",START"
prints vaFrame.txt,0
printh 0A
tDetectState.txt="请求已发送"
tDetSub.txt="等待主控确认"
```

### 3.2 `bSearch` 单帧识别

按下事件：清空。

弹起事件：

```c
gSeq+=1
if(gSeq>2000000000)
{
  gSeq=1
}
covx gSeq,vaTmp.txt,0,0
vaFrame.txt="@C1,"
vaFrame.txt+=vaTmp.txt
vaFrame.txt+=",INFER"
prints vaFrame.txt,0
printh 0A
tDetectState.txt="请求已发送"
tDetSub.txt="等待主控确认"
```

### 3.3 `bCapture` 底盘与田间参数入口

按下事件：清空。

弹起事件：

```c
page carset
```

### 3.4 `bReturn` 滑轨与拍摄参数入口

按下事件：清空。

弹起事件：

```c
page scanset
```

### 3.5 `bStop` 紧急停止

弹起事件：清空。

按下事件：

```c
gSeq+=1
if(gSeq>2000000000)
{
  gSeq=1
}
covx gSeq,vaTmp.txt,0,0
vaFrame.txt="@E1,"
vaFrame.txt+=vaTmp.txt
prints vaFrame.txt,0
printh 0A
tDetectState.txt="急停请求已发送"
tDetectState.pco=60042
tDetSub.txt="等待主控确认"
```

### 3.6 网络设置入口

实际控件名可能是 `mNetSet` 或 `bNetSet`，以属性面板显示为准。

按下事件：清空。

弹起事件：

```c
page netset
```

## 4. carset 页按钮

当前页面仍是旧字段，加减按钮可以先修正范围，但 `bCarSave` 暂时不能发送 `@P1`，因为页面还缺少照片视野、沿 Y 拍摄间隔、运动超时和微调距离。

每个可编辑数值变化后都执行：

```c
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

### 4.1 `bCarBack`

弹起事件：

```c
page main
```

### 4.2 `bToScan`

弹起事件：

```c
page scanset
```

### 4.3 `bPatSub` / `bPatAdd`

将 `nPatSpd` 暂时改为“最大速度 mm/s”，范围 1～3000，步长 10。

`bPatSub` 弹起事件：

```c
if(nPatSpd.val>1)
{
  nPatSpd.val-=10
  if(nPatSpd.val<1)
  {
    nPatSpd.val=1
  }
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

`bPatAdd` 弹起事件：

```c
if(nPatSpd.val<3000)
{
  nPatSpd.val+=10
  if(nPatSpd.val>3000)
  {
    nPatSpd.val=3000
  }
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

### 4.4 `bSegSub` / `bSegAdd`

将 `nSegDist` 改为“每条过道行程 mm”，范围 10～1000000，步长 100。

`bSegSub` 弹起事件：

```c
if(nSegDist.val>10)
{
  nSegDist.val-=100
  if(nSegDist.val<10)
  {
    nSegDist.val=10
  }
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

`bSegAdd` 弹起事件：

```c
if(nSegDist.val<1000000)
{
  nSegDist.val+=100
  if(nSegDist.val>1000000)
  {
    nSegDist.val=1000000
  }
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

### 4.5 `bRowSub` / `bRowAdd`

将 `nRowGap` 改为“小车过道宽度 mm”，范围 100～2000，步长 10。

`bRowSub` 弹起事件：

```c
if(nRowGap.val>100)
{
  nRowGap.val-=10
  if(nRowGap.val<100)
  {
    nRowGap.val=100
  }
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

`bRowAdd` 弹起事件：

```c
if(nRowGap.val<2000)
{
  nRowGap.val+=10
  if(nRowGap.val>2000)
  {
    nRowGap.val=2000
  }
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

### 4.6 `bAngSub` / `bAngAdd`

新方案不再使用转向角度。

- 清空按下事件。
- 清空弹起事件。
- 隐藏 `bAngSub`、`bAngAdd`、`nTurnAng` 和对应标题/单位。

### 4.7 `bCntSub` / `bCntAdd`

将 `nRowCnt` 改为“巡检过道数”，范围 1～100，步长 1。

`bCntSub` 弹起事件：

```c
if(nRowCnt.val>1)
{
  nRowCnt.val-=1
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

`bCntAdd` 弹起事件：

```c
if(nRowCnt.val<100)
{
  nRowCnt.val+=1
}
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

### 4.8 `bCarSave`

暂时不发送任何串口帧。清空旧的 `@A`、`@CARCFG` 或任何直接运动代码。

弹起事件：

```c
tSaveSt.txt="未下发"
tSaveSt.pco=62725
```

待新参数控件补齐后，再将该按钮改为发送完整 `@P1`。

## 5. scanset 页按钮

当前页面仍缺少滑轨模式、固定位置、停稳时间、推理超时、异常策略和微调距离，因此 `bScanSave` 暂时不能发送 `@P1`。

### 5.1 `bScanBack`

弹起事件：

```c
page main
```

### 5.2 `bToCar`

弹起事件：

```c
page carset
```

### 5.3 `bScanSub` / `bScanAdd`

`nScanDist` 暂时表示扫描总行程，范围 10～300 mm，步长 10。

`bScanSub` 弹起事件：

```c
if(nScanDist.val>10)
{
  nScanDist.val-=10
}
tScanSt.txt="未下发"
tScanSt.pco=62725
```

`bScanAdd` 弹起事件：

```c
if(nScanDist.val<300)
{
  nScanDist.val+=10
}
tScanSt.txt="未下发"
tScanSt.pco=62725
```

### 5.4 `bGapSub` / `bGapAdd`

`nShotGap` 改为滑轨拍摄间隔，范围 10～300 mm，步长 10。

`bGapSub` 弹起事件：

```c
if(nShotGap.val>10)
{
  nShotGap.val-=10
}
tScanSt.txt="未下发"
tScanSt.pco=62725
```

`bGapAdd` 弹起事件：

```c
if(nShotGap.val<300)
{
  nShotGap.val+=10
}
tScanSt.txt="未下发"
tScanSt.pco=62725
```

### 5.5 `bRailSub` / `bRailAdd`

正式页面不再允许编辑导轨速度。

- 清空按下事件。
- 清空弹起事件。
- 隐藏 `bRailSub`、`bRailAdd`、`nRailSpd` 和对应标题/单位。

### 5.6 `btStartShot`

新方案删除“起点拍照”。

- 清空按下事件。
- 清空弹起事件。
- 隐藏控件和标题。

### 5.7 `bHomeTest` 和 `bRailFix`

这两个按钮不允许在 HMI 直接控制 ZDT42。

- 清空按下事件。
- 清空弹起事件。
- 暂时隐藏；后续在 `manual` 页改成发送 `@J1`。

### 5.8 `bScanSave`

清空旧 `@A`、`@SCANCFG` 和所有直接滑轨控制代码。

弹起事件：

```c
tScanSt.txt="未下发"
tScanSt.pco=62725
```

## 6. netset 页按钮

网络协议仍使用 `@N/@D/@G`，不加 `SEQ`。

### 6.1 `bNetBack`

弹起事件：

```c
page main
```

### 6.2 `btDhcp`

DHCP 必须固定开启或只读。清空所有按下/弹起事件，并设置：

```text
val=1
```

### 6.3 `bNetConn`

确认 `vaNetFrm.txt_maxl=128`、`tSSID.txt_maxl=32`、`tPwd.txt_maxl=64`、`tPwd.pw=1`。

按下事件：清空。

弹起事件：

```c
tNetState.txt="请检查输入"
tNetState.pco=62725
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
    tNetState.txt="请求已发送"
    tNetState.pco=62725
  }
}
```

### 6.4 `bNetDisc`

按下事件：清空。

弹起事件：

```c
prints "@D",0
printh 0A
tNetState.txt="请求已发送"
tNetState.pco=62725
```

### 6.5 `bNetGet`

按下事件：清空。

弹起事件：

```c
prints "@G",0
printh 0A
tNetState.txt="刷新请求已发送"
tNetState.pco=62725
```

## 7. 本阶段编译和串口验证

1. 先编译 HMI，要求 0 错误。
2. 打开调试器，切到“用户 MCU 输入”或使用 XCOM 抓取屏幕 TX。
3. `bPatrol` 应发送类似 `@C1,1,START\n`。
4. `bSearch` 应发送类似 `@C1,2,INFER\n`。
5. `bStop` 应发送类似 `@E1,3\n`。
6. 同一次点击只能出现一帧。
7. 不应再出现 `@A`、`@Q`、`@AUTO#`、`@CARCFG`、`@SCANCFG`或 `$vx,vy,wz`。

当前暂不验收 `@P1`。必须先完成 `carset/scanset` 新控件和输入范围，然后再一次性生成完整 16 字段参数申请帧。
