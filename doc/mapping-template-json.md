# 映射模板 JSON 格式

适用分支：`trae/agent-6FTCv4`
撰写时分支 tip：`a9ed8b82`（本文档随后提交入库，最新 tip 以
`git log -1 trae/agent-6FTCv4` 为准）。
格式相关实现基线：`836aa5c5`（任务三提交，含全部字段读写代码）。
对照实现：`app/src/mapping_manager.c` 的 `parse_template()`（加载）与
`save_active_impl()`（保存），以及 `app/src/input_manager.c` 的注入逻辑。

> 本文档描述的是**实际实现**，不是设计设想。每个字段的读取点、写入点和
> 校验规则都可以在上面两个函数里逐行找到。

## 1. 文件位置与发现规则

- 模板目录：`<scrcpy.exe 所在目录>/templates/`。
- 文件名即模板名：`templates/default.json` 对应模板名 `default`。
- 模板列表来自该目录下 `*.json`（扩展名大小写不敏感）的枚举；文件名去掉
  `.json` 就是模板名（`sc_mapping_manager_list()`）。
- 当前活动模板记录在 `<exe 目录>/scrcpy_mapping_config.json` 的
  `active_template` 字段；该字段缺省为 `default`。
- 模板文件与配置文件都在运行时按 mtime(100ns)+文件大小监视变化，变化后
  500ms 内自动热重载（`sc_mapping_manager_reload_if_changed()`）。
- 单个模板最多 256 个条目（`SC_MAPPING_MAX_ENTRIES`）；超出部分逐条拒绝并
  计入失败统计。

## 2. 根对象

```json
{
  "name": "default",
  "mappings": [ ... ]
}
```

| 字段 | 类型 | 必填 | 实际行为 |
| --- | --- | --- | --- |
| `name` | string | 否 | **加载时完全不读取**。模板名来自文件名。保存时由 `save_active_impl()` 写回当前活动模板名，仅作注释用途。 |
| `mappings` | array | **是** | 条目数组。缺失或不是数组 → 整个文件加载失败，保留旧条目，日志 `Template has no 'mappings' array`。 |

- 根节点必须是 JSON object，否则整个文件加载失败（日志 `Template root is not an object`）。
- 未知的额外根字段会被忽略（实现只按名字取 `name`/`mappings`）。
- 条目按数组顺序加载，顺序保留在内存表中。

## 3. 条目字段

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `key` | string | **是** | 非空字符串。普通键名、组合键文法（见第 5 节）或鼠标保留字（见第 7 节）。 |
| `x` | number | **是** | 归一化横坐标，闭区间 `[0,1]`。 |
| `y` | number | **是** | 归一化纵坐标，闭区间 `[0,1]`。 |
| `type` | string | 否 | `"touch"`（默认）或 `"slide"`；其它值整条拒绝。 |
| `dx` | number | slide 必填 | 归一化横向位移，闭区间 `[-1,1]`，可为负。 |
| `dy` | number | slide 必填 | 归一化纵向位移，闭区间 `[-1,1]`，可为负。 |
| `duration_ms` | number | 否 | 仅 slide 有意义。缺省 200；加载时夹到 `[50,5000]`。整数截断（`(int)`）。 |

字段校验顺序（与代码一致）：

1. 条目必须是 object，否则拒绝（`entry is not an object`）。
2. `key` 必须是非空 string，否则拒绝（`missing or invalid 'key'`）。
3. 与**已接受**条目做大小写不敏感重复检查，重复则拒绝后者（`duplicate key`）。
4. `x`/`y` 必须都是 number，否则拒绝（`missing or invalid 'x'/'y'`）。
5. `x`/`y` 越界 `[0,1]` → 拒绝（**不夹取**，`'x'/'y' out of range [0,1]`）。
6. `type` 若存在必须是 string；`"touch"` / `"slide"` 大小写不敏感；其它值拒绝（`unknown type`）。
7. 鼠标保留字：不能是 slide（`cannot be a slide`）；其余字段按普通点按处理。
8. 其余键名走组合键文法（第 5 节）；解析失败拒绝。
9. slide：`key` 必须是单个键（组合键拒绝，`slide key must be a single key, not a combo`）；`dx`/`dy` 必须都是 number 且落在 `[-1,1]`；`duration_ms` 若存在必须是 number，然后夹取到 `[50,5000]`（夹取只告警，不拒绝）。
10. 跨条目规则：slide 键不得同时作为某个组合键的成员，否则该 slide 条目被拒绝（`slide key '...' is also used in a combo`）。

所有拒绝都会 `LOGW` 一行（含模板路径与条目下标），并计入该模板的失败统计：
`sc_mapping_manager.last_load` 的 `fail` 计数与最多 16 条 `msgs` 文本，
管理面板（F5）头部显示 `loaded: N ok, M rejected`。

## 4. 坐标语义

- `x`/`y` 是相对于**解码后视频帧**的比例，与窗口大小、缩放、黑边无关。
- 触摸坐标在**每次注入时**按**当前** `frame_size` 换算成像素
  （`inject_touch_norm()`），不是按键按下时快照；因此旋转、`--max-size`、
  `--crop` 导致帧尺寸变化后仍然落在同一相对位置。
- 服务端 `PositionMapper` 要求发送的 `screen_size` 与当前视频尺寸一致；
  用当前帧尺寸换算正是为了通过这项检查。
- 加载时越界的 `x`/`y` 直接拒绝（不夹取）。运行时注入前内部仍会做
  `[0,1]` 夹取（`inject_touch_norm_once()` 对负像素归零），但这只是防御，
  正常模板不会触发。
- 滑动中的实时位置由 `nx + ndx * t`、`ny + ndy * t` 计算（`t ∈ [0,1]`），
  并在注入前夹到 `[0,1]`；所以 `dx`/`dy` 可以指向帧外，实际会被边界截断。

## 5. 组合键文法

- 分隔符：`+`。例：`ctrl+shift+a`。
- 成员数：**2..4**（`SC_MAPPING_MAX_KEYS = 4`）。
- 不含 `+` 的表达式是**单键**（`nkeys = 1`），不是组合键。
- 拒绝规则（整条拒绝，计入失败统计）：
  - `+` 总数 > 3（即成员 > 4）：`too many combo members (N > 4) in '<原文>'`；
  - 空成员（`a+`、`+a`、`a++b`）或纯空白成员：`empty combo member`；
  - 单个成员长度 ≥ 64 字节：`combo member too long`；
  - 成员键名无法解析：`unknown key '...' in '...'`；
  - 成员重复：`duplicate combo member`；
  - 组合成员数 < 2（仅在含有 `+` 的表达式上可能出现）：`combo needs at least 2 members`。
- 成员两端空白会被裁剪（`ctrl + a` 与 `ctrl+a` 等价）。
- 组合键按下时按“任一成员松开即释放”语义触发（`update_combos()`）。
- 组合键不能与滑动键共用键名（见第 3 节第 10 条）。

## 6. 键名文法

单个键名按以下顺序解析（`sc_mapping_parse_key_name()`），全部大小写不敏感：

1. 别名表：

   | 别名 | 结果 |
   | --- | --- |
   | `space` | `SDLK_SPACE` |
   | `shift` / `lshift` / `left shift` | `SDLK_LSHIFT` |
   | `rshift` / `right shift` | `SDLK_RSHIFT` |
   | `ctrl` / `lctrl` / `left ctrl` | `SDLK_LCTRL` |
   | `rctrl` / `right ctrl` | `SDLK_RCTRL` |
   | `alt` / `lalt` / `left alt` | `SDLK_LALT` |
   | `ralt` / `right alt` | `SDLK_RALT` |
   | `esc` / `escape` | `SDLK_ESCAPE` |
   | `enter` / `return` | `SDLK_RETURN` |
   | `tab` | `SDLK_TAB` |
   | `backspace` | `SDLK_BACKSPACE` |

2. 单个字母/数字字符：转为大写后映射到 `a`..`z` / `0`..`9`。
3. `SDL_GetKeyFromName()`（按原文）。
4. `SDL_GetKeyFromName()`（整串转大写后的回退）。
5. 全部失败 → `SDLK_UNKNOWN`，该条目拒绝。

保存时 `key` 原样写回内存中的字符串（`key_name`），不做规范化：`Ctrl+A` 保存后仍是 `Ctrl+A`。

## 7. 鼠标保留字

| 名称（大小写不敏感） | 含义 |
| --- | --- |
| `MOUSE_RIGHT` / `RIGHT_CLICK` | 右键固定映射到该条目的 `x`/`y`。 |
| `MOUSE_LEFT_LOCKED` / `LEFT_CLICK_LOCKED` | 左键锁定模式：左键按下/松开映射到该条目的 `x`/`y`。 |

生效条件（`input_manager.c`）：

- 映射总开关 `enabled == true`；
- 不处于设计模式、已连接且未暂停（`control && !paused && !disconnected`）；
- `MOUSE_LEFT_LOCKED` 还需要用**左 Alt**（`SDL_LALT`，不带 Ctrl/Shift）切换锁定状态；锁定后左键按下注入 DOWN、松开注入 UP；
- `MOUSE_RIGHT` 在右键按下/松开时分别注入 DOWN/UP。

限制：

- 鼠标条目不能是 `type: "slide"`；
- 鼠标条目不是键表达式，不会被解析成组合键，也不能作为组合键成员（组合成员必须是真实键名）；
- 鼠标条目在模板中缺省不存在；默认模板为空映射，且 `enabled` 缺省为 `false`。

同一模板里同时出现 `MOUSE_RIGHT` 与 `RIGHT_CLICK` 两个条目时，加载都能通过
（`key` 字符串不同、重复检查不命中），但鼠标索引取扫描到的**最后一个**。

## 8. 完整示例

`templates/example.json`：

```json
{
  "name": "example",
  "mappings": [
    { "key": "w", "x": 0.15625, "y": 0.67963 },
    { "key": "space", "x": 0.84375, "y": 0.92593 },
    { "key": "ctrl+shift+a", "x": 0.5, "y": 0.4 },
    { "type": "slide", "key": "Up", "x": 0.49896, "y": 0.49630,
      "dx": 0.0, "dy": -0.37037, "duration_ms": 333 },
    { "key": "MOUSE_RIGHT", "x": 0.75, "y": 0.5 },
    { "key": "LEFT_CLICK_LOCKED", "x": 0.25, "y": 0.75 }
  ]
}
```

说明：

- 前两条是普通单键点按（缺省 `type` 即 `touch`）。
- 第三条是 3 成员组合键。
- 第四条按住 Up 会从 `(0.499, 0.496)` 向 `dy = -0.370` 方向持续滑动，每段约 333ms，松手即停；段与段之间补 UP 再补 DOWN，不做反向瞬移。
- 后两条是鼠标保留字。

## 9. 滑动运行时参数（`duration_ms` 的实际作用）

加载/保存只校验 `duration_ms ∈ [50,5000]`（缺省 200）。运行时
`slide_start()` 再据此计算步进：

- 目标步数 20，`step_ms = duration_ms / 20`；
- 若 `step_ms < 4`，强制 `step_ms = 4`，步数改为 `duration_ms / 4`（0 延迟配置按 4ms 处理并告警）；
- 步数夹到 `[2,120]`；
- 推进由主循环按 `SDL_GetTicks`（`sc_tick_now()`）驱动，没有独立线程；
- 段末补 UP（终点）后，若该键仍按住则从起点补 DOWN 重新开始，不使用单条 MOVE 瞬移回起点（避免被设备识别为反向 fling）。
- 停止条件：键松开、窗口失焦、`paused`、连接断开、`enabled=false`、切模板、热重载、程序退出，任一成立立即补 UP。

## 10. 保存行为与幂等性

`save_active_impl()` 写回活动模板：

- 每个内存条目都写出：`key`、`x`、`y`；
- `type == slide` 时额外写 `"type": "slide"`、`dx`、`dy`、`duration_ms`；
- `touch` 条目不写 `type`/`dx`/`dy`/`duration_ms`（加载时它们本来就被忽略）；
- 根对象写 `name`（活动模板名）与 `mappings`。

因此 **加载 → 保存 → 再加载** 得到同一组条目与同样的数值（`x`/`y`/`dx`/`dy` 为
`double`，cJSON 默认以 `%1.15g` 输出，若 15 位精度不足以无损往返则回退到
`%1.17g`，因此数值往返无损；`duration_ms` 为整数）。额外未识别的字段不会被
保留（保存只写上述字段）。

写盘策略：

- `save_active()` 只在脏标记置位时写（模板内容被增删改过）；
- `save_active_force()` 无条件写（管理面板里按 `S`）；
- 写入采用“同目录临时文件 `.tmp` + 原子替换”（Windows `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`，POSIX `rename`），读取方不会看到半截文件；
- 保存成功后刷新文件时间戳，避免触发热重载回环。

## 11. 错误处理与兼容规则（逐条对应实现）

| 情况 | 行为 |
| --- | --- |
| 文件不存在 | `parse_template()` 返回 false：`Could not open template`。运行时启动时活动模板加载失败 → `enabled` 置 false（`Active template '...' not found: mapping disabled`）；热重载失败保留原有条目。切换模板失败保留旧模板（先加载成功再改活动名与配置）。 |
| JSON 非法 | 同上，`Invalid JSON in template`；旧条目保留。 |
| 根不是 object | `Template root is not an object`；整个文件失败。 |
| 缺 `mappings` / 不是数组 | `Template has no 'mappings' array`；整个文件失败。 |
| 条目不是 object | 该条拒绝：`entry is not an object`。 |
| `key` 缺失/空/非字符串 | 该条拒绝：`missing or invalid 'key'`。 |
| `key` 重复（大小写不敏感，且前一条已被接受） | 后者拒绝：`duplicate key '...'`（前一条若因其它原因已被拒，后一条可被接受）。 |
| `x`/`y` 缺失/非数字 | 该条拒绝：`missing or invalid 'x'/'y'`。 |
| `x`/`y` 越界 `[0,1]` | 该条拒绝（不夹取）。 |
| `type` 非字符串 | 该条拒绝：`'type' must be a string`。 |
| 未知 `type` | 该条拒绝：`unknown type '...'`，计入失败统计。 |
| slide 缺 `dx`/`dy` 或非数字 | 该条拒绝：`slide requires numeric 'dx' and 'dy'`。 |
| slide `dx`/`dy` 越界 `[-1,1]` | 该条拒绝。 |
| slide 用组合键 | 该条拒绝。 |
| slide 键同时出现在某个组合键 | 该 slide 条目拒绝。 |
| `duration_ms` 非数字 | 该条拒绝：`'duration_ms' must be a number`。 |
| `duration_ms` 越界 | 夹取到 `[50,5000]` 并告警，条目保留。 |
| 鼠标条目指定 `type: "slide"` | 该条拒绝：`cannot be a slide`。 |
| 未识别的额外字段 | 忽略，不影响加载；保存后不再写出。 |
| 条目数 > 256 | 超出部分逐条拒绝：`mapping table full`。 |

管理面板（F5）编辑走 `sc_mapping_manager_upsert()`，它复用同一套
`parse_key_expr()` 与区间检查，另外在保存前拦截：空键名、`x`/`y` 越界、
组合超限、slide 用组合键、slide/组合键冲突、鼠标条目设为 slide。

## 12. 配置文件的快捷键字段（不属于模板，但同目录同机制）

`scrcpy_mapping_config.json`：

```json
{
  "shortcuts": {
    "switch_template": "F2",
    "design_mode": "F3",
    "toggle_mapping": "F4",
    "manager_panel": "F5"
  },
  "active_template": "default",
  "enabled": false
}
```

- 四个快捷键项缺省值即上表；键名用 `sc_mapping_parse_key_name()` 解析，
  解析失败则保留缺省值（不报错）。
- `active_template` 缺省 `default`。
- `enabled` 缺省 `false`；默认模板 `templates/default.json` 为空映射
  （`{"name":"default","mappings":[]}`），因此初次运行不拦截任何按键或鼠标。
