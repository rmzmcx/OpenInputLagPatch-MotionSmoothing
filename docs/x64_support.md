# x64 运行时

## 产物与配置

- DLL：`openinputlagpatch_x64.dll`
- 加载器：`oilp_loader_x64.exe`
- 全局配置：`%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch_x64.ini`
- 游戏目录中的 `openinputlagpatch_x64.ini` 可选，存在时覆盖全局配置中的同名项目

加载器会识别当前目录下的受支持游戏，并支持启动后注入或附加到已经运行的游戏。当前 x64
入口通过可执行文件 SHA-256 选择，只接受已登记版本的 `th06nc.exe`。

## 保留功能

### GameFPS

接管游戏主循环的帧限制，替换游戏自带的帧限制调用，按 `GameFPS` 安排逻辑更新节奏。最低值为
60。该设置独立于 Replay 慢放。

### ReplaySpeedControl

Replay 播放期间按住游戏配置的低速移动键时，按 `ReplaySlowRate` 倍率减少实际执行的游戏更新次数；默认
`0.5`。补丁读取游戏自身输入合成结果中的低速移动动作位，因此游戏配置或第三方工具把低速
移动键改成其他按键后仍然有效，也不依赖具体的物理键码。主循环和游戏自身显示的逻辑帧率仍
保持 `GameFPS`。

慢放期间游戏左下角原生 Replay 标识显示对应倍率，例如 `x0.5`。松开低速移动键后按照慢放期间
实际执行的逻辑帧数重新定位 BGM，并保留游戏的循环点处理。游戏自身的 Replay 加速逻辑继续
优先；加速状态不会触发补丁减速。

Replay 加速倍率由 `ReplayFastRate` 设置，默认 `8`，允许 `1` 到 `128`。按住游戏自身配置的
射击键时，游戏原生加速循环会按该倍率执行；左下角 Replay 标识同步显示 `xN`。`1` 会关闭
额外更新，保持普通 Replay 播放速度。

### ShowOverlay

通过 D3D11 Present 在游戏 BackBuffer 上绘制，不创建额外覆盖窗口，因此不会让游戏的现代 flip
呈现降级。

- `0`：关闭
- `1`：显示实际 Present 帧率
- `2`：显示帧处理耗时
- `3`：同时显示两项

原补帧帧率读数暂时保留，但当前 x64 构建没有补帧，因此该行显示实际 Present 次数，等同于
游戏当前实际呈现帧率。

### DebugConsole

`DebugConsole = 1` 时分配控制台窗口，输出版本识别、Hook 安装和 D3D11 交换链信息。

### ForceFlipDiscard

在游戏创建 DXGI 交换链前修改描述，优先请求 `DXGI_SWAP_EFFECT_FLIP_DISCARD`。创建失败时回退
到游戏原描述。只有启动前注入才能改变交换链创建参数；附加到已运行游戏时不会重建现有交换链。

### U 键诊断

游戏内按一次 `U` 会在游戏目录写入 `oilp_diag.txt`。日志包含：

- 模块基址、进程 ID、当前 `GameFPS` 和显示刷新率。
- `ReplaySpeedControl`、慢放倍率、overlay、控制台和 Flip 配置。
- 交换链是否为 `FLIP_DISCARD`，以及各 Hook 的安装状态。
- 游戏模式、逻辑输入、实时输入动作位、低速/射击动作、Replay 控制器、慢放状态、
  BGM 回拨位置和自机位置。
- 最近 300 帧的模式、逻辑更新执行情况、慢放状态和 Present 时间。

## 已移除的补帧路径

x64 构建不再包含对象外推、额外帧、额外 Present、绘制链重放、捕获屏历史保护、D3D11 资源
追踪或补帧专用配置。x64 ini 不读取 `[Interpolation]`，共享 limiter 中的补帧入口也只供
原 x86 构建使用。
