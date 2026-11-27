# scrcpy 控制面板方案（待确认）

> 目标：用户不再接触安装根目录里的文件。双击入口直接打开一个 WebView2 桌面窗口，在窗口里配置启动参数、变量、映射模板，并一键启动投屏；保留“打开安装根目录”按钮作为应急入口。

## 1. 已确认的选择

| 项目 | 决定 |
|---|---|
| 前端形态 | WebView2 桌面窗口（独立窗口，不是浏览器标签页） |
| 入口行为 | 双击 `scrcpy.exe` 打开控制面板；面板里点“启动投屏”才运行真正的 scrcpy |
| 启动参数存储 | 新建 `launch_options.json`，与现有 `scrcpy_mapping_config.json` 分离 |
| 视觉风格 | 参考 NectarLink 的 Hive 风格：蜂蜜/琥珀强调色、圆角卡片、左侧导航、明暗主题、细腻蜂巢背景 |
| DLL 布局 | 全部 DLL 放进硬编码的 `dll\` 子目录；不依赖 PATH、不读写注册表 |

## 2. 安装后的目录结构

```
<安装根目录>\
  scrcpy.exe              ← 控制面板（WebView2 窗口），双击入口
  scrcpy-server
  version.txt             ← 安装程序判断新装/升级
  launch_options.json     ← 面板 owns：启动参数、变量、主题
  scrcpy_mapping_config.json  ← C 端 owns：快捷键、活动模板、映射开关
  templates\*.json        ← 映射模板
  uninstall.exe
  dll\
    scrcpy-core.exe       ← 真正的 scrcpy 客户端
    *.dll                 ← 全部运行时 DLL，包括 WebView2Loader.dll
```

说明：控制面板本身在根目录，但它需要的 `WebView2Loader.dll` 在 `dll\`。面板代码用硬编码相对路径 `dll\WebView2Loader.dll` 调用 `LoadLibraryExA` 加载，再 `GetProcAddress` 取 `CreateCoreWebView2EnvironmentWithOptions` 等少量导出函数；不需要 PATH，也不需要把 DLL 放在 exe 旁边。

## 3. 运行流程

1. 用户双击 `scrcpy.exe`（控制面板）。
2. 面板从自身路径推导安装根目录，加载 `dll\WebView2Loader.dll`。
3. 创建 WebView2 环境，窗口内用 `NavigateToString` 加载内嵌的单个 HTML 文档（CSS/JS 全部内联，**不向磁盘释放 UI 文件**）。
4. 页面通过 `window.chrome.webview.postMessage` 向 C 端发请求；C 端通过 `WebMessageReceived` 处理：
   - 读/写 `launch_options.json`
   - 读/写 `scrcpy_mapping_config.json` 中面板负责的字段
   - 列出/读取/保存/复制/删除 `templates\*.json`
   - 启动 `dll\scrcpy-core.exe`，传入拼好的参数
   - 打开安装根目录（`ShellExecuteA("explorer", <root>)`）
5. 点“启动投屏”时，面板按 `launch_options.json` 解析变量、拼接参数，用 `CreateProcessA` 启动 core；参数字符串由面板构造并正确加引号，不经过任何启动器转发。

## 4. UI 设计（Hive 风格）

### 视觉规范
- 配色：蜂蜜金 `#F5B301` / `#FFC933` 为主强调色；浅色底 `#FAF7F0`，深色底 `#1B1714`；卡片白/深灰，圆角 14px，柔和阴影。
- 布局：左侧固定导航（约 220px），顶部标题栏 + 主题切换，右侧内容区卡片流式布局。
- 背景：极淡的六边形蜂巢纹理（CSS/SVG 内联），不用外部图片。
- 主题：默认 Hive 浅色；设置里可切深色，并预留 Bloom / Graphite 两套配色作为后续扩展。

### 页面

**A. 启动（首页）**
- 大号「启动投屏」主按钮；旁边「停止」。
- 设备选择：调用 `adb devices` 列出设备，可手动填 `--serial`。
- 实时命令预览框：显示最终将传给 `scrcpy-core.exe` 的完整命令行，可复制。
- 最近一次配置摘要：分辨率、码率、活动模板、映射开关状态。

**B. 启动参数**
- 表单分组（字段从 `app/src/cli.c` 的参数定义整理，避免遗漏）：
  - 连接：`--serial`、`--tcpip`、`--select-usb`、`--select-usb`
  - 显示：`--window-size`、`--fullscreen`、`--always-on-top`、`--rotation`、`--no-window`
  - 视频：`--max-size`、`--bit-rate`、`--fps`、`--codec`、`--crop`
  - 音频：`--no-audio`、`--audio-source`
  - 输入：`--keyboard`、`--mouse`、`--gamepad`、`--no-control`
  - 设备：`--turn-screen-off`、`--stay-awake`、`--show-touches`
- 每种参数有控件类型（开关/下拉/数字/文本）和帮助说明。
- 底部「额外参数」文本框，兜底任何表单未覆盖的 flag；保存前做基础校验（未知参数仅提示，不拦截）。

**C. 变量**
- key-value 表格，例如 `device=abc123`、`size=1280x720`。
- 参数字段里可用 `${name}` 引用；启动前由面板展开，命令预览同步显示展开结果。
- 支持“插入变量”按钮，把 `${name}` 插到当前光标位置。

**D. 映射模板**
- 左侧模板列表，显示 `templates\*.json` 的 `name` 字段；支持启用/停用、设为活动模板、复制、删除、新建。
- 右侧 JSON 编辑器（带基础高亮和格式校验），保存前解析确认合法；结构遵循现有格式：
  ```json
  { "name": "xxx", "mappings": [ { "key": "w", "x": 0.15, "y": 0.68 } ] }
  ```
  `x/y` 为 0.0~1.0 归一化坐标。
- 「打开 F5 面板拾取坐标」按钮：用设计模式启动 scrcpy，方便用户回到游戏里取点。
- 同步 `scrcpy_mapping_config.json` 的 `active_template`、`enabled` 字段；快捷键字段也在这里编辑（F2/F3/F4/F5）。

**E. 设置**
- 主题切换（Hive 浅色/深色）。
- 安装信息：版本（`version.txt`）、安装根目录、模板数量。
- **「打开安装根目录」按钮**（需求明确要求保留）。
- 关于/开源许可链接；预留“检查更新”占位。

## 5. `launch_options.json` 草案

```json
{
  "schema": 1,
  "theme": "hive-light",
  "device": "",
  "variables": {
    "size": "1280x720"
  },
  "arguments": {
    "max-size": "${size}",
    "bit-rate": "8M",
    "fps": "60",
    "fullscreen": false,
    "turn-screen-off": true,
    "no-audio": false
  },
  "extraArgs": "",
  "activeTemplate": "default"
}
```

规则：
- `arguments` 的 key 用不带 `--` 的参数名；布尔值 true 表示加该 flag，false 表示不加；字符串值转成 `--key=value`。
- 启动时按固定顺序拼参数，再追加 `extraArgs`；变量只展开一层，循环引用给出错误提示。
- 升级时该文件**保留**，不覆盖。

## 6. 与 C 端的衔接

- `scrcpy_mapping_config.json` 结构不变（shortcuts / active_template / enabled），面板只改这些已知字段，C 端解析无需改动。
- 由于真正的 `scrcpy-core.exe` 在 `dll\`，而它的配置/模板需要在安装根目录，`app/src/mapping_manager.c` 已有改动：Windows 下把基路径固定为 exe 所在目录的上一级。此改动保留。
- 启动参数不再由 `scrcpy.exe` 启动器转发，而是面板直接构造命令行启动 `dll\scrcpy-core.exe`；因此不再有启动器对参数的兼容风险。

## 7. 安装程序与 CI 改动

- `windows/installer.nsi`：
  - 安装 `scrcpy.exe` = 控制面板；`version.txt`、`launch_options.json` 默认文件放根目录；`scrcpy-server`、`templates\`、配置照旧。
  - `dll\` 放 `scrcpy-core.exe` + 所有 DLL + `WebView2Loader.dll`。
  - 升级规则不变：替换 `dll\` 和程序文件，保留 `launch_options.json`、`scrcpy_mapping_config.json`、`templates\`、`version.txt` 覆盖为新值。
  - 不读写注册表；继续用 `version.txt` 判断新装/升级。
- `.github/workflows/build.yml`：
  - 新增一步下载并解压固定版本的 `Microsoft.Web.WebView2` NuGet 包，只取 `WebView2.h`。
  - 用 `objcopy` 把单文件 `panel.html` 作为二进制资源嵌入控制面板 exe（不落地、不解压）。
  - 控制面板用动态 `LoadLibraryExA("dll\\WebView2Loader.dll")` + `GetProcAddress`，因此**不需要 MSVC 的 .lib**。
  - Release 仍只上传 `scrcpy-setup.exe`。

## 8. 风险与应对

| 风险 | 应对 |
|---|---|
| 目标机器没有 WebView2 Runtime | 启动时检测，弹窗给出安装引导链接；Win11/新版 Win10 通常已内置 |
| MinGW 下 COM / WebView2 头文件兼容问题 | 只使用 C 接口与 COM vtable，NuGet 头文件固定版本；M1 先做最小窗口验证 |
| WebView2Loader.dll 位置 | 硬编码 `dll\` 路径动态加载，不依赖 PATH/注册表 |
| 参数列表随 scrcpy 版本变化 | 表单从 `cli.c` 整理并保留“额外参数”兜底；预览可人工核对 |
| JSON 编辑写坏模板 | 保存前解析校验，失败不落盘并提示 |
| 控制面板自身遮挡投屏窗口 | 启动投屏后可选择最小化/隐藏面板窗口 |

## 9. 实施顺序

1. **M1**：WebView2 最小窗口 + 内嵌 HTML 渲染 + C↔JS 消息桥验证。
2. **M2**：`launch_options.json` 读写、变量展开、命令预览、启动 core。
3. **M3**：五个页面完整实现（启动/参数/变量/模板/设置）。
4. **M4**：Hive 主题视觉打磨（蜂巢背景、卡片、明暗切换）。
5. **M5**：NSIS + CI 集成，生成安装包并验证升级保留数据。
6. **M6**：真实设备实测参数透传与模板编辑。

## 10. 待确认

- 是否接受 WebView2 Runtime 作为运行时依赖（缺失时给安装引导）？
- 主题默认用 Hive 浅色，是否需要一开始就做四套主题，还是先只做 Hive 明暗两套？
- 启动投屏后面板窗口：自动最小化、保持显示、还是提供选项？
- 是否需要面板内“检查 GitHub 新版本”入口（第一版可先占位）？
