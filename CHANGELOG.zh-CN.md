# 更新日志

[English](CHANGELOG.md) | 简体中文

本文件记录相对上游 [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch) 的改动。

只记录已经在游戏里实测确认可用的改动，因此本文件同时也是「哪些修改真正被验证过」的凭证。

## 2026-10-04

### 修复

- **使用 dgVoodoo2 时误报「couldn't hook Direct3DCreate9」警告**（`d20a521`）

  dgVoodoo2 是把 D3D8 翻译到 D3D11 的，它的 `d3d8.dll` 里没有 `Direct3DCreate9` 可以钩，所以即使游戏运行完全正常，补丁仍会弹出该警告。现在改为读取已加载的 `d3d8.dll` 的版本资源来精确识别（`ProductName` 含 `dgVoodoo`，会遍历所有语言/代码页块），只在这种情况下跳过警告。未知的 wrapper 仍会像以前一样警告，d3d8to9 仍走正常的钩子路径。

- **红魔乡 + dgVoodoo2：加速时帧数无法超过屏幕刷新率**（`667fe5e`）

  dgVoodoo2 会把游戏的 D3D8 呈现转换成 D3D11 的 flip 模型交换链，该交换链在队列满时会阻塞到下一次垂直回扫。由于游戏逻辑每帧只提交一次，游戏速度就被钉在屏幕刷新率上，导致 replay 快进与 thprac 加速都无法更快。现在改为钩住 D3D8 的 Present（虚表下标 15），当目标帧率高于刷新率时按 `N = ceil(目标帧率 / 刷新率)` 抽帧提交；正常帧率下 N 为 1，每次调用都原样转发。

  目前只对红魔乡安装该钩子，因为这个问题看起来是该作特有的——之后的作品不受影响，D3D9 作品走的是另一条呈现路径。
