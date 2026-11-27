# scrcpy 控制面板方案（Tauri 版，待确认）

> 与 `doc/control-panel-plan.md` 的 C + WebView2 方案对照。功能目标、目录结构、数据格式、UI 风格完全一致，区别只在控制面板的实现技术：Tauri v2（Rust 后端 + Web 前端，Windows 上同样使用 WebView2 渲染）。

## 1. 已确认的选择（沿用）

| 项目 | 决定 |
|---|---|
| 前端形态 | 独立桌面窗口（Tauri 窗口，Windows 上底层是 WebView2） |
| 入口行为 | 双击 `scrcpy.exe` 打开控制面板；面板里点“启动投屏”才运行 scrcpy |
| 启动参数存储 | 新建 `launch_options.json`，与 `scrcpy_mapping_config.json` 分离 |
| 视觉风格 | NectarLink Hive 风格：蜂蜜金、圆角卡片、左导航、明暗主题、淡蜂巢背景 |
| DLL 布局 | 全部 DLL 放进硬编码的 `dll\` 子目录；不依赖 PATH、不读写注册表 |

## 2. 总体架构

```
┌──────────────────────────────────────────────┐
│  scrcpy.exe = Tauri 控制面板                 │
│  ┌────────────────────────────────────────┐  │
│  │ 前端：HTML + CSS + JS（构建时嵌入 exe）│  │
│  │  - 启动 / 参数 / 变量 / 模板 / 设置     │  │
│  └───────────────┬────────────────────────┘  │
│                  │ Tauri IPC (invoke)         │
│  ┌───────────────▼────────────────────────┐  │
│  │ Rust 后端：读写 JSON、列模板、启动进程  │  │
│  └───────────────┬────────────────────────┘  │
└──────────────────┼───────────────────────────┘
                   │ std::process::Command
                   ▼
        <root>\dll\scrcpy-core.exe
```

- 前端和图标等静态资源由 Tauri 在构建时打包进 exe，运行时不向磁盘释放。
- Rust 侧用 `std::env::current_exe()` 定位安装根目录，所有路径都是**硬编码相对路径**：
  - `dll\scrcpy-core.exe`
  - `dll\*.dll`
  - `templates\`
  - `launch_options.json`、`scrcpy_mapping_config.json`、`version.txt`
- 不读环境变量覆盖路径；不写注册表。升级判断仍由 NSIS 安装程序用 `version.txt` 完成。

## 3. 安装目录结构

与 C 方案完全一致：

```
<安装根目录>\
  scrcpy.exe              ← Tauri 控制面板（双击入口）
  scrcpy-server
  version.txt
  launch_options.json
  scrcpy_mapping_config.json
  templates\*.json
  uninstall.exe
  dll\
    scrcpy-core.exe
    *.dll
```

注意：Tauri 自身**不需要单独的 `WebView2Loader.dll` 随包携带**，Tauri 运行时通过系统安装的 WebView2 Runtime 创建窗口；这比 C 方案少一个随包 DLL，但仍依赖系统 WebView2 Runtime。

## 4. 源码布局

```
tauri-app/
  package.json               # 前端脚本 + @tauri-apps/cli
  index.html                 # 单页入口
  src/
    main.js                  # 页面路由、invoke 调用、命令预览
    style.css                # Hive 主题样式（内联 SVG 蜂巢背景）
  src-tauri/
    Cargo.toml               # tauri = { version = "2", features = [...] }
    tauri.conf.json          # 窗口、图标、frontendDist
    build.rs
    icons/                   # 应用图标
    src/
      main.rs                # tauri::Builder 启动
      lib.rs                 # 注册命令
      commands.rs            # 文件/进程/目录命令实现
      paths.rs               # current_exe → 安装根目录，硬编码相对路径
```

前端建议先用**原生 HTML/CSS/JS**，不引入 React/Vue，避免额外构建复杂度；UI 状态用少量 JS 管理即可。后续若要复杂交互，可以平滑迁移到 React。

## 5. Rust 后端命令（Tauri IPC）

| 命令 | 作用 |
|---|---|
| `read_launch_options` / `write_launch_options` | 读写 `launch_options.json` |
| `read_mapping_config` / `write_mapping_config` | 读写 `scrcpy_mapping_config.json` 中已知字段 |
| `list_templates` | 列出 `templates\*.json`，返回 name / 文件名 / 映射条数 |
| `read_template` / `save_template` | 读取/保存单个模板，保存前校验 JSON |
| `duplicate_template` / `delete_template` | 复制/删除模板文件 |
| `build_launch_command` | 变量展开 + 参数拼接，返回 argv 数组和预览字符串 |
| `launch_scrcpy` | 用 `std::process::Command` 启动 `dll\scrcpy-core.exe`，传入 argv |
| `open_install_dir` | 用 `explorer.exe` 打开安装根目录（需求要求的按钮） |
| `list_devices` | 调用 `adb devices`，返回设备序列号列表 |
| `get_app_info` | 返回版本、安装路径、模板数量 |

关键点：`launch_scrcpy` 使用 argv 数组传参，由 Windows API 正确处理空格和引号，天然避免“启动器透传参数”的兼容问题。

## 6. UI 与数据格式

UI 五个页面、Hive 视觉规范、明暗主题、`launch_options.json` 结构，全部沿用 `doc/control-panel-plan.md` 的第 4、5 节，不再重复。

前端只通过 `invoke("command", args)` 访问文件系统，不直接调用 Node fs，也不使用 Tauri 的宽松权限插件；`tauri.conf.json` 里只启用需要的 capability，减少攻击面。

## 7. CI 与安装程序改动

### 构建流程
1. 现有 MSYS2 任务照常编译 `scrcpy-core.exe`、收集 DLL、server、模板、配置，写入 `version.txt` 到 `dist\`。
2. 新增一个 `windows-latest` Job（或在同一 Job 内）：
   - `dtolnay/rust-toolchain@stable` 安装 Rust
   - `actions/setup-node@v4` 安装 Node
   - `npm install && npm run build` 生成前端 `dist/`
   - `cargo tauri build` 生成 `scrcpy.exe`
   - 把 exe 复制到打包目录根的 `scrcpy.exe`
3. NSIS 脚本继续生成 `scrcpy-setup.exe`，规则不变：替换 `dll\` 和程序文件，保留 `launch_options.json`、配置、`templates\`。

### WebView2 Runtime
- Tauri 官方 NSIS/MSI 打包可以附带 WebView2 引导安装器；但我们用的是自定义 NSIS，所以有两个选择：
  1. CI 下载 WebView2 Runtime Bootstrapper，安装包里静默安装（体积增加约 2MB）；
  2. 不附带，Tauri 启动时检测不到 Runtime 就弹引导页面（方案更轻，但用户需手动装一次）。
- 建议第一版选 2，并在设置页提供“安装 WebView2 Runtime”链接。

## 8. 与 C + WebView2 方案对比

| 维度 | C + WebView2（前一方案） | Tauri（本方案） |
|---|---|---|
| 控制面板二进制 | 约 45KB 起 + 手工 COM 代码 | 约 5–10MB，Rust 标准运行时 |
| 工具链 | 现有 MinGW/GCC 即可 | 需新增 Rust + Node |
| WebView2 API 集成 | 手写 COM、LoadLibrary、消息桥 | Tauri 封装好，IPC 类型安全 |
| UI 开发 | 手写 HTML + 手工桥接 | 标准前端工程，热重载方便 |
| 路径/进程逻辑 | 全 C，需自己处理 JSON | Rust + serde，代码更短更安全 |
| 随包 DLL | 需 `WebView2Loader.dll` 放 `dll\` | 不需要额外 loader DLL |
| WebView2 Runtime 依赖 | 同样需要 | 同样需要 |
| 维护成本 | 低层代码多，后续扩展 UI 累 | 前期工具链重，长期更好维护 |

**我的倾向**：如果你打算后续持续扩展这个面板（更多参数、模板可视化、多主题、检查更新），Tauri 更合适；如果只想要一个够用的小面板、不想引入 Rust/Node，前一方案更轻。

## 9. 风险与应对

| 风险 | 应对 |
|---|---|
| Tauri 版本升级导致配置格式变化 | 锁定 Tauri v2 具体小版本，CI 用 lockfile |
| 系统缺 WebView2 Runtime | 启动检测 + 引导安装；可选把 bootstrapper 打进 NSIS |
| MSYS2 与 Rust/Node 在同一 runner 共存 | 分 Job 或用 PowerShell 步骤，路径隔离 |
| 前端资源未正确嵌入 | 用 Tauri 自带 `frontendDist`，构建产物检查 |
| 升级时覆盖用户数据 | NSIS 规则明确保留 `launch_options.json` / `templates\` / 配置 |
| 参数列表漂移 | 表单从 `cli.c` 整理 + “额外参数”兜底 + 命令预览 |

## 10. 实施顺序

1. **M1**：搭 Tauri 最小工程，窗口渲染 Hive 风格空白页，验证 IPC。
2. **M2**：实现 paths/commands，完成 `launch_options.json` 读写与命令预览、启动 scrcpy。
3. **M3**：五个页面完整实现（启动/参数/变量/模板/设置）。
4. **M4**：主题打磨（蜂巢背景、卡片、明暗切换）。
5. **M5**：CI + NSIS 集成，验证升级保留数据。
6. **M6**：真机实测参数透传与模板编辑。

## 11. 待确认

- 采用本 Tauri 方案，还是回到 C + WebView2 方案？
- WebView2 Runtime 缺失时：只弹引导链接，还是随安装包静默安装？
- 第一版主题：Hive 明暗两套，还是四套全做？
- 启动投屏后，面板窗口自动最小化 / 保持显示 / 做成选项？
- 是否第一版就放“检查 GitHub 新版本”入口（可先占位）？
