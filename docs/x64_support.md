# x64 运行时

## 产物与配置

- DLL：`openinputlagpatch_x64.dll`
- 加载器：`oilp_loader_x64.exe`
- 全局配置：`%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch_x64.ini`
- 游戏目录中的 `openinputlagpatch_x64.ini` 可选，存在时覆盖全局配置中的同名项目

加载器会识别当前目录下的受支持游戏，并支持启动后注入或附加到已经运行的游戏。当前 x64
入口通过可执行文件 SHA-256 选择，只接受已登记版本的 `th06nc.exe`。

## 功能

### GameFPS

接管游戏主循环的帧限制，替换游戏自带的帧限制调用，按 `GameFPS` 安排逻辑更新节奏。最低值为
60。该设置独立于 Replay 慢放。检测到官方 thprac 时，limiter 改为跟随 thprac 的 GameFPS
设置，两套界面使用同一个目标帧率。

### ReplaySpeedControl

Replay 播放期间按住游戏配置的低速移动键时，按 `ReplaySlowRate` 倍率减少实际执行的游戏更新次数；默认
`0.5`。补丁读取游戏自身输入合成结果中的低速移动动作位，因此游戏配置或第三方工具把低速
移动键改成其他按键后仍然有效，也不依赖具体的物理键码。主循环和游戏自身显示的逻辑帧率仍
保持 `GameFPS`。

未加载 thprac 时，慢放期间游戏左下角原生 Replay 标识显示对应倍率，例如 `x0.5`。松开低速
移动键后按照慢放期间实际执行的逻辑帧数重新定位 BGM，并保留游戏的循环点处理。游戏自身的
Replay 加速逻辑继续优先；加速状态不会触发补丁减速。

Replay 加速倍率由 `ReplayFastRate` 设置，默认 `8`，允许 `1` 到 `128`。按住游戏自身配置的
射击键时，游戏原生加速循环会按该倍率执行；左下角 Replay 标识同步显示 `xN`。`1` 会关闭
额外更新，保持普通 Replay 播放速度。

官方 thprac 同时加载时，Replay 加减速、快速倍率和原生 `xN` 显示由 thprac 管理。补丁仍从
游戏实时输入合成结果读取改键后的低速移动动作，并映射到 thprac 使用的低速状态，因此仍可在
改键后触发 thprac 的白色 `/N` 减速显示。BGM 回拨按 Update Chain 实际执行的逻辑帧数计算，
不依赖 thprac 的固定按键或内部倍率。

### ShowOverlay

通过 D3D11 Present 在游戏 BackBuffer 上绘制，不创建额外覆盖窗口，因此不会让游戏的现代 flip
呈现降级。游戏窗口失焦后，overlay 会跟随游戏继续刷新和绘制。

- `0`：关闭
- `1`：显示实际 Present 帧率
- `2`：显示帧处理耗时
- `3`：同时显示两项

该行统计真实成功的 Present 次数，包括补帧产生的额外呈现。

### Interpolation

`[Interpolation]` 使用与 x86 版一致的格式：

- `0`：关闭
- `1`：按显示器刷新率
- `-1`：显示器刷新率内游戏帧率的整数倍
- `N`：每游戏逻辑帧呈现 N 次
- `*R`：固定每秒呈现 R 次

补帧保持原始 Update Chain 频率，只重复游戏的 Render Chain、最终合成和 Present。每个额外帧
前后都会恢复游戏对象与绘制状态；额外渲染期间不会执行 Update Chain。

当前覆盖：

- 状态 1 的普通敌弹。
- 自机本体与两个 Option。
- 自机弹。
- 直线激光。

敌弹使用当前速度与状态系数前移。自机使用当前输入和角色速度表。Option、自机弹和直线激光
使用相邻逻辑帧实测位移，并按对象跳变阈值过滤槽位复用。直线激光角度按最短弧处理。

时间停止、暂停或对象更新被阻断时，敌弹和激光不会在额外帧中前移。游戏自身的 FPS 统计也会
在额外最终合成前后恢复，因此原生 FPS 文本仍表示真实逻辑/主循环帧率。

对象开关：

- `EnemyBullets`
- `Lasers`
- `PlayerBody`
- `PlayerShots`

默认均为 `1`。

### DebugConsole

`DebugConsole = 1` 时分配控制台窗口，输出版本识别、Hook 安装和 D3D11 交换链信息。

### ForceFlipDiscard

在游戏创建 DXGI 交换链前修改描述，优先请求 `DXGI_SWAP_EFFECT_FLIP_DISCARD`。创建失败时回退
到游戏原描述。只有启动前注入才能改变交换链创建参数；附加到已运行游戏时不会重建现有交换链。

### AllowTearing

`ForceFlipDiscard = 1` 时请求 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`。支持时 Present 会带
`DXGI_PRESENT_ALLOW_TEARING`，允许提交率超过显示器刷新率，可能改善延迟或撕裂表现。创建失败
时依次回退普通 `FLIP_DISCARD` 和游戏原描述。

显示器每秒实际扫描出的唯一画面数仍受刷新率限制。默认 `1`。

### U 键诊断

游戏内按一次 `U` 会在游戏目录写入 `oilp_diag.txt`。日志包含：

- 模块基址、进程 ID、当前 `GameFPS` 和显示刷新率。
- `ReplaySpeedControl`、慢放倍率、overlay、控制台和 Flip 配置。
- 交换链是否为 `FLIP_DISCARD`，以及各 Hook 的安装状态。
- 官方 thprac 是否加载、兼容层是否启用、外部 GameFPS 和实际 Replay 快速倍率。
- 游戏模式、逻辑输入、低速/射击动作、Replay、慢放、时停/暂停阻断和自机位置。
- 独立 Update Chain 总数、实际逻辑 FPS，以及额外 Render/Compose 期间是否发生 Update。
- 原生 FPS 文本与统计状态。
- 敌弹、激光、自机和自机弹的投影数量，以及直线激光角度跳变。
- 最近 300 帧的模式、逻辑更新、补帧对象数量和 Present 时间。

## 当前范围外

敌机本体和曲线激光尚未纳入补帧。自机弹发射前的精确空槽 Hook 尚未定位，因此同帧槽位复用
主要依靠状态、弹型、逻辑位置和绘制位置跳变守卫。
