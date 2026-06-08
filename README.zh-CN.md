# M5StickS3 TickTick / Typeless 控制器

[English README](README.md)

这是一个个人 V1 项目：把 M5Stack StickS3 / M5StickS3 做成一个通过 Wi-Fi 控制 Mac 的小型物理控制器，用来控制 TickTick 正计时 Focus、Typeless 语音输入、设备状态仪表盘，以及 TickTick 倒计时番茄钟。

项目由两部分组成：

- `firmware/`：StickS3 固件。负责读取 KEY1/KEY2、绘制屏幕 UI、处理 IMU 姿态、播放提示音，并通过 Wi-Fi 调用 Mac bridge。
- `mac-bridge/`：Mac 本地 HTTP bridge。负责维护状态，并通过 macOS Shortcuts、Typeless helper 或命令钩子控制 TickTick / Typeless。

本地敏感配置不会入库：

- `firmware/include/config.h`
- `mac-bridge/config.local.json`
- `.pio/` 下的 PlatformIO 构建产物

请用仓库里的 example 文件作为模板。

## 当前交互模型

`KEY2` 短按在四个屏幕之间循环：

1. 番茄专注屏
2. Typeless 语音输入屏
3. 低亮度时钟 / 电量 / OTA 仪表屏
4. 沙漏倒计时屏

全局控制：

- `KEY2` 单击：切换到下一屏。
- `KEY2` 双击：息屏。Wi-Fi、bridge 轮询、正计时和倒计时状态都会继续运行。
- 息屏后第一次按任意键：只唤醒屏幕，不顺手触发开始、暂停、换屏或语音输入。
- `KEY2` 长按：当前故意留空，不承担业务功能。

`KEY1` 根据当前屏幕执行不同动作：

- 番茄专注屏：
  - 单击：开始 / 暂停 / 继续 TickTick 正计时 Focus
  - 双击：结束当前 Focus，并播放 Harvest 收获动画
- Typeless 屏：
  - 长按：开始 Typeless 语音输入
  - 长按后松手：结束 Typeless 语音输入
- 沙漏屏：
  - 单击：开始 / 暂停 / 继续倒计时
  - 双击：结束 / 重置倒计时

机身电源 / 复位相关按键不参与业务逻辑，避免和系统功能冲突。

## 番茄正计时

番茄专注屏用于长任务的正计时，不是 25 分钟倒计时。固件从 bridge 获取 `elapsed_seconds`，再用本地 `millis()` 在网络轮询之间平滑推进动画。

动画规则：

- 0 到 5 分钟：小番茄成形，偏青红，跳动较快。
- 5 到 30 分钟：番茄变大，从青红过渡到橙红。
- 30 到 90 分钟：番茄成熟，变圆润、深红，出现叶片和高光。
- 90 分钟以后：进入深度工作态，不再无限变大，只增加轻微光晕 / 成熟纹理。
- 暂停：番茄保持当前成熟度，降低亮度，轻微呼吸。
- 结束：番茄弹一下，出现 Harvest 高光和短提示音。
- 离线 / 错误：番茄灰化，但保留轮廓。

TickTick 正计时开始依赖 macOS Shortcut：`TickTick Start Stopwatch`。暂停、继续和结束目前依赖可配置的 macOS UI 自动化钩子。

## Typeless 语音输入

Typeless 屏和 TickTick 记录完全独立。

- 麦克风图标在未长按时保持安静状态。
- `KEY1` 长按调用 `/device/voice-input-start`。
- `KEY1` 松手调用 `/device/voice-input-stop`。
- 当前使用 Mac / Typeless 侧麦克风，不使用 StickS3 自带麦克风。

Mac bridge 通过 `TypelessKeySender.app` 这个小型原生 helper 发送 Typeless 快捷键。当前开始和结束都使用：

```text
Ctrl + Cmd + Shift + Space
```

早期的 `KEY2` 长按录音备注方案已经废弃。

## 仪表屏

仪表屏是低亮度状态页，用来查看：

- 24 小时制本地时间
- 电量百分比
- 电池图标
- 充电标记
- OTA 地址，格式为 `OTA <device-ip>`

时间来自 Mac bridge 返回的 `server_time`，随后由 StickS3 本地用 `millis()` 平滑推进。

仪表屏只是低亮度，不等同于息屏。真正息屏由 `KEY2` 双击触发，屏幕亮度会降到 0。

## 沙漏倒计时

沙漏屏用于 TickTick 倒计时 / 番茄钟模式，和正计时番茄屏彼此独立。

- 默认时长：25 分钟，`POMO_COUNTDOWN_SECONDS = 1500`
- 开始接口：`POST /pomo/start`
- bridge 快捷指令：`TickTick Start Countdown`

V1 行为：

- 开始倒计时会通过 Shortcuts 同步到 TickTick。
- 暂停、继续和结束会通过 `ticktick_focus_ui.py` 走 TickTick 桌面 UI 自动化；如果自动化失败，bridge 仍会保留本地倒计时状态作为兜底。

IMU 姿态映射：

- X 轴负责正立 / 倒置。
- Z 轴负责平放。
- Y 轴不参与沙漏逻辑。
- X 轴翻转后显示 `FLIP`，触发开始 / 继续倒计时。
- Z 轴平放时显示 `FLAT`，触发暂停。
- `FLIP` 状态下沙漏屏会自动旋转 180 度，倒置设备时仍能正向阅读。

沙漏屏里的 `KEY1` 单击 / 双击交互和番茄专注屏保持一致。

## Mac Bridge 快速启动

Dry-run 模式：

```bash
python3 mac-bridge/focus_bridge.py --dry-run --state-path /tmp/m5stick-focus-state.json
```

模拟设备请求：

```bash
curl -s -X POST http://127.0.0.1:8787/focus/click
curl -s -X POST http://127.0.0.1:8787/focus/double-click
curl -s -X POST http://127.0.0.1:8787/device/voice-input-start
curl -s -X POST http://127.0.0.1:8787/device/voice-input-stop
curl -s -X POST http://127.0.0.1:8787/pomo/start -H 'Content-Type: application/json' -d '{"duration_seconds":1500}'
curl -s -X POST http://127.0.0.1:8787/pomo/pause
curl -s -X POST http://127.0.0.1:8787/pomo/resume
curl -s -X POST http://127.0.0.1:8787/pomo/end
curl -s http://127.0.0.1:8787/focus/state
curl -s http://127.0.0.1:8787/pomo/state
```

Live 模式：

```bash
python3 mac-bridge/focus_bridge.py --config mac-bridge/config.local.json --live
```

Mac bridge 设计为可通过 macOS LaunchAgent 后台运行：

```text
com.machiwhale.m5stick-focus-bridge
```

## Shortcuts 设置

需要创建两个 macOS Shortcuts：

- `TickTick Start Stopwatch`
  - TickTick 动作：Start Focus / Stopwatch
- `TickTick Start Countdown`
  - TickTick 动作：Start Pomo / Countdown，25 分钟

复制 bridge 配置模板：

```bash
cp mac-bridge/config.example.json mac-bridge/config.local.json
```

然后把 `dry_run` 改为 `false`，并按本机路径调整命令。

## 固件设置

复制固件配置模板：

```bash
cp firmware/include/config.example.h firmware/include/config.h
```

编辑 Wi-Fi 和 bridge 地址：

```cpp
static const char* WIFI_SSID = "your-wifi";
static const char* WIFI_PASSWORD = "your-password";
static const char* BRIDGE_BASE_URL = "http://your-mac-lan-ip:8787";
static const char* BRIDGE_TOKEN = "";
```

USB 编译和烧录：

```bash
cd firmware
pio run -e m5stack-sticks3
pio run -e m5stack-sticks3 -t upload --upload-port /dev/cu.usbmodem2101
```

OTA 编译和上传：

```bash
cd firmware
pio run -e m5stack-sticks3-ota -t upload
```

如果 `m5stick-focus.local` 无法解析，切到仪表屏查看底部 IP，然后使用：

```bash
pio run -e m5stack-sticks3-ota -t upload --upload-port <device-ip>
```

PlatformIO 环境使用 `M5Unified`、`M5PM1`、`ArduinoOTA`、`HTTPClient` 和 `WiFi`。

## 可选 Token

局域网内可以在 bridge 和固件中设置同一个 token：

```json
{
  "security": {
    "token": "replace-with-a-long-random-string"
  }
}
```

固件会通过 `X-Focus-Token` 发送它。

## 验证记录

当前 V1 已验证：

- PlatformIO 编译通过
- USB 烧录通过
- Wi-Fi OTA 上传通过
- bridge 单元测试通过
- 拔掉 USB 后可通过 Wi-Fi 控制 TickTick 和 Typeless
- TickTick 正计时：开始 / 暂停 / 继续 / 结束可用
- Typeless：长按开始、松手结束可用
- 仪表屏：时间、电量、OTA 地址显示可用
- 沙漏倒计时：X 轴翻转启动、Z 轴平放暂停、UI 自动旋转、TickTick 倒计时开始 / 暂停 / 结束可用
- `KEY2` 双击息屏和任意键唤醒吞键行为可用

运行 bridge 测试：

```bash
python3 -m unittest mac-bridge/test_focus_bridge.py
```

## 文件结构

- `firmware/src/main.cpp`：StickS3 固件主程序。
- `firmware/include/config.example.h`：干净的固件配置模板。
- `firmware/platformio.ini`：PlatformIO 构建配置。
- `mac-bridge/focus_bridge.py`：本地 HTTP 服务和 TickTick / Typeless 控制器。
- `mac-bridge/ticktick_focus_ui.py`：TickTick Focus 的 macOS UI 自动化 helper。
- `mac-bridge/test_focus_bridge.py`：bridge 状态机测试。
- `mac-bridge/config.example.json`：bridge 配置模板。
- `mac-bridge/SHORTCUTS.md`：TickTick macOS Shortcuts 备注。
- `mac-bridge/send_typeless_key.c`：Typeless 原生按键 helper 源码。
