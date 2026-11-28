# scrcpy — 输入映射增强版

> [!IMPORTANT]
> **上游仓库（官方唯一来源）：<https://github.com/Genymobile/scrcpy>**
>
> 本仓库 <https://github.com/fggre6/scrcpy> 是上述上游项目的第三方 fork，仅用于承载输入映射相关的增量改动。
> 下载官方发行版请始终前往上游仓库；不要从任何名称含 `scrcpy` 的第三方网站下载程序。

`scrcpy`（读作 **scr**een **c**o**py**）把通过 USB 或 [TCP/IP](doc/connection.md#tcpip-wireless) 连接的 Android 设备屏幕（含音频）镜像到电脑上，并允许用电脑的键盘和鼠标操控设备。它**不需要 root**，也**不需要在设备上安装任何 App**，可在 Linux、Windows、macOS 上运行。

本项目基于上游 **scrcpy v5.0.1**，在完整保留上游能力的基础上，新增了一套可配置的**键盘/鼠标 → 触摸映射系统**。

## 与上游的关系

| 项目 | 地址 | 说明 |
| --- | --- | --- |
| **上游（官方）** | <https://github.com/Genymobile/scrcpy> | 由 Genymobile 与 Romain Vimont 维护的官方项目，唯一权威来源 |
| **本仓库（fork）** | <https://github.com/fggre6/scrcpy> | 本 fork，新增输入映射、条件依赖、管理面板等特性 |
| 当前主分支 | `master` | 由功能分支 `trae/agent-6FTCv4` 快进合并而来 |

本 fork 沿用上游的 Apache-2.0 许可证与版权声明（见文末）。上游的功能文档、问题反馈与发行版均以其官方仓库为准。

## 上游继承的核心能力

- **轻量**：原生程序，只显示设备屏幕
- **高性能**：30~120fps，取决于设备
- **高质量**：1920×1080 或更高
- **低延迟**：[35~70ms](https://github.com/Genymobile/scrcpy/pull/646)
- **启动快**：约 1 秒显示首帧
- **无侵入**：设备上不残留任何安装内容
- **无账号、无广告、无需联网**
- 音频转发（Android 11+）、[录制](doc/recording.md)、[虚拟显示屏](doc/virtual-display.md)、[复制粘贴](doc/control.md#copy-paste)、画质配置、摄像头镜像（Android 12+）、[HID 键鼠模拟](doc/keyboard.md#physical-keyboard-simulation)、[手柄](doc/gamepad.md)、[OTG 模式](doc/otg.md) 等

完整用法见 [连接](doc/connection.md)、[控制](doc/control.md) 等文档与 [常用快捷键](doc/shortcuts.md)。

## 本 fork 新增：输入映射系统

把电脑输入映射成设备屏幕上的触摸操作，配置全部以 JSON 模板保存，运行时热重载。

### 主要特性

- **映射模板**：`templates/*.json`，文件名即模板名；`scrcpy_mapping_config.json` 中的 `active_template` 指定当前模板
- **映射类型**：
  - `touch`（默认）：按键映射到屏幕坐标，支持 `positions` **多指同时触控**（含主位置最多 5 个）
  - `slide`：按键触发一段带方向、时长的滑动
  - `motion`（`MOUSE_MOVE`）：把鼠标相对位移转成连续触摸滑动，支持 `sensitivity` 灵敏度与 `hold: "always"` 视角模式（到屏幕边缘自动回绕继续转动）
  - **虚拟键** `virtual: true`：不注入触摸，仅维护逻辑状态，供依赖条件使用
- **组合键**：`ctrl+shift+a` 形式，2~4 个成员
- **条件依赖** `depends_on`：可依赖物理按键/组合键、`entry:<条目名>`（另一映射的逻辑状态）或 `flag:<功能名>`；写成数组时多条件按“与”组合，并支持 `depends_state` 指定期望状态，带环检测
- **`toggle` 模式**：按键翻转式开关（仅单键 touch / 虚拟键）
- **运行时管理**：F2 切换模板、F3 设计模式、F4 启用/禁用映射、F5 打开内建管理面板；模板与配置文件变更后约 500ms 内自动热重载
- **坐标语义**：`x`/`y` 是相对解码后视频帧的归一化比例 `[0,1]`，与窗口大小、缩放、黑边无关

完整的字段定义、校验规则与文法见 **[映射模板 JSON 格式](doc/mapping-template-json.md)**。

### 模板示例

```json
{
  "name": "default",
  "mappings": [
    { "key": "w", "x": 0.15625, "y": 0.67963 },
    { "key": "lshift", "x": 0.97708, "y": 0.83333 },
    { "key": "w+a", "x": 0.12917, "y": 0.68704 },
    {
      "type": "slide",
      "key": "Up",
      "x": 0.49896, "y": 0.49630,
      "dx": 0, "dy": -0.37037,
      "duration_ms": 333
    },
    {
      "type": "motion",
      "key": "MOUSE_MOVE",
      "sensitivity": 1.0,
      "hold": "always"
    },
    { "key": "q", "virtual": true },
    {
      "key": "e",
      "x": 0.9, "y": 0.5,
      "depends_on": "entry:q",
      "depends_state": "active"
    }
  ]
}
```

### 全局配置

`<scrcpy.exe 所在目录>/scrcpy_mapping_config.json`：

```json
{
  "shortcuts": {
    "switch_template": "F2",
    "design_mode": "F3",
    "toggle_mapping": "F4",
    "manager_panel": "F5"
  },
  "active_template": "default",
  "enabled": false,
  "stop_keyboard_forwarding": false,
  "stop_keyboard_forwarding_allow_template_mapping": false
}
```

- `stop_keyboard_forwarding`：为 `true` 时**键盘不再转发给设备**——不会在手机上打字，也不会触发设备侧按键（按键事件与文本输入两条路径都被拦住）。映射本身不受影响，鼠标也不受影响，scrcpy 自己的快捷键（`F2`–`F5`、`Alt+Z`、`Alt+C/V`、`F11` 等）仍可用。
- `stop_keyboard_forwarding_allow_template_mapping`：仅在上一个开关为 `true` 时生效；为 `true` 时**模板里确有映射规则的按键仍然照常映射**（单键条目与组合键成员），只是不再转发；为 `false` 时连映射一起停掉，键盘完全静默。

## 快速开始

1. 按 [上游文档](doc/connection.md) 准备好 Android 设备并开启 USB 调试（API ≥ 21）。
2. 按 [Windows](doc/windows.md) / [Linux](doc/linux.md) / [macOS](doc/macos.md) 安装或构建客户端。
3. 将模板放在可执行文件同级的 `templates/` 目录，编辑 `scrcpy_mapping_config.json` 选择模板并设 `"enabled": true`。
4. 启动 scrcpy，用 **F4** 切换映射开关、**F5** 打开管理面板查看加载结果与在线编辑。

## 构建与开发

- 通用构建说明：[doc/build.md](doc/build.md)、[doc/develop.md](doc/develop.md)
- 可视化编辑映射模板（浏览器里拖拽生成 JSON）：[tools/mapping-designer](tools/mapping-designer/README.md)，直接双击 `tools/mapping-designer/index.html`
- 多平台正式版 CI：[build-all.yml](.github/workflows/build-all.yml)。推送 `master`、功能分支或 `v*` / `build-*` tag 时，自动编译 Linux / Windows / macOS 的 x86、x64、ARM 变体并发布 Release；不兼容的变体会被跳过而不阻断其余发布，每个正式版压缩包内都内置上述可视化设计器、`templates/` 与 `scrcpy_mapping_config.json`
- Windows：CI 已切换到 **UCRT64**（MINGW64 已弃用）；`.NET` 管理面板相关构建使用 **PowerShell**
- 映射条件模块的单元测试：`app/tests/test_mapping_cond.c`
- 旧版独立 .NET 管理器源码保留在 `scrcpy-mapping-manager/`，发布包中已改用内建 F5 面板

## 文档索引

- [映射模板 JSON 格式](doc/mapping-template-json.md) — 本 fork 核心特性完整参考
- [连接 / TCP-IP](doc/connection.md) · [控制](doc/control.md) · [键盘](doc/keyboard.md) · [鼠标](doc/mouse.md)
- [视频](doc/video.md) · [音频](doc/audio.md) · [录制](doc/recording.md) · [窗口](doc/window.md)
- [FAQ](FAQ.md) · [映射输入卡顿调查记录](debug-mapping-input-stalls.md)（状态：收集中，尚未定论）
- 上游 Wiki：<https://github.com/Genymobile/scrcpy/wiki>

## 已知问题

鼠标 `MOUSE_MOVE` 连续映射在快速移动/急停/反向时可能出现延迟或短暂卡顿；相关假设与日志埋点记录在 [debug-mapping-input-stalls.md](debug-mapping-input-stalls.md)，尚未形成结论，欢迎提供运行时证据。

## 许可证

    Copyright (C) 2018 Genymobile
    Copyright (C) 2018-2026 Romain Vimont

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

本 fork 的增量改动同样遵循上述许可证。上游项目：<https://github.com/Genymobile/scrcpy>。
