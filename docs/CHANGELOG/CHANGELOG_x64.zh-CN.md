# x64 更新日志

[English](CHANGELOG_x64.en.md) | 简体中文 | [日本語](CHANGELOG_x64.ja.md)

本文件记录独立 x64 分支的功能与变更，与 x86 作品的更新日志分开维护。

## 2026-11-12

### 新增

- **《东方红魔乡：新典》x64 运行时支持对象级补帧**（`37a6ad9`）

  `th06nc.exe` 的 x64 运行时现在保持原始 Update Chain 频率，只重复游戏的 Render Chain、
  最终合成和 Present。额外帧复用游戏自身的 D3D11 绘制路径和现有 Present/overlay Hook，
  前后恢复对象及绘制状态；额外 Render/Compose 期间不会执行 Update Chain。

  当前覆盖状态 1 的普通敌弹、自机本体与两个 Option、自机弹和直线激光。敌弹使用当前速度与
  状态系数前移；自机使用当前输入和角色速度表；Option、自机弹及激光使用相邻逻辑帧实测位移，
  并按对象类型、逻辑位置和绘制位置跳变过滤槽位复用。直线激光的角度按最短弧处理。

  时间停止、暂停或对象更新被阻断时，敌弹和激光不会在额外帧中前移。额外最终合成前后会保存
  并恢复游戏自身的 FPS 统计，因此原生 FPS 文本仍显示真实逻辑/主循环帧率。实测日志中主循环
  约 60fps，额外 Render/Compose 期间 Update 次数为 0。

  `[Interpolation]` 支持 x86 版的 `0`、`1`、`-1`、`N`、`*R` 格式，并新增
  `EnemyBullets`、`Lasers`、`PlayerBody`、`PlayerShots` 对象开关。D3D11 交换链可选启用
  `ALLOW_TEARING`，超刷新率提交可影响延迟或撕裂表现。官方 thprac 的 UI 重放状态通过
  `thprac64.dll` 版本表发现；版本不匹配时关闭 UI 重放。

  敌机本体和曲线激光尚未纳入补帧。自机弹发射前的精确空槽 Hook 尚未定位，目前依靠对象状态、
  弹型、逻辑位置和绘制位置守卫处理槽位复用。

## 2026-11-11

### 新增

- **与官方 thprac 同步 GameFPS、Replay 速度和倍率显示**（`e77dfcd`）

  thprac 会接管 th06nc 的 Update 调用和 Replay 加减速。现在检测到 `thprac64.dll` 后，
  GameFPS 跟随 thprac 的帧限制设置，Replay 快速倍率和原生 `xN` 显示交还 thprac；先前固定
  显示 `x8` 的问题已消失。补丁仍读取改键后的低速移动动作，用该动作触发 thprac 的白色 `/N`
  慢放显示，并在 Update Chain 入口统计实际执行的逻辑帧，以便松开低速键后重新定位 BGM。

  已使用官方 thprac 实测：非默认快放倍率显示正确，改键减速和 BGM 回拨有效，两套 overlay
  正常共存，游戏保持 60 FPS。未加载 thprac 时仍使用原有的 `ReplaySlowRate`、
  `ReplayFastRate` 和 `GameFPS` 路径。

## 2026-11-10

### 修复

- **窗口失焦时 overlay 不再消失**（`284d165`）

  游戏窗口失焦后仍会继续渲染和 Present，但 overlay 之前会依据前台窗口检查直接关闭，
  因而切到其他窗口时就消失。现在 overlay 会继续刷新和绘制，Present FPS 与帧处理耗时
  在后台也会更新；`ShowOverlay = 0` 仍会正常关闭 overlay。

## 2026-11-09

### 新增

- **建立独立的 x64 DLL、加载器和配置体系**（`3f2f217`）

  新增 `openinputlagpatch_x64.dll`、`oilp_loader_x64.exe`、版本 SHA-256 识别和全局配置
  `%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch_x64.ini`。加载器支持启动游戏后注入，
  也支持附加到已经运行的受支持进程。

- **x64 运行时保留 GameFPS、Replay 慢放、overlay、控制台和 Flip 模型功能**（`3f2f217`）

  `GameFPS` 接管游戏主循环的帧限制。Replay 播放期间按住左 Shift 会按 `ReplaySlowRate`
  慢放，原生 Replay 标识显示对应倍率，松开后重新对齐 BGM。`ShowOverlay` 在游戏 BackBuffer
  上显示实际 Present 帧率和帧处理耗时，不创建会导致 flip 呈现降级的额外窗口。
  `DebugConsole` 打开调试控制台，`ForceFlipDiscard` 在交换链创建前请求
  `DXGI_SWAP_EFFECT_FLIP_DISCARD`。

- **保留游戏内按 U 导出诊断**（`3f2f217`）

  `oilp_diag.txt` 记录 x64 版本、配置、Hook 状态、交换链模型、Replay 慢放、BGM 回拨、
  自机位置以及最近 300 个游戏帧的运行信息。

- **Replay 减速改为读取游戏自身的低速移动动作**（`2a41c58`）

  补丁不再轮询物理左 Shift，而是在游戏把回放输入写入逻辑状态前保存本帧的实时输入合成结果，
  再用低速移动动作位触发慢放。游戏内改键、多套键位或第三方工具修改低速移动键后仍然有效。

- **新增可配置的 Replay 加速倍率 `ReplayFastRate`**（`2a41c58`）

  默认 `8`，与游戏原版一致；允许 `1` 到 `128`。按住游戏配置的射击键时，原生 Replay 加速
  循环按该倍率执行，左下角 Replay 标识同步显示 `xN`。

### 变更

- **暂时保留 overlay 的原补帧帧率行**（`3f2f217`）

  当前 x64 构建没有补帧，因此该行显示实际 Present FPS，而不是补帧目标帧率。

### 修复

- **修复非默认 `ReplayFastRate` 下启动时高概率崩溃**（`2a41c58`）

  WER 中的旧记录确认同一崩溃也存在于加入倍率功能之前，位置是 overlay 释放 D3D11 COM 对象
  的路径。overlay 资源生命周期现在由独占锁串行化，释放前先清空指针，并且不再在
  `DllMain(PROCESS_DETACH)` 的加载器锁内释放 D3D 对象。以非 8 倍率反复启动已确认正常。

### 移除

- **移除 th06nc x64 补帧及 Render Replay 实现**（`3f2f217`）

  删除对象外推、额外帧、额外 Present、绘制链重放、捕获屏历史保护、D3D11 资源追踪和
  x64 补帧配置。x64 ini 不再读取 `[Interpolation]`。共享 limiter 中保留的补帧入口只供
  原 x86 构建使用，x86 补帧功能不受影响。
