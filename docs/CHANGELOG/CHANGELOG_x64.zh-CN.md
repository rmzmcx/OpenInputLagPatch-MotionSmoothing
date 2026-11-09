# x64 更新日志

[English](CHANGELOG_x64.en.md) | 简体中文 | [日本語](CHANGELOG_x64.ja.md)

本文件记录独立 x64 分支的功能与变更，与 x86 作品的更新日志分开维护。

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
