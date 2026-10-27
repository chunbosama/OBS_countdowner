# obs-countdown

给 OBS Studio 用的倒计时源插件。在画面里放倒计时，用快捷键或旁边的控制条就能开始、暂停、停止、重置，不用把鼠标从直播画面上挪开。一个场景里可以同时放多个倒计时，各自计时、各自绑键。

- 目标平台：**Windows 10/11 x64**，**OBS Studio 31.x**
- 当前版本：`0.3.0`（功能范围见 [`mvp.md`](mvp.md)）
- 许可证：GPL-2.0（链接了 GPLv2 的 libobs，分发二进制时必须同时提供源码）

---

## 功能

| 能力 | 说明 |
| --- | --- |
| 倒计时源 | 来源列表里新增 **倒计时**，默认显示 `05:00` |
| 多实例 | 同一场景放几个都行，各走各的，互不干扰 |
| 一实例一套热键 | 每个实例单独注册 4 条热键，在「设置 → 热键」里按源名分组，可以给 A 绑 F1、给 B 绑 F2 |
| 快速控制条 | 停靠窗口列出所有实例，一行一个：状态、剩余时间、开始/暂停/停止/重置按钮 |
| 四个动作 | 开始 / 暂停·继续 / 停止 / 重置，按钮、热键、属性面板三处都能触发 |
| 时长配置 | 时 / 分 / 秒三段输入；运行中改时长不打断当前倒数 |
| 显示格式 | 自动 `MM:SS` / 始终 `HH:MM:SS` / 中文 `5分00秒` / 纯秒数 `300` |
| 外观样式 | 纯数字、**卡片**、**圆环（钟面）**、**进度条** 四种，见下节 |
| 计时精度 | 基于单调时钟记录目标结束时刻，掉帧不漂移；改系统时间不影响 |
| 显隐联动 | **隐藏源或切走场景时自动冻结**，回到画面从冻结处继续 |
| 结束表现 | 停在 `00:00` 并切换为结束颜色（默认红），可选 1Hz 闪烁 3 秒；进度环/条同步归零并变结束色 |
| 样式 | 字体、字号、文字颜色、结束颜色、描边开关/颜色/粗细、水平与垂直对齐 |
| 实例标签 | 给实例起个短名字（如「抽奖」），控制条和属性面板用它显示，留空则用源名 |
| 状态可见 | 属性面板顶部显示「当前状态 · 剩余 · 设定」；控制条实时显示同样的信息 |
| 持久化 | 配置随场景集合保存；控制条位置也记着，重开 OBS 还在老地方 |
| 可分发包 | 自带版本资源、安装器、GPLv2 许可证；`package.ps1` 一键出 zip |

### 外观样式

属性面板的「外观样式」下拉里选，选完只显示这一种样式用得上的参数：

| 样式 | 画面 | 参数 |
| --- | --- | --- |
| 纯数字 | 只有数字，等同 0.2.x | — |
| 卡片 | 数字 + 圆角底卡片 | 内边距、卡片圆角、卡片颜色、卡片不透明度 |
| 圆环（钟面） | 数字居中，外面一圈进度环，可选 12 个钟面刻度 | 内边距、进度粗细、进度颜色、轨道颜色/不透明度、显示刻度 |
| 进度条 | 数字 + 下方一条横向进度条 | 内边距、进度粗细、进度颜色、轨道颜色/不透明度 |

- **进度按剩余时间画**：满值是整圈/整条，走完清空；倒计时结束的那一帧换成「结束颜色」。
- **环的直径按文字外接圆算**，所以长文本（`05:00`）得到的环更大，数字四角不会压到环上。
- 装饰图元是插件自己用顶点缓冲画的纯色几何，不依赖任何额外源；文字仍然由内置文本源渲染。

### 动作语义

| 动作 | 行为 |
| --- | --- |
| 开始 | 从当前剩余时间倒数；已结束时先回满值再跑 |
| 暂停 / 继续 | 同一个热键切换：运行中冻结剩余时间，暂停中从冻结值继续 |
| 停止 | 停住并**保留**剩余时间（下次「开始」接着倒数） |
| 重置 | 停止并把剩余时间恢复为设定时长 |

状态机：`IDLE → RUNNING ⇄ PAUSED`，`RUNNING → FINISHED`，任意状态「重置/停止」回到 `IDLE`。

控制条上的按钮会跟着状态变灰：空闲时只有「开始」「重置」可点；运行中「开始」变灰、「暂停」「停止」亮起；暂停时「开始」显示为「继续」。

---

## 环境要求

编译插件本体：

| 依赖 | 说明 |
| --- | --- |
| OBS Studio 31.x | 已安装，默认路径 `C:\Program Files\obs-studio` |
| Visual Studio 2022 | 勾选「使用 C++ 的桌面开发」工作负载（含 MSVC x64 + Windows SDK 10） |
| Git | 首次构建时用来拉取与 OBS 版本匹配的 libobs 头文件 |
| PowerShell 5.1+ | Windows 自带 |

编译**快速控制条**（默认会编）额外需要 Qt6 开发文件，见下文「准备 Qt」。

运行插件本体：只要 OBS。`Qt6Core/Gui/Widgets.dll`、`obs-frontend-api.dll`、VC 运行库都是 OBS 自带或 OBS 本身就要求的，不用另装。

---

## 编译

```powershell
cd C:\Users\si\WorkBuddy\CountDown

# 构建（产物在 build\）
powershell -ExecutionPolicy Bypass -File .\build.ps1

# 指定 Qt 位置
powershell -ExecutionPolicy Bypass -File .\build.ps1 -QtDir "C:\Qt\6.8.3\msvc2022_64"

# 没有 Qt，只编「多实例 + 每实例热键」那部分
powershell -ExecutionPolicy Bypass -File .\build.ps1 -NoQuickBar

# 构建并直接装进 OBS（需要管理员权限）
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Install

# 干净重建
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean
```

可选参数：

| 参数 | 默认值 | 用途 |
| --- | --- | --- |
| `-ObsDir` | `C:\Program Files\obs-studio` | OBS 安装位置 |
| `-ObsSource` | `.deps\obs-studio` | libobs 头文件所在目录（已有源码时直接指定） |
| `-QtDir` | 自动搜索 | Qt6 开发文件目录（含 `include\QtCore` 和 `lib\Qt6Core.lib`） |
| `-NoQuickBar` | 关 | 不编快速控制条，也不需要 Qt |
| `-SkipTests` | 关 | 跳过核心逻辑测试（默认每次构建都会跑） |
| `-ObsMirror` | `https://gitee.com/mirrors/obs-studio` | 头文件仓库镜像 |
| `-ApiTag` | 本机 OBS 版本 | 用哪个 libobs 版本编译。填更低版本（如 `31.0.0`）可让插件兼容更早的 OBS |
| `-Install` | 关 | 编译后复制到 OBS 安装目录 |
| `-Clean` | 关 | 清空 `build\` 与旧的导入库后重建 |

### 准备 Qt

OBS 安装目录只有 Qt 的 dll，没有头文件。用官方安装器或 `aqtinstall` 拿一份**和 OBS 自带的 Qt 大版本一致**的开发文件即可（OBS 31.1.2 内置 Qt 6.8.3，可从 `bin\64bit\Qt6Core.dll` 的文件版本读到）：

```powershell
pip install aqtinstall
aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --archives qtbase --outputdir C:\Qt
```

`--archives qtbase` 只取基础模块（Core/Gui/Widgets + 头文件），约 200 MB，不用下整个 Qt。

`build.ps1` 会依次在这些位置找：`-QtDir` → `%QTDIR%` → `.deps\Qt` → `C:\Qt` → `%USERPROFILE%\.workbuddy\binaries\qt` → `%USERPROFILE%\Qt`，找到带 `include\QtCore\QObject` + `lib\Qt6Core.lib` 的目录就用。

### 脚本做了什么（以及为什么这样编译）

官方推荐用 `obs-plugintemplate` + `obs-deps` + 完整的 OBS 构建树，那套流程要下载 GB 级依赖，且必须能访问 GitHub。本项目的目标是「一台已经装了 OBS 的机器上，一条命令就能出 dll」，所以 `build.ps1` 走的是轻量路线：

1. **头文件**：读取已安装的 `obs64.exe` 版本号（如 `31.1.2`），从镜像浅克隆同名的 obs-studio tag（约 64 MB），只取 `libobs/` 与 `frontend/api/` 下的头文件。
2. **导入库**：用 `dumpbin /exports` 解析已安装的 `obs.dll`（1761 个导出符号）和 `obs-frontend-api.dll`（96 个），生成 `.def`，再用 `lib /def` 生成对应的 `.lib`。这样不需要编译整个 libobs。
3. **编译**：`cl /LD /std:c++20 /utf-8 /MD /Zc:__cplusplus`，链接 Qt6Core/Gui/Widgets + obs + obs-frontend-api。
4. 把 `data/locale/*.ini` 按 OBS 的目录约定摆好。

三个容易踩的点：

- **必须 `/MD` 而不是 `/MT`**。Qt 是动态 CRT 编的，而 `QWidget` 的子对象由 Qt 自己 `delete`。用静态 CRT 的话，我们 `new` 出来的控件落在插件自己的堆上、Qt 用另一个堆去 `delete`，直接堆损坏。0.1.x 用 `/MT` 是因为当时只跟 libobs 打交道（全是纯 C 接口，内存都在 obs.dll 内分配释放），加了 Qt 之后这个前提不成立了。
- **必须 `/Zc:__cplusplus`**，否则 Qt 的 `qcompilerdetection.h` 会报「Qt requires a C++17 compiler」——MSVC 默认把 `__cplusplus` 报成 `199711L`。
- **`obsconfig.h` 得自己写**。正常走 OBS 构建系统时它是 CMake 生成的，这里由脚本生成一个最小版本。

**关键前提**：头文件的 tag 必须与已安装 OBS 的版本一致，否则 `obs_module_ver()` 返回的 API 版本对不上，OBS 会拒绝加载。脚本会自动读版本选 tag，不要手工改成别的版本。

### 测试

核心逻辑（状态机 + 时间换算 + 格式化）抽在 `src/countdown-core.h` 里，不依赖 libobs，可以独立编译验证：

```
tests/core_test.cpp     // 10 组用例：格式化边界与四种显示格式、样式版面（数字/卡片/圆环/进度条）、
                        // 剩余占比、开始推进、暂停继续、走完重开、停止保留剩余、隐藏冻结、时长变更
```

`build.ps1` 默认会在编译前跑一遍，失败直接中止（`-SkipTests` 可跳过），产物是 `build\core_test.exe`。

这一层是 0.1.0 踩坑后补的：当时 tick 里把 `end_ns - now` 的**纳秒**直接写进了 `remaining_ms`（毫秒），于是按下开始就显示 `99:59:59`、暂停/停止看起来"没反应"、只有重置（写的是正确单位）能恢复。现在所有 ns↔ms 换算只走 `ns_to_ms` / `ms_to_ns`。

### 已经验证到什么程度

在 OBS 31.1.2 进程内实测过（把 OBS 整个复制到临时目录做便携版，避免往 `C:\Program Files` 写文件）：

- 编译零警告；导出 `obs_module_load` 等 7 个必需符号；`obs_module_ver()` 返回 `0x1F010002`，与已安装 OBS 完全匹配。
- 依赖表：`obs.dll`、`Qt6Core/Gui/Widgets.dll`、`obs-frontend-api.dll`、`MSVCP140/VCRUNTIME140.dll` + UCRT —— 全部由 OBS 或它要求的运行库提供。
- 核心逻辑测试全绿，含"1 秒后剩余必须是毫秒量级"的回归断言。
- 场景内放 3 个倒计时：分别显示 `05:00` / `00:30` / `00:01:00`（第三个开了「始终显示小时」）。
- 控制条列出 3 行，名字正确；点第一行「开始」，只有它变「运行中 04:58」，另外两行仍是「空闲」。
- 给 A 绑 `F1`、B 绑 `F2`，按 `F1` 只有 A 开始走；场景集合里每个实例各存了 4 条独立热键槽位。
- 关掉 OBS 再开，控制条回到上次停靠的位置。
- **v0.3.0 样式**：同一场景放 4 个实例，分别选「纯数字 / 卡片 / 圆环（带 12 刻度）/ 进度条」，四种都正常出图；按 F1–F4 同时开跑后，圆环按 `剩余/设定` 缩短、进度条同步变短，结束的那个实例数字变红、进度清零。
- 四种显示格式在画面上分别验证：`4分49秒`（中文）、`00:00:50`（时:分:秒）、`20`（纯秒数）、`04:50`（自动）。
- 属性面板：状态行显示「空闲 · 剩余 30 · 设定 00:00:30」；「外观样式」切换后只保留该样式用得上的参数（圆环不显示卡片项，反之亦然）；取色器里的色块与实际渲染一致。

---

## 安装

```powershell
# 方式一：脚本安装（管理员 PowerShell）
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Install

# 方式二：手动复制
#   build\obs-countdown.dll   -> <OBS>\obs-plugins\64bit\
#   build\data\obs-plugins\obs-countdown\  -> <OBS>\data\obs-plugins\obs-countdown\
```

装完**重启 OBS**。校验：`来源 → +` 里应出现 **倒计时**；日志（`帮助 → 日志文件 → 查看当前日志`）里能看到 `[obs-countdown] v0.3.0 loaded` 和 `[obs-countdown] 快速控制栏已创建`。

---

## 使用

### 放多个倒计时

1. 场景里添加来源 **倒计时**，画面出现 `05:00`。要几个就加几次。
2. 每个实例在属性面板里单独设：**实例标签**（建议填，比如「主倒计时」「抽奖」）、时长、字体、颜色、对齐。
3. 多个实例可以摆在同一画面的不同位置，各自倒数。

### 换个样子（卡片 / 圆环 / 进度条）

1. 双击来源打开属性面板，找到 **外观样式**，在下拉里选：
   - **纯数字**：只有字，和以前一样；
   - **卡片**：字下面垫一块圆角底，适合压在花哨的画面上；
   - **圆环（钟面）**：字居中，外圈一圈进度环，勾上「显示 12 个钟面刻度」更像表盘；
   - **进度条**：字下面一条横向进度条。
2. 选完面板会自动只显示这个样式用得上的参数（内边距、圆角、粗细、颜色、轨道颜色等），改一下就能实时看到效果。
3. 想要「5分00秒」这种中文写法，或者只显示秒数，改 **显示格式**；想要 `00:05:00` 就选「始终显示小时」。

> 装饰画在数字外侧，所以源的宽高会比纯数字时大一些。如果场景项设了「边界框」，记得把边界改成「无」或按新尺寸重设。

### 绑热键（不同实例绑不同键）

**设置 → 热键**，列表按**源名**分组，每个倒计时各有一组：

```
倒计时A
  开始倒计时
  暂停 / 继续
  停止（保留剩余）
  重置为设定时长
倒计时B
  开始倒计时
  ...
```

给 A 绑 `F1`、给 B 绑 `F2`，两者就完全独立了。热键在 OBS 窗口失焦时同样生效。

> 想改分组里显示的名字，就改**源名**（来源列表里双击）；「实例标签」只影响控制条与属性面板的显示。

### 快速控制条

插件加载后会在主窗口**右侧停靠**一个「倒计时控制」面板，一行一个实例：

```
主倒计时            空闲   05:00
[开始] [暂停] [停止] [重置]
```

- 首次加载自动停靠在右侧，不挡预览画面。
- 想放到别处（比如场景列表上方）直接拖标题栏，位置会记住，下次启动还在那儿。
- 拖丢了或关掉了：`工具 → 倒计时控制` 重新显示。
- 面板每 200ms 从插件读一次状态，所以按钮点击后最多 200ms 内状态就会更新。

> 计时只在源可见时推进：把源的眼睛图标关掉、或者切到别的场景，倒计时会自动冻结；回到画面后从冻结处继续。控制条会显示「运行中 · 已冻结」。

---

## 分发给别人

```powershell
# 编译 + 组装 + 打 zip，产物在 dist\
powershell -ExecutionPolicy Bypass -File .\package.ps1

# 复用已编译的 dll，只重新打包
powershell -ExecutionPolicy Bypass -File .\package.ps1 -SkipBuild
```

产出 `dist\obs-countdown-<版本>-win64.zip`，解压后结构直接对应 OBS 目录：

```
obs-countdown-0.3.0-win64.zip
├── obs-plugins\64bit\obs-countdown.dll
├── data\obs-plugins\obs-countdown\locale\*.ini
├── install.bat      右键「以管理员身份运行」即可安装
├── install.ps1      安装器（自动找 OBS 目录，支持 -Uninstall）
├── INSTALL.txt      给最终用户的说明
└── LICENSE.txt      GPL-2.0 全文
```

对方只需要：解压 → 右键 `install.bat` → 以管理员身份运行 → 重启 OBS。安装器会自动从注册表或常见路径找到 OBS，只复制 dll 和语言包，不改任何配置。**不需要给对方的机器装 Qt**，Qt 的 dll 是 OBS 自带的。

**发出去之前注意两点**：

1. **API 版本兼容**。OBS 加载插件时会校验 `obs_module_ver()`，插件声明的 API 版本高于对方 OBS 版本会被直接拒绝加载。默认编译跟随你本机的 OBS（31.1.2），只能给 31.1.2 及以上的人用。要兼容更早的版本，用更低版本的 libobs 头文件编译：

   ```powershell
   .\build.ps1 -ApiTag 31.0.0     # 编出兼容 31.0.x 及以上的插件
   .\package.ps1                  # 再打包
   ```

   不要反过来：`-ApiTag` 高于对方 OBS 版本，装上也加载不了。

2. **GPL-2.0 义务**。插件链接了 GPLv2 的 libobs，分发二进制时必须同时提供源码获取方式。把仓库地址写进发布说明，或连同源码一起发。

---

## 目录结构

```
CountDown/
├── AGENT.md                 协作约定
├── mvp.md                   功能定义与技术选型（含 v0.2.0 / v0.3.0 追加需求）
├── README.md                本文件
├── build.ps1                编译脚本（SDK 准备、导入库生成、Qt、编译、安装）
├── package.ps1              打包脚本：组装 dist 并压缩成可分发的 zip
├── install.ps1 / install.bat 面向最终用户的安装器（打包时一起分发）
├── INSTALL.txt              给最终用户的安装与使用说明
├── LICENSE.txt              GPL-2.0 许可证全文
├── src/
│   ├── plugin-main.cpp      模块声明、源注册、控制条挂接
│   ├── countdown-source.cpp 源实现（热键、属性面板、渲染、显隐联动）
│   ├── countdown-core.h     纯逻辑：状态机 + ns/ms 换算 + 格式化 + 样式版面（可独立测试）
│   ├── countdown-draw.h     装饰图元：圆角卡片 / 进度环（含刻度）/ 进度条
│   ├── countdown-draw.cpp   ↑ 的 libobs 顶点绘制实现
│   ├── countdown-registry.h 全局实例登记表（UI 线程 ↔ 视频线程的唯一通道）
│   ├── countdown-registry.cpp
│   ├── quick-bar.h          快速控制条（停靠窗口）
│   ├── quick-bar.cpp
│   ├── countdown-source.h
│   ├── plugin.rc            dll 版本资源
│   └── version.h
├── tests/core_test.cpp      核心逻辑测试（不依赖 libobs）
├── data/locale/             UI 文案（en-US / zh-CN）
├── build/                   构建产物（git 忽略）
└── .deps/                   头文件与生成的 SDK（git 忽略）
```

---

## 实现说明

- **计时**：`os_gettime_ns()`（单调时钟）记录 `end_ns`，`video_tick` 里用 `end_ns - now` 求剩余时间，不逐帧累减，所以掉帧、源被隐藏、机器卡顿都不会累积误差。
- **渲染**：内部创建一个 OBS 内置文本源（优先 `text_gdiplus_v3`，回退 `text_ft2_source`）作为私有子源，插件只把格式化好的字符串和样式写进去，字体排版与描边全部复用官方实现。子源配置保存在一份完整的 `obs_data_t` 里整体下发，避免 `obs_source_update` 丢键。
- **多实例**：每个源实例一份 `countdown` 结构体，`std::mutex` 保护状态；跨线程只做「改状态 + 打脏标记」，真正调用 libobs 更新子源只在 `video_tick` 里做。
- **显隐与计时**：`activate`/`deactivate`（是否在节目画面里）和 `show`/`hide`（眼睛图标）共同决定"可见"。一旦不可见就冻结剩余时间；重新可见时用冻结值重建结束时刻，所以隐藏多久都不会"偷跑"。
- **控制条怎么连到实例**：`countdown-registry` 是一张加锁的实例表。视频线程在 `video_tick` 里把「状态 + 剩余 + 设定」写进去，UI 线程每 200ms 读一次快照。UI 侧只持有编号，从不持指针，所以源被删除也不会有野指针。
  - 锁顺序固定为 **registry → 实例锁**，反过来一律不允许。因此 `do_action` 拆成两个版本：给热键/按钮用的是会回报状态的 `do_action`，给登记表回调用的是只改状态的 `do_action_internal`（此时 registry 锁已被持有，再回头去拿它会自锁死）。
- **为什么控制条不声明 `Q_OBJECT`**：它不需要自定义信号槽，全部用函数式 `connect` + lambda 就够了，于是整个项目不需要 moc 这一步，编译链少一环。
- **控制条位置**：OBS 在恢复窗口布局**之后**才发 `FINISHED_LOADING`，而插件只能那时候建 dock，所以 OBS 自己存的那份 `DockState` 永远落不到它头上。插件自己在 `plugin_config\obs-countdown\dock.ini` 里记位置/浮动/停靠区域/可见性，在 `OBS_FRONTEND_EVENT_EXIT` 时保存（不放在 `obs_module_unload`：那时主窗口可能已析构）。
- **装饰怎么画**（v0.3.0）：卡片/圆环/进度条是自己用顶点缓冲画的纯色几何，用内置 `solid.effect` 的 **`SolidColored`** 技法（顶点色 × `color` 参数）。每帧重建一个几百顶点的缓冲，不缓存，代码简单且开销可忽略。三处容易翻车的细节：
  - **退出时必须还原面剔除模式**。OBS 画 2D 精灵默认是 `GS_NEITHER`，如果恢复成 `GS_BACK`，紧接着由文本子源画的字会被整片剔掉——现象是「只剩装饰、数字全没了」。所以 `begin()` 里存 `gs_get_cull_mode()`，`end()` 原样还原。
  - **颜色是 `0xBBGGRR` 不是 `0xRRGGBB`**。OBS 的 `gs_effect_set_color()` 内部走 `vec4_from_bgra`，属性面板的取色器也是按「低字节 = R」解析（`QColor(val & 0xff, (val >> 8) & 0xff, ...)`）。按 RGB 拆字节会让红变蓝、青变橙。
  - **版面在 `countdown-core.h` 里算**，`countdown-draw.cpp` 只负责画。这样「环要多大才包得住文字」这种几何问题可以在 `core_test.cpp` 里断言，不用开 OBS 试。
- **属性面板的语言文件不要放 `%s`**：OBS 的 ini 解析会把 `%` 吃掉，`Prop.Status=%s · 剩余 %s` 取出来只剩前缀。所以状态行的分隔符和数字都写在代码里，ini 只提供「剩余 / 设定」这种单词。

## 已知限制

- 只支持 Windows x64 + OBS 31.x，未做 macOS / Linux。
- 控制条依赖 obs-frontend-api 与 Qt6，只在有前端的环境（正常桌面版 OBS）里有；用 `-NoQuickBar` 编的版本没有控制条，功能退化为「多实例 + 每实例热键」。
- 控制条的位置由插件自己记，不进场景集合。换场景集合不会影响它，但也没法给不同场景集合配不同布局。
- 描边依赖内置文本源能力：GDI+ 文本源完整支持；若回退到 FreeType 源，部分样式键会被忽略。
- 因为「隐藏即冻结」，源一直不在画面上时倒计时不会走完；这是刻意行为，不是 bug。
- 分发时的 API 版本必须不高于对方 OBS 版本，否则会被拒绝加载（见「分发给别人」）。
- 倒计时不做音效、桌面通知、自动切场景、进度环。

## 排错

| 现象 | 处理 |
| --- | --- |
| 来源列表里没有「倒计时」 | 确认 dll 在 `obs-plugins\64bit\`、`data\obs-plugins\obs-countdown\locale\` 存在；看 OBS 日志里的加载报错 |
| 日志提示 API 版本不兼容 | 插件编译用的 API 版本高于你的 OBS。改用 `-ApiTag <你的OBS版本>` 重新编译 |
| 找不到「倒计时控制」面板 | `工具 → 倒计时控制` 可开关；或删掉 `%APPDATA%\obs-studio\plugin_config\obs-countdown\dock.ini` 让它回到默认位置 |
| 控制条里是空的 | 当前场景集合里还没有倒计时源；加一个就有了（空状态会写提示） |
| 热键按了没反应 | 确认绑在了**对应实例**那一组（列表按源名分组）；再看源是否被隐藏、OBS 是否在焦点之外且热键被别的软件占用 |
| 数值不动了 | 检查源是否被隐藏、或所在场景是否不在节目画面——隐藏即冻结是刻意行为 |
| 中文界面显示英文 | 系统语言非中文时 OBS 用 `en-US.ini`；切 OBS 语言到简体中文即可 |
| 选了卡片/圆环/进度条，画面上却只有数字 | 装饰画在数字外侧，源的宽高会跟着变大；若该场景项设过「边界框/裁剪」，把它改成「无」或重新调整大小 |
| 进度环/进度条颜色和属性面板里的色块不一致 | 0.2.x 存的是 RGB 直写的颜色，0.3.0 起统一按 OBS 的 `0xBBGGRR`；在属性面板里重新选一次颜色即可 |
| 圆环的进度长度和剩余时间不匹配 | 进度是每帧按 `剩余 / 设定` 算的，运行中改时长不改变当前这轮的设定值，重置后才对齐 |
| 改了样式没反应 | 样式在下一帧生效；若源不在当前场景，切回去即可 |
| 编译报 `Qt6 development files not found` | 没装 Qt：按「准备 Qt」装一份，用 `-QtDir` 指过去；或加 `-NoQuickBar` 跳过控制条 |
| 编译报 `Qt requires a C++17 compiler` | 缺 `/Zc:__cplusplus`，检查 `build.ps1` 的编译行有没有被改过 |
| 编译报 `config_open: function does not take 1 arguments` | OBS 30.2 起 `config_open` 改成「出参 + 打开方式」的形式，参考 `quick-bar.cpp` 里的 `open_dock_config()` |
| 编译报找不到 `obsconfig.h` | 删掉 `.deps\sdk` 重新运行 `build.ps1`（脚本会自动生成） |
| `install.bat` 提示需要管理员 | 右键 `install.bat` → 以管理员身份运行（要写入 `C:\Program Files`） |
| 脚本报「意外的标记」或中文乱码 | `.ps1` 必须存成 **UTF-8 with BOM + CRLF**，PowerShell 5.1 会把无 BOM 的 UTF-8 当 GBK 解析 |
