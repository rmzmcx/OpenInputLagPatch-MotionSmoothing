# 更新日志

[English](CHANGELOG.md) | 简体中文

本文件记录相对上游 [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch) 的改动。

只记录已经在游戏里实测确认可用的改动，因此本文件同时也是「哪些修改真正被验证过」的凭证。

## 2026-10-05

### 新增

- **窗口模式：与 vpatch 相同的 `[Window]` 配置，并新增开机询问是否全屏**（`26b310a`）

  `openinputlagpatch.ini` 现在带有 vpatch 的 `[Window]` 段：`AskWindowMode`、`enabled`、`X`、`Y`、`Width`、`Height`、`TitleBar`、`AlwaysOnTop`，含义与 `vpatch.ini` 相同。游戏处于窗口模式时会按这些设置移动与缩放窗口，并应用标题栏、窗口置顶。

  开启 `AskWindowMode` 后，th06 到 th09.5 会在创建窗口之前询问是否以全屏启动，不必再去改游戏自己的显示设置。答案会写进游戏自己的「窗口模式」标志位——和 vpatch 的做法一样——而不是只强制 D3D 呈现参数：dgVoodoo2 配置成把独占全屏转成无边框窗口时（`AppControlledScreenMode = false`、`FullscreenAttributes = fake`）会忽略该请求，游戏自身设置仍是窗口模式的话，行为也依旧是窗口模式。

  询问会在游戏创建单实例互斥体之后立刻弹出，这是 thprac 等工具仍能检测到游戏的最早时机；之后用游戏内菜单切换模式也不会被强制回去。只有 th06 到 th09.5 会询问，因为之后的作品自己就有该选项。

## 2026-10-04

### 修复

- **使用 dgVoodoo2 时误报「couldn't hook Direct3DCreate9」警告**（`d20a521`）

  dgVoodoo2 是把 D3D8 翻译到 D3D11 的，它的 `d3d8.dll` 里没有 `Direct3DCreate9` 可以钩，所以即使游戏运行完全正常，补丁仍会弹出该警告。现在改为读取已加载的 `d3d8.dll` 的版本资源来精确识别（`ProductName` 含 `dgVoodoo`，会遍历所有语言/代码页块），只在这种情况下跳过警告。未知的 wrapper 仍会像以前一样警告，d3d8to9 仍走正常的钩子路径。

- **红魔乡 + dgVoodoo2：加速时帧数无法超过屏幕刷新率**（`667fe5e`）

  dgVoodoo2 会把游戏的 D3D8 呈现转换成 D3D11 的 flip 模型交换链，该交换链在队列满时会阻塞到下一次垂直回扫。由于游戏逻辑每帧只提交一次，游戏速度就被钉在屏幕刷新率上，导致 replay 快进与 thprac 加速都无法更快。现在改为钩住 D3D8 的 Present（虚表下标 15），当目标帧率高于刷新率时按 `N = ceil(目标帧率 / 刷新率)` 抽帧提交；正常帧率下 N 为 1，每次调用都原样转发。

  目前只对红魔乡安装该钩子，因为这个问题看起来是该作特有的——之后的作品不受影响，D3D9 作品走的是另一条呈现路径。

- **新增 `AlwaysBlt` 选项：窗口未激活时继续渲染**（`12fe031`、`c41c53d`、`ea884df`）

  切出窗口再切回来时画面会定格，原因有两处：一是游戏自身在失去焦点后就不再渲染（`GameWindow::Render` 在窗口未激活时直接返回）；二是 oilp 的窗口更新钩子在失焦时直接返回、不调用游戏原本的窗口更新——而那里同时也是驱动渲染的地方，于是画面完全不再绘制。

  现在 AlwaysBlt 会绕开那个早退判断（把 th06 的 `0x004206F0`、th07 的 `0x004346F0` 处的条件跳转改成无条件跳转），并让窗口更新在失焦时继续执行，行为与 vpatch 的 AlwaysBlt 选项一致。该选项默认开启，可通过 `openinputlagpatch.ini` 里的 `AlwaysBlt` 关闭；仓库现在也附带了一份参考配置文件。

  仅对 th06 与 th07 实现。
