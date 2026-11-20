# 映射模板 JSON 格式

适用分支：`trae/agent-6FTCv4`
本文档与实现同一次提交入库；最新 tip 以 `git log -1 trae/agent-6FTCv4` 为准。
对照实现：`app/src/mapping_manager.c` 的 `parse_template()`（加载）、
`save_active_impl()`（保存）、`sc_mapping_manager_upsert_ex()`（管理面板编辑），
以及 `app/src/input_manager.c` 的注入 / 依赖 / 位移逻辑。

> 本文档描述的是**实际实现**，不是设计设想。每个字段的读取点、写入点和
> 校验规则都可以在上面几个函数里逐行找到。

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
| `key` | string | **是** | 非空字符串。普通键名、组合键文法（见第 5 节）、鼠标按键名（见第 7 节）或保留字 `MOUSE_MOVE` / `MOUSE_LEFT_LOCKED`。 |
| `x` | number | 见下 | 归一化横坐标，闭区间 `[0,1]`。`touch`/`slide` 必填；`motion` 可选（缺省 `0.5`，是回绕重锚点）。 |
| `y` | number | 见下 | 归一化纵坐标，闭区间 `[0,1]`。规则同 `x`。 |
| `type` | string | 否 | `"touch"`（默认）、`"slide"` 或 `"motion"`；其它值整条拒绝。 |
| `dx` | number | slide 必填 | 归一化横向位移，闭区间 `[-1,1]`，可为负。 |
| `dy` | number | slide 必填 | 归一化纵向位移，闭区间 `[-1,1]`，可为负。 |
| `duration_ms` | number | 否 | 仅 slide 有意义。缺省 200；加载时夹到 `[50,5000]`。整数截断（`(int)`）。 |
| `mode` | string | 否 | `"momentary"`（默认）或 `"toggle"`；其它值拒绝。`toggle` 只允许单键 `touch` 条目（鼠标按键可以，组合键 / slide / motion / `MOUSE_LEFT_LOCKED` 拒绝）。 |
| `depends_on` | string | 否 | 依赖键：**单个**键名或鼠标按键名（不能是组合键）。见第 6.2 节。 |
| `depends_state` | string | 否 | `"pressed"`（默认）或 `"released"`；其它值拒绝。没有 `depends_on` 时出现 → 拒绝。 |
| `sensitivity` | number | 否 | 仅 `type: "motion"` 允许，其它类型给出该字段 → 拒绝。缺省 `1.0`，有效范围 `[0.05, 20.0]`，越界夹取（加载时 `LOGW` 告警；面板编辑静默夹取）。见第 9 节。 |
| `hold` | string | 否 | 仅 `type: "motion"` 允许。`"button"`（默认）表示只在按住鼠标键时才有虚拟手指；`"always"` 是**视角模式**：从第一次鼠标移动起虚拟手指一直按住不放，直到映射停止。见第 9.4 节。 |

字段校验顺序（与 `parse_template()` 一致）：

1. 条目必须是 object，否则拒绝（`entry is not an object`）。
2. `key` 必须是非空 string，否则拒绝（`missing or invalid 'key'`）。
3. 与**已接受**条目做大小写不敏感重复检查，重复则拒绝后者（`duplicate key`）。
4. `type` 若存在必须是 string；`"touch"` / `"slide"` / `"motion"` 大小写不敏感；否则拒绝（`unknown type`）。
5. `type: "motion"` 与 `key: "MOUSE_MOVE"` 必须成对出现：
   - 类型是 motion 但键不是 `MOUSE_MOVE` → `type 'motion' requires key 'MOUSE_MOVE'`；
   - 键是 `MOUSE_MOVE` 但类型不是 motion → `'MOUSE_MOVE' requires type 'motion'`。
6. `x`/`y`：两者都是 number 时校验范围；只出现一个或类型不对 → `'x'/'y' must both be numbers`；两者都缺失时只有 `motion` 允许（使用默认 `0.5, 0.5`），其它类型拒绝（`missing or invalid 'x'/'y'`）。越界 `[0,1]` **不夹取**，直接拒绝。
7. `mode` 非 string 或值未知 → 拒绝。
8. `depends_on` 非空 string 且不能含 `+`；随后解析键名，未知 → 拒绝。`depends_state` 规则见上表。
9. `sensitivity` 非 number → 拒绝；非 motion 类型出现 → 拒绝（`'sensitivity' is only valid for type 'motion'`）；数值夹到 `[0.05, 20.0]` 并 `LOGW` 告警。
10. `hold` 非 string → 拒绝；非 motion 类型出现 → 拒绝（`'hold' is only valid for type 'motion'`）；取值不是 `button` / `always` → 拒绝（`unknown hold '...'`）。
11. 鼠标保留字 `MOUSE_LEFT_LOCKED` / `LEFT_CLICK_LOCKED` 的特殊分支：不允许 slide/motion（`cannot be a slide` / `cannot be a motion`）、不允许 `mode: toggle`、不允许 `depends_on`。
12. `type: "motion"` 分支：不允许 `mode: toggle`；其余字段（`depends_on`、`hold`）允许。
13. 其余键名走键表达式文法（第 5 节）；解析失败拒绝。**鼠标按键名也算单键**。
14. `mode: toggle` 要求单键：组合键拒绝（`'mode: toggle' requires a single key, not a combo`）；slide 拒绝（`'mode: toggle' is not supported for slides`）。
15. `depends_on` 与本条目的单键触发键相同 → 拒绝（`'...' depends on itself`）。
16. slide：`key` 必须是单个键（组合键拒绝）；`dx`/`dy` 必须都是 number 且落在 `[-1,1]`；`duration_ms` 若存在必须是 number，然后夹取到 `[50,5000]`（夹取只告警，不拒绝）。
17. 跨条目规则：slide 键不得同时作为某个组合键的成员，否则该 slide 条目被拒绝（`slide key '...' is also used in a combo`）。

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
- `motion` 条目的 `x`/`y` 是**起点 / 回绕重锚点**：默认（`hold: "button"`）
  在按下任一鼠标键后第一次产生位移时在该点注入 DOWN，之后按相对位移移动；
  越界回绕后再从该点重新 DOWN。`hold: "always"` 时在该点注入 DOWN 后不再
  回绕，只在映射停止时 UP（见第 9 节）。

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
  - 成员是鼠标按键：`mouse button '...' cannot be a combo member in '...'`；
  - 组合成员数 < 2（仅在含有 `+` 的表达式上可能出现）：`combo needs at least 2 members`。
- 成员两端空白会被裁剪（`ctrl + a` 与 `ctrl+a` 等价）。
- 组合键按下时按“任一成员松开即释放”语义触发（`update_combos()`）。
- 组合键不能与滑动键共用键名（见第 3 节第 16 条）。
- 组合键可以携带 `depends_on`：依赖不满足时不触发（见第 6.2 节）。

## 6. 键名文法

### 6.1 单个键名

按以下顺序解析（`sc_mapping_parse_key_name()`），全部大小写不敏感：

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

2. 鼠标按键别名（见第 7 节），命中则返回合成键码。
3. 单个字母/数字字符：转为大写后映射到 `a`..`z` / `0`..`9`。
4. `SDL_GetKeyFromName()`（按原文）。
5. `SDL_GetKeyFromName()`（整串转大写后的回退）。
6. 全部失败 → `SDLK_UNKNOWN`，该条目拒绝。

保存时 `key` 原样写回内存中的字符串（`key_name`），不做规范化：`Ctrl+A` 保存后仍是 `Ctrl+A`。

### 6.2 依赖键（`depends_on`）

- 只接受**单个**键名（同一个解析函数）或鼠标按键名；含 `+` 直接拒绝
  （`dependency must be a single key, not a combo`）。
- 不允许依赖条目自身的单键触发键（`depends on itself`）。
- 依赖键可以**没有**自己的映射条目：这类键仍会被跟踪按下状态
  （`sc_mapping_manager_key_is_dependency()`），并且按键事件继续正常转发。
- `depends_state` 缺省 `pressed`：

| 取值 | 含义 |
| --- | --- |
| `pressed` | 依赖键处于按下状态时映射才存在 |
| `released` | 依赖键处于松开状态时映射才存在 |

运行时行为（`evaluate_dependencies()`）：

- 触发键按下时依赖不满足 → 不产生映射，日志
  `Mapping '...' not entered: dependency '...' is not pressed/released`。
- 映射已激活后依赖丢失 → 立即退出映射并补发一次 UP（`Mapping '...' released: dependency lost`），不会漏发。
- 依赖重新满足时：
  - `momentary` 条目在触发键仍按住的情况下自动重新进入；
  - `toggle` 条目**不会**自动重新按下，必须等下一次显式按下边沿。

## 7. 鼠标按键与保留字

### 7.1 可映射的鼠标按键

| 名称（大小写不敏感） | 别名 | SDL 按钮号 |
| --- | --- | --- |
| `MOUSE_LEFT` | `LEFT_CLICK` | 1（左键） |
| `MOUSE_MIDDLE` | `MIDDLE_CLICK` | 2（中键） |
| `MOUSE_RIGHT` | `RIGHT_CLICK` | 3（右键） |
| `MOUSE_X1` | `MOUSE_BUTTON4` | 4（侧键 X1） |
| `MOUSE_X2` | `MOUSE_BUTTON5` | 5（侧键 X2） |

- 实现上它们被解析成**合成键码** `-1000 - button`（真实 SDL 键码永远不为负），
  因此与键盘键不冲突、也不会被键盘事件产生。
- 它们是普通**单键**条目：`key` 写名字即可，`type` 缺省 `touch`，也可以写
  `slide`（按住鼠标键持续滑动）或 `mode: toggle`。
- 不能作为组合键成员（第 5 节）；不能用作 `MOUSE_LEFT_LOCKED` 的替代。
- 生效条件：映射总开关 `enabled == true`、非设计模式、已连接且未暂停。
  绑定后该鼠标键的事件被消费（不转发给设备），并在按下/松开时注入
  DOWN/UP（`momentary`）或翻转（`toggle`）。
- 同一个合成键码被两个别名条目同时占用时（例如 `MOUSE_RIGHT` 与
  `RIGHT_CLICK` 各写一条），加载都能通过（`key` 字符串不同），但查找
  `sc_mapping_manager_find_single()` 按条目顺序返回**第一条**，第二条不会触发。
- 保存时 `key` 保持条目里的原文字符串，不改成规范名。

### 7.2 保留字 `MOUSE_LEFT_LOCKED` / `LEFT_CLICK_LOCKED`

左键锁定模式：用**左 Alt**（`SDL_LALT`，不带 Ctrl/Shift）切换锁定状态；
锁定后左键按下注入 DOWN、松开注入 UP。限制：

- 不能是 `type: "slide"` 或 `"motion"`；
- 不能使用 `mode: toggle`；
- 不能使用 `depends_on`；
- 判定优先于普通 `MOUSE_LEFT` 条目。

### 7.3 保留字 `MOUSE_MOVE`

鼠标位移映射（`type: "motion"`）。它不是按键、没有键码，默认在**按住任意鼠标键
（1..5）并移动鼠标**时驱动一条独立的滑动指针；写 `"hold": "always"` 后变成
视角模式，不需要按住鼠标键，手指一直按住直到映射停止。字段与运行时见第 9 节。
管理面板里按 `V` 可直接创建该条目，按 `H` 切换 hold。

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
    { "key": "MOUSE_X1", "x": 0.25, "y": 0.85 },
    { "key": "LEFT_CLICK_LOCKED", "x": 0.25, "y": 0.75 },
    { "key": "q", "x": 0.1, "y": 0.9, "mode": "toggle" },
    { "key": "e", "x": 0.2, "y": 0.9,
      "depends_on": "lshift", "depends_state": "pressed" },
    { "type": "motion", "key": "MOUSE_MOVE",
      "x": 0.7, "y": 0.55, "sensitivity": 2.4, "hold": "always" }
  ]
}
```

说明：

- 前两条是普通单键点按（缺省 `type` 即 `touch`）。
- 第三条是 3 成员组合键。
- 第四条按住 Up 会从 `(0.499, 0.496)` 向 `dy = -0.370` 方向持续滑动，每段约 333ms，松手即停；段与段之间补 UP 再补 DOWN，不做反向瞬移。
- 第五、六条把右键 / 侧键 X1 变成可映射按键（点按）。
- 第七条是左键锁定条目。
- 第八条 `q` 是切换型：第一次按下注入 DOWN 并保持，再按一次注入 UP。
- 第九条 `e` 只在 `lshift` 处于按下状态时生效。
- 第十条是视角模式：虚拟手指锚定在画面右侧 `(0.7, 0.55)`，鼠标一动就按住并持续跟随鼠标位移，直到映射停止才抬起。

## 9. 鼠标位移映射（`type: "motion"`）

### 9.1 字段含义

| 字段 | 含义 |
| --- | --- |
| `key` | 固定为 `MOUSE_MOVE`（唯一允许的键）。 |
| `x` / `y` | 虚拟手指的起点（`hold: "button"` 时也是回绕重锚点），缺省 `0.5`/`0.5`。 |
| `sensitivity` | 灵敏度，缺省 `1.0`，有效范围 `[0.05, 20.0]`。 |
| `hold` | 手指保持方式：`"button"`（默认，按住鼠标键才按住手指）或 `"always"`（视角模式，手指永不抬起）。见 9.4。 |
| `depends_on` / `depends_state` | 可选。依赖不满足时既不注入也不消费鼠标位移。 |

换算公式（`motion_feed()`）：

```text
dnx = xrel * sensitivity / frame_width
dny = yrel * sensitivity / frame_height
```

即 `sensitivity = 2.0` 表示鼠标移动 1 px，设备上移动 2 px。除数是**当前**
帧尺寸（与第 4 节同理），所以缩放 / 旋转后手感一致。

### 9.2 参与条件（全部满足才注入）

1. 模板里存在 `MOUSE_MOVE` 条目（唯一，重复键名会被加载拒绝）；
2. 映射总开关 `enabled == true`；
3. 已连接、未暂停、非设计模式、未打开 F2 切换弹窗 / F5 管理面板；
4. `hold` 为 `always` 时无按键门槛；为 `button`（默认）时至少有一个鼠标键
   （1..5）处于物理按下状态；
5. 条目依赖（若有）满足。

参与时鼠标位移事件被**消费**，不再转发给设备，避免物理指针与合成滑动
同时争用同一块屏幕。条件 4/5 不满足时调用 `motion_stop()` 补 UP，并恢复正常
转发（物理指针继续工作）。

### 9.3 运行时细节与取舍

- **指针 id**：使用该条目自己的分配 id（`-(4+slot)`），与滑动条目、单键条目
  互不冲突；不占用 scrcpy 保留的 `-1/-2/-3`。
- **限流**：两次注入之间至少间隔 `8 ms`（`MOTION_MIN_INTERVAL_MS`），期间
  的位移累加（`pend_nx/pend_ny`），到点一次性注入累加值；不会把每个鼠标
  事件都塞进控制队列（队列上限 60 条）。
- **DOWN 时机**：`hold: "button"` 时，按住鼠标键后收到的第一条鼠标移动事件
  就注入 DOWN（在 `x`/`y`），即使该事件位移为 0；只按下不移动不会产生滑动。
  `hold: "always"` 时，启用状态下收到的第一条鼠标移动事件注入 DOWN，此后
  不再重复 DOWN，直到映射停止。
- **越界与回绕**：`hold: "button"` 时，目标点越出 `[0,1]` 就先在**夹取后的
  边界点**注入 `MOVE`，再在同一点注入 `UP`，然后回到 `x`/`y`（重锚点）注入
  `DOWN`。不使用“单条 MOVE 瞬移回起点”，因为设备会把它识别为反向 fling。
  代价是每次回绕留下一个“轻点”痕迹（边界点 DOWN/UP 对），且回绕后位置
  从重锚点重新开始。`hold: "always"` 不做回绕：位置被硬夹在边界，手指始终
  不抬起（原因见 9.4）。
- **停止条件**：`hold: "button"` 时所有鼠标键松开即停；`hold: "always"` 时
  只能靠下面这些事件停止。两种情况下的其余停止事件相同：依赖丢失；
  `enabled=false`；`paused`；连接断开；切模板 / 热重载 / 打开 F2 或 F5
  面板 / 进入设计模式；窗口失焦；程序退出。任一成立都会补 UP
  （`motion_stop()` / `mapping_release_all()`），不留按住不放的残留。
- **与现有滑动的关系**：`motion` 与 `slide` 各自持有独立指针 id，可以同时
  运行；二者都不与物理鼠标指针共用 id（只有 `SC_POINTER_ID_MOUSE` 转发时
  才用）。设备端 `MAX_POINTERS = 10`，同时按住的条目越多越容易触顶，这是
  与原有映射共有的限制。
- **与虚拟鼠标（Alt+拖动）的关系**：参与位移映射时鼠标移动事件被消费，
  虚拟鼠标的 `MOVE` 不再产生；`hold: "button"` 下松开所有鼠标键、
  `hold: "always"` 下映射停止后恢复。这是“同一时刻只有一个指针在动”的
  取舍，未做两路同时注入。
- **未验证项**：真机注入时序、边界回绕手感、与 `slide` 同时运行的观感、
  不同灵敏度下的体感，均未在真机验证（本仓库构建环境无法运行 Android
  设备注入）。上面描述的是代码路径，不是实测结果。

### 9.4 视角模式（`hold: "always"`）

用途：像在 PC 上玩 FPS 一样用鼠标直接转视角，而不是“按住鼠标拖动屏幕”。

```json
{ "type": "motion", "key": "MOUSE_MOVE", "x": 0.55, "y": 0.5,
  "sensitivity": 2.4, "hold": "always" }
```

- **启用方式**：在 motion 条目里写 `"hold": "always"` 并加载模板即可，不需要
  额外按键进入。管理面板（F5）里选中 `MOUSE_MOVE` 条目按 `H` 也能切换。
- **无按键门槛**：不需要按住任何鼠标键；鼠标一动，虚拟手指就在 `x`/`y`
  按下，并随鼠标位移连续移动。鼠标左/右/侧键仍然是普通可映射按键，可以和
  视角模式同时使用（例如左键映射成开火）。
- **只有停止才抬手**：运行期间只注入 DOWN 一次和持续 MOVE，不注入 UP；
  因此设备侧看到的是“一根一直按在屏幕上的手指”。停止条件见 9.3，停止时
  一定补 UP。
- **边界是硬夹取**：手指到屏幕边缘后停在边缘，鼠标继续往同方向推不会再
  产生位移。原因：单根手指的坐标被限制在屏幕内，若不抬手就无法“无限旋转”；
  另外两种做法（到边界抬指回锚点、单条 MOVE 瞬移回锚点）分别会产生视角
  跳变和被设备识别为反向 fling，因此没有采用。实际手感是“一次满屏拖动
  能转多少度由 `sensitivity` 决定”，需要转更多圈时增大灵敏度或反向推动
  鼠标让手指回到画面中部。
- **与依赖共存**：`depends_on` 依然有效。依赖不满足时手指会被抬起，重新
  满足后要等下一次鼠标移动才会再次按下（不会凭空产生按压）。
- **未验证项**：真实游戏中视角旋转方向、灵敏度体感、夹取到边界后的玩家
  感受均未验证；当前 `sensitivity` 只接受正数，没有反向轴 / 反转 Y 轴。

## 10. 切换型映射（`mode: "toggle"`）

- 只允许**单键 touch** 条目：组合键、slide、motion、`MOUSE_LEFT_LOCKED` 都拒绝。
  鼠标按键条目是合法的单键 touch，因此可以切换。
- 语义：**按下边沿**翻转——若该条目当前未按下则注入 DOWN 并保持；若当前
  已按下则注入 UP 并释放。**松开边沿不改变状态**（否则就退化成瞬时映射）。
- 与瞬时映射共存：每个条目独立维护 `active` / `down`，互不影响；组合键
  覆盖同一按键时会先抬起被覆盖的单键条目（沿用原有规则）。
- 依赖丢失时自动抬起（补一次 UP），不会自动重新按下；依赖恢复后等下一次
  按下边沿。
- 松手、热重载、切模板、关闭映射（`enabled=false`）、打开面板、退出时，
  `mapping_release_all()` 会给所有仍标记为按下的条目补 UP 并清空状态，
  切换型不会留下“按住不放”的残影。
- 已知取舍：切换型按下后不依赖物理键保持，若程序异常崩溃仍可能残留手指，
  只能靠服务端超时或重启客户端解决（与所有注入方案相同，未做额外处理）。

## 11. 滑动运行时参数（`duration_ms` 的实际作用）

加载/保存只校验 `duration_ms ∈ [50,5000]`（缺省 200）。运行时
`slide_start()` 再据此计算步进：

- 目标步数 20，`step_ms = duration_ms / 20`；
- 若 `step_ms < 4`，强制 `step_ms = 4`，步数改为 `duration_ms / 4`（0 延迟配置按 4ms 处理并告警）；
- 步数夹到 `[2,120]`；
- 推进由主循环按 `SDL_GetTicks`（`sc_tick_now()`）驱动，没有独立线程；
- 段末补 UP（终点）后，若该键仍按住则从起点补 DOWN 重新开始，不使用单条 MOVE 瞬移回起点（避免被设备识别为反向 fling）。
- 停止条件：键松开、窗口失焦、`paused`、连接断开、`enabled=false`、切模板、热重载、程序退出，任一成立立即补 UP。

## 12. 保存行为与幂等性

`save_active_impl()` 写回活动模板：

- 每个内存条目都写出：`key`、`x`、`y`；
- `type == slide` 时额外写 `"type": "slide"`、`dx`、`dy`、`duration_ms`；
- `type == motion` 时写 `"type": "motion"` 与 `sensitivity`（总是写，缺省 1.0）；
- `hold` 仅在值为 `always` 时写 `"hold": "always"`（默认 `button` 不写）；
- `mode` 仅在值为 `toggle` 时写 `"mode": "toggle"`（默认 `momentary` 不写）；
- 依赖仅在存在时写 `depends_on`（用 `sc_mapping_key_name()` 输出规范名）与
  `depends_state`（`pressed` / `released`，总是写）；
- `touch` 条目不写 `type`/`dx`/`dy`/`duration_ms`（加载时它们本来就被忽略）；
- 根对象写 `name`（活动模板名）与 `mappings`。

因此 **加载 → 保存 → 再加载** 得到同一组条目与同样的语义字段
（`x`/`y`/`dx`/`dy`/`sensitivity` 为 `double`，cJSON 默认以 `%1.15g` 输出，
若 15 位精度不足以无损往返则回退到 `%1.17g`，因此数值往返无损；
`duration_ms` 为整数）。新增的可选字段全部参与往返：`mode`、`depends_on`、
`depends_state`、`sensitivity`、`hold`、`type: motion` 加载后保存不会丢失。

注意两点：

- `depends_on` 保存的是规范键名（来自 `sc_mapping_key_name()`），例如模板里
  写 `MOUSE_BUTTON4`，保存后变成 `MOUSE_X1`；`esc` 变成 `Escape`。语义不变。
- 未识别的额外字段仍会被丢弃（保存只写上述字段），这一点与本次新增字段无关。

写盘策略：

- `save_active()` 只在脏标记置位时写（模板内容被增删改过）；
- `save_active_force()` 无条件写（管理面板里按 `S`）；
- 写入采用“同目录临时文件 `.tmp` + 原子替换”（Windows `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`，POSIX `rename`），读取方不会看到半截文件；
- 保存成功后刷新文件时间戳，避免触发热重载回环。

## 13. 错误处理与兼容规则（逐条对应实现）

| 情况 | 行为 |
| --- | --- |
| 文件不存在 | `parse_template()` 返回 false：`Could not open template`。运行时启动时活动模板加载失败 → `enabled` 置 false（`Active template '...' not found: mapping disabled`）；热重载失败保留原有条目。切换模板失败保留旧模板（先加载成功再改活动名与配置）。 |
| JSON 非法 | 同上，`Invalid JSON in template`；旧条目保留。 |
| 根不是 object | `Template root is not an object`；整个文件失败。 |
| 缺 `mappings` / 不是数组 | `Template has no 'mappings' array`；整个文件失败。 |
| 条目不是 object | 该条拒绝：`entry is not an object`。 |
| `key` 缺失/空/非字符串 | 该条拒绝：`missing or invalid 'key'`。 |
| `key` 重复（大小写不敏感，且前一条已被接受） | 后者拒绝：`duplicate key '...'`（前一条若因其它原因已被拒，后一条可被接受）。 |
| `x`/`y` 缺失/非数字（非 motion） | 该条拒绝：`missing or invalid 'x'/'y'`。 |
| `x`/`y` 只出现一个或非数字（motion） | 该条拒绝：`'x'/'y' must both be numbers`。 |
| `x`/`y` 越界 `[0,1]` | 该条拒绝（不夹取）。 |
| `type` 非字符串 | 该条拒绝：`'type' must be a string`。 |
| 未知 `type` | 该条拒绝：`unknown type '...'`，计入失败统计。 |
| motion 类型但键不是 `MOUSE_MOVE` | 该条拒绝：`type 'motion' requires key 'MOUSE_MOVE'`。 |
| `MOUSE_MOVE` 但类型不是 motion | 该条拒绝：`'MOUSE_MOVE' requires type 'motion'`。 |
| `mode` 非字符串/未知值 | 该条拒绝：`'mode' must be a string` / `unknown mode '...'`。 |
| `mode: toggle` + 组合键 | 该条拒绝：`'mode: toggle' requires a single key, not a combo`。 |
| `mode: toggle` + slide | 该条拒绝：`'mode: toggle' is not supported for slides`。 |
| `mode: toggle` + motion | 该条拒绝：`'mode: toggle' is not supported for motion`。 |
| `MOUSE_LEFT_LOCKED` + `mode: toggle` | 该条拒绝：`cannot use 'mode: toggle'`。 |
| `depends_on` 非字符串/空 | 该条拒绝：`missing or invalid 'depends_on'`。 |
| `depends_on` 含 `+` | 该条拒绝：`dependency must be a single key, not a combo`。 |
| `depends_on` 键名未知 | 该条拒绝：`unknown dependency key '...'`。 |
| `depends_state` 没有 `depends_on` | 该条拒绝：`'depends_state' requires 'depends_on'`。 |
| `depends_state` 非字符串/未知值 | 该条拒绝：`'depends_state' must be a string` / `unknown depends_state '...'`。 |
| 条目依赖自身触发键 | 该条拒绝：`'...' depends on itself`。 |
| `MOUSE_LEFT_LOCKED` + `depends_on` | 该条拒绝：`cannot use 'depends_on'`。 |
| `sensitivity` 非数字 | 该条拒绝：`'sensitivity' must be a number`。 |
| `sensitivity` 出现在非 motion 条目 | 该条拒绝：`'sensitivity' is only valid for type 'motion'`。 |
| `sensitivity` 越界 | 夹取到 `[0.05,20.0]` 并告警，条目保留。 |
| `hold` 非字符串 | 该条拒绝：`'hold' must be a string`。 |
| `hold` 出现在非 motion 条目 | 该条拒绝：`'hold' is only valid for type 'motion'`。 |
| `hold` 取值未知 | 该条拒绝：`unknown hold '...'`。 |
| slide 缺 `dx`/`dy` 或非数字 | 该条拒绝：`slide requires numeric 'dx' and 'dy'`。 |
| slide `dx`/`dy` 越界 `[-1,1]` | 该条拒绝。 |
| slide 用组合键 | 该条拒绝。 |
| slide 键同时出现在某个组合键 | 该 slide 条目拒绝。 |
| `duration_ms` 非数字 | 该条拒绝：`'duration_ms' must be a number`。 |
| `duration_ms` 越界 | 夹取到 `[50,5000]` 并告警，条目保留。 |
| 鼠标锁定条目指定 `type: "slide"` | 该条拒绝：`cannot be a slide`。 |
| 未识别的额外字段 | 忽略，不影响加载；保存后不再写出。 |
| 条目数 > 256 | 超出部分逐条拒绝：`mapping table full`。 |

管理面板（F5）编辑走 `sc_mapping_manager_upsert_ex()`，它复用同一套
`parse_key_expr()` 与区间检查，另外在保存前拦截：空键名、`x`/`y` 越界、
组合超限、slide 用组合键、slide/组合键冲突、鼠标锁定条目设为 slide、
toggle 用于非单键 touch、依赖为组合键或依赖自身、灵敏度仅 motion 可设。

> 同一规则在加载器与管理面板里的**措辞**可能略有不同（例如 toggle 用于
> slide 时加载器报 `'mode: toggle' is not supported for slides ('...')`，
> 面板报 `'mode: toggle' is only supported for touch entries`），拒绝行为一致。

面板按键：`1`-`6` 编辑字段、`Enter` 提交、`N` 新建（再按一个键）、
`V` 新建 `MOUSE_MOVE`、`T` 切换 touch/slide、`M` 切换 momentary/toggle、
`H` 切换 hold（`button` / `always`，仅 motion）、
`D` 循环依赖（无 → 按键捕获 pressed → released → 无）、`S` 保存。
面板编辑只改内存，按 `S`（或关闭面板时若脏）才写回文件。

## 14. 配置文件的快捷键字段（不属于模板，但同目录同机制）

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
