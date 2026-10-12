# iGC.Lite 灯光控制逆向分析

分析对象：`./iGC.Lite/`（七彩虹 iGame Center Lite，v1.0.x，2026-01 构建）。

---

## 1. 程序结构

`iGC.Lite` 是 **托管 .NET(WPF) 前端 + 原生 C++ 硬件层** 的组合：

```
iGC.Lite.exe                    WPF 主程序（MIT 之外的 Stylet MVVM）
iGC.Lite.Service.dll            Windows 服务
iGameCenter.Lite.Service.dll    服务逻辑，P/Invoke 原生 LED 库   ← 关键
iGameCenter.Hardware.dll        传感器（HWiNFO / OpenHardwareMonitor）
iGameCenter.Web.dll / Utils / ConfigManager / Contracts

iGameAPI/
  iGameAPI.LED.dll      ★ 统一灯效引擎（当前 Lite 版实际调用这个）
  iGameAPI.LEDCalc.dll  ★ 灯效计算（静态/呼吸/渐变…）
  iGameAPI.NVAPI.dll    ★ NVIDIA NVAPI 封装 → 显卡 i2c
  iGameEasyCalc.dll / iGameSpaceCalc.dll   空间/屏幕同步灯效
  iGameTransfer.dll / iGameLEDController.dll   WS2_32 → 127.0.0.1 内部 IPC
  iGameMBoard.dll       （旧引擎）主板 HID，自带 hidapi（导出 hid_*）
  iGameDRAM.dll         内存（内部符号 iGameDRAM_ENE → ENE SMBus）
  iGameGCard.dll        显卡（调用 NVAPI ReadFromi2c/WriteToi2c）
  iGameKeyBoard.dll / iGameControlBox.dll   键盘/控制盒（HID）
  iGameAPI.LCD*.dll     副屏

WinRing0x64.sys/.dll    微软签名的内核驱动（SuperIO/风扇/SMBus 直通）
HWiNFO64.dll, PresentMon 第三方监控/帧率
```

当前版本实际使用的灯控链路：

```
iGC.Lite.exe → iGameCenter.Lite.Service.dll
             → iGameAPI.LED.dll   (统一引擎，P/Invoke)
             → iGameAPI.LEDCalc.dll (效果计算)
```

---

## 2. 各硬件用的传输方式

| 设备 | 模块 | 传输 | 结论 |
|------|------|------|------|
| 显卡 RGB | `iGameGCard` / `iGameAPI.NVAPI` | **NVAPI i2c** → GPU SMBus | 地址 `0x61` |
| 主板 RGB/ARGB | `iGameAPI.LED`（`Colorful_Z590*`/`ECS_*`/`WB_*`…类）、旧版 `iGameMBoard` | **USB HID** | VID `0x2F4C`，usage page `0xFF01` |
| 显卡风扇/超频 | `iGameAPI.NVAPI` | NVAPI | — |
| 主板风扇/温度 | `iGameAPI.MBoard.WinRing0`（托管） | WinRing0 端口 I/O（SuperIO `HWM_INDEX/DATA_PORT`） | — |
| 内存 RGB | `iGameDRAM` | ENE（`iGameDRAM_ENE`） | 未移植 |
| 键盘 / 控制盒 | `iGameKeyBoard` / `iGameControlBox` | HID | 未移植 |
| 屏幕同步灯效 | `iGameAPI.LEDCalc` / `iGameSpaceCalc` | WS2_32 → `127.0.0.1` | 未移植 |

---

## 3. 显卡协议（已验证，OpenRGB 亦采用）

握手：向从机地址 `0x61` 原始写 6 字节，再读 0x40 字节。

```
write: AA EF 81 02 1C 02
read : 期望返回首 3 字节 AA EF 81
```

静态色（`COLORFUL_PACKET_LENGTH_V1 = 11`，Ampere/Ada，含 RTX 3070）：

```
AA EF 12 03 01 FF R G B CRC_LO CRC_HI
CRC = (sum of the first 9 bytes) & 0xFFFF     (小端)
```

V2（RTX 4070 Vulcan / 4090 Neptune，`COLORFUL_PACKET_LENGTH_V2 = 122`）：

```
AA EF 01 04 88 26 [R G B]×38                  # 从第 6 字节起重复填充三通道
末 2 字节 = 前 120 字节和的低 16 位（小端）
```

**关灯 = 直接写 `R=G=B=0`。** iGame GeForce 不给第三方暴露 MCU 特效，所以只有
"直接色"；呼吸/彩虹在主控端逐帧刷新。

> Linux 对应实现：`/dev/i2c-N` + `I2C_RDWR` 单条 `i2c_msg`（与 OpenRGB
> `i2c_write_block`/`i2c_read_block` 完全相同：裸 i2c 传输，不是 SMBus block）。
> NVIDIA 专有驱动会暴露名为 `NVIDIA i2c adapter N at ...` 的适配器。

---

## 4. 主板协议（HID，本次逆向的核心）

### 4.1 设备发现

`iGameAPI.LED.dll` 里每类主板一个构造函数，调用自带的：

```c
hid_device *hid_open1(uint16_t vid, uint16_t pid, uint16_t usage_page)
// 内部遍历 hid_enumerate()，比较 hid_device_info 的
//   +0x08 vendor_id, +0x0a product_id, +0x30 usage_page
```

调用点参数（`iGameAPI.LED.dll`，地址 0x180043ca0 为 `hid_open1`）：

| PID | 类（RTTI `.?AV...@@`） | 调用点 |
|-----|------------------------|--------|
| `0x1004` | `Colorful_Z590Write` | 0x18002be72 |
| `0x1006` | `Colorful_Z590_2021_Write` | 0x18002e792 |
| `0x2002` | `ECS_B560Write` | 0x1800303f2 |
| `0x1000` | (另一主板类) | 0x18003668c |
| `0x1003` | (另一主板类) | 0x18003713c |
| `0x1005` | (另一主板类) | 0x18003811c |
| `0x1007` | `WB_B660Write` / `Z490Write` | 0x18003871c / 0x180041d32 |
| `0x04D9` | Holtek 方案（键盘/其它） | 0x180034f76 |

> 所有主板类共用一个 VID：**`0x2F4C`**，第三参数一直是 **`0xFF01`**（厂商自定义
> usage page）。每个机型一个 PID。`iGameMBoard.dll` 里还有更多新板型
> （`B760W4`、`CQ_H610`、`CQ_Z990_0x1023`、`Colorful_2025_1124_0x1022` 等）。

### 4.2 报文格式（65 字节）

两条独立证据链得到同一格式：

**A. `iGameAPI.LED.dll`**
`iGameAPI_LED_SetDeviceEffect(0x18002adb0)` → 设备对象虚表 `[+0x20]`
（如 `Colorful_Z590Write::SetDeviceEffect = 0x18002e2c0`）→ 取 LED 数据对象
`0x180082e78`（RTTI：`Colorful_Z590_LED_DATA`）虚表 `[+0x68] = 0x18002d300`
生成的报文列表，逐条 `WriteFile`（长度 `0x41 = 65`）。

`0x18002d300` 按效果 id `[obj+0x7C]` 分派：

| 效果 id | 处理函数 | 追加字节 |
|---------|----------|----------|
| `0` OFF | `0x18002dc48` | `00`, `[obj+0xA0]`, `[obj+0xA1]`, `0` |
| `1` STATIC | `0x18002da36` | `00`, `[obj+0xA0]`, `[obj+0xA1]`, `1`, 颜色向量(R,G,B…) |
| `2` | `0x18002d890` | 呼吸类 |
| `3` | `0x18002d712` | 渐变类 |

其中 `[obj+0xA0]=1`、`[obj+0xA1]=0`（构造时 `mov word ptr [rsi+0xa0], 1`）。

**B. `iGameMBoard.dll`**
`iGameMBoard_Set_MCU` 对输入结构体首 dword（效果 id）做 switch，把结果写进
`[rbp+0x3d50]` 的 65 字节缓冲，公共尾部 `mov word ptr [rbp+0x3d51], 1`，再
`WriteFile`（`r8d = 0x41`）。跳转表（`[image+0x34cac]` 索引表 + `[image+0x34cc8]`
映射）给出：

```
效果 0x00 → 0x1800346c8 : [3]=0        （关灯）
效果 0x01 → 0x1800346d3 : [3]=1, [4..6]=R,G,B
效果 0x02 → 0x18003472e : [3]=2         （呼吸：亮度、速度）
效果 0x03 → 0x18003477f : [3]=3
效果 0x88 → 0x1800347b3 : 逐灯 RGB 数组
效果 0xAA → 0x180034c38 : [3]=0xAA
```

`iGameMBoard_Get_MCU_CMD_List` 把命令名内联为 `movabs` 立即数：
`0x65687461657242 = "Breathe"`、`0x776f626e696152 = "Rainbow"`；命令 id 集合
`{0,1,2,3,0x88,0xAA}`。

### 4.3 结论：报文布局

```
byte  0 : 0x00            报告 ID
byte  1 : 0x01            常量
byte  2 : 0x00            常量
byte  3 : effect          0=关灯 1=静态 2=呼吸 3=渐变 0x88=逐灯 0xAA=…
byte  4 : R
byte  5 : G
byte  6 : B
byte  7..64 : 0           补零到 65 字节
```

**关灯报文（65 字节）**：

```
00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00
```

Linux 实现：遍历 `/sys/class/hidraw/hidraw*/device/uevent`，取 `HID_ID` 中
`VID=2F4C` 的设备，`open("/dev/hidrawN")` 后 `write()` 这 65 字节即可
（hidraw 的写即 HID output report）。

---

## 5. 灯效引擎

- `iGameAPI.LEDCalc.dll` 负责把"静态/呼吸/渐变/彩虹/温度/音乐"等效果算成每帧
  的 RGB，导出 `iGameAPI_LEDCalc_SetLightingEffect` / `SaveLightingEffect` /
  `SetAtmosSyncLightingEffect` / `CanRazerConnect`（支持 Razer Chroma Connect）。
- `iGameEasyCalc.dll` / `iGameSpaceCalc.dll` 做逐灯/区域映射与"屏幕空间同步"。
- `iGameAPI.LED.Immersive.exe` 与主程序通过 `127.0.0.1`（WS2_32）通信
  （`iGameTransfer` / `iGameLEDController`）。

## 6. 配置与日志

- `CCData/Configs/NormalConfig.json` — 总开关、刷新率、写入周期
  （`WriteDataTimes`）、是否加载 DRAM/网络/LCD/HWiNFO。
- `CCData/Configs/OsdSetting.json` — OSD 叠加层。
- `CCData/Configs/GraphicsCard_<型号>_<subsys>/OCConfig.json` — 显卡超频/风扇曲线。
- `CCData/Logs/*.txt` — 运行日志（默认为空）。

## 7. 与 OpenRGB 的关系

- OpenRGB 0.9 的 `ColorfulGPUController` 已经涵盖 iGame RTX 20/30/40/50 系列
  （I2C，地址 `0x61`），并已收录 `iGame GeForce RTX 3070 Advanced OC-V`
  （SVID `7377`，SPID `140A`）——在设备页搜 "colorful" 搜不到，是因为搜索匹配
  的是产品名列而不是控制器列。
- **主板没有任何 Colorful 条目**；本项目的主板协议是独立逆向的。
- OpenRGB 按 PCI 子系统 ID 匹配显卡；同型号的不同子版本常识别失败，本工具直接
  探测 `0x61`，无需匹配。

## 8. 未覆盖硬件的路线图

| 目标 | 传输 | 下一步 |
|------|------|--------|
| 内存 RGB | ENE SMBus | 参考 OpenRGB `ENESMBusController`；`iGameDRAM_ENE` 符号 |
| 键盘 / 控制盒 | HID | 复用本工具 HID 层，按 `iGameKeyBoard` 的型号表补 PID |
| 屏幕同步 / 逐灯 | 应用层 | 需要 `LEDCalc` 的每帧 RGB 列表，工作量大 |

## 9. 免责声明

本分析用于在自有硬件上实现互操作。代码为独立实现（MIT）。原程序许可协议
禁止逆向，但该限制对互操作性用途在多数辖区不可执行；上游 OpenRGB 也已公开发布
同类协议。

---

## 10. 附录：Linux 用户态访问 GPU 内部 i2c 的实验（已实现，但控制器不可达）

这是本次调查中额外打通的能力，记录以备后用。目标是复刻 NVAPI 的 i2c，绕过
nvidia 驱动只暴露 DDC 适配器的限制。**它本身是通的**（在 DDC 端口读 `0x50`
拿到了 EDID），但 iGame 显卡的 RGB 控制器（`0x61`）不在任何可访问端口上。

### 10.1 用户态 RM 客户端流程

```
open("/dev/nvidiactl")                        # 控制设备
  ioctl(_IOWR('F', 0x2B), NVOS21{hClass=0x41})              -> hClient
open("/dev/nvidia0")
  ioctl(_IOWR('F', 201  ), {ctl_fd=nvidiactl})              # NV_ESC_REGISTER_FD
  ioctl(_IOWR('F', 0x2B), NVOS21{parent=hClient,
                     hClass=0x80, params=NV0080_ALLOC_PARAMETERS{deviceId=0}}) -> hDevice
  ioctl(_IOWR('F', 0x2B), NVOS21{parent=hDevice, hClass=0x2080}) -> hSubDevice
  ioctl(_IOWR('F', 0x2B), NVOS21{parent=hSubDevice, hClass=0x402C}) -> hI2C
```

要点：
- ioctl 宏必须用 `_IOWR('F', n, type)` 编码，`_IOC_NR`/`_IOC_SIZE` 会参与驱动校验。
- `NV_ESC_REGISTER_FD`（201）把 `/dev/nvidiaN` 关联到 `/dev/nvidiactl`，
  否则 i2c ioctl 返回 `NV_ERR_INVALID_CLIENT`。
- `NV40_I2C` 对象（class `0x402C`）必须挂在**subdevice**下，挂在 device 下会
  `NV_ERR_INVALID_OBJECT_PARENT`。

### 10.2 发 i2c 事务

`NV_ESC_RM_CONTROL`（`_IOWR('F',0x2A, NVOS54)`），
`cmd = NV402C_CTRL_CMD_I2C_TRANSACTION = 0x402c0105`，
参数 `NV402C_CTRL_I2C_TRANSACTION_PARAMS`（96 字节）：

```
u8  portId            // 端口号（0xFF = dynamic）
u32 flags
u16 deviceAddress     // 7 位地址 << 1
u32 transType         // 2 = I2C_BLOCK_RW, 3 = I2C_BUFFER_RW, 4 = SMBUS_BYTE_RW …
u8  transData[80]     // union；I2C_BLOCK_RW: {u8 bWrite; u32 messageLength; u64 pMessage;}
```

其他可用控制：`NV402C_CTRL_CMD_I2C_GET_PORT_INFO`（`0x402c0101`）、
`..._GET_PORT_SPEED`（`0x402c0103`）、`..._TABLE_GET_DEV_INFO`（`0x402c0104`）。

### 10.3 这台机器上的实测端口

```
GET_PORT_INFO:  port0=0x01  port1=0x11  port2=0x11  port3=0x01  port4=0x01
                port5..8=0x17(DDC)  port9=0x01  port15=0x10
地址扫描:       port1 -> 0x50            (DP-1 的 EDID)
                port5 -> 0x37 3A 4A 4B 50  (HDMI 标准设备 + EDID)
                port0/2/3/4/9..15/0xFF -> 无
DCB i2c 设备表: 仅 1 项: type=71 addr=0x4A port=2
```

`0x61` / `0x58` 在 port1/5/6/7/8 全部 `NV_ERR_I2C_ERROR`（NACK）。因此 iGame
3070 的 RGB 控制器**不在 RM 暴露的任何 i2c 总线上**，用户态无法触及。

（另外 `NV2080_CTRL_CMD_I2C_ACCESS`（经 `NV_ESC_RM_I2C_ACCESS`, 0x39）在本驱动上
对任何命令/端口都返回 `NV_ERR_NOT_SUPPORTED`，GSP-RM 已不实现该 legacy 路径。）

