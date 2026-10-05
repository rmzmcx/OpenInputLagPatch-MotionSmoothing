# 更新日志

[English](CHANGELOG.md) | 简体中文

本文件记录相对上游 [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch) 的改动。

只记录已经在游戏里实测确认可用的改动，因此本文件同时也是「哪些修改真正被验证过」的凭证。

## 2026-10-06

### 新增

- **不走 D3D9 的 dx8 游戏现在也有 overlay 了**（`d0cc56a`、`ed756d7`）

  原来 overlay 只有 D3D9 版：它在 D3D9 的 CreateDevice 钩子里创建、在 EndScene 钩子里绘制，所以 d3d8to9 与 DXVK（把 D3D8 翻成 D3D9）能用，而 dgVoodoo2（把 D3D8 直接翻成 D3D11）和不装 wrapper 的场合都没有。现在多了一份用游戏自身 D3D8 设备绘制的 overlay，且只在"没有 D3D9 设备"时启用，所以走 D3D9 的 wrapper 仍沿用原来那份。

- **`FullscreenRefreshRate` 对 dx8 游戏也生效**（`5d1d73f`）

  这个选项以前只存在于 D3D9 钩子里，对 dx8 作品完全没作用：它们用 IDirect3D8 建呈现参数，而游戏请求的是"当前桌面刷新率"，所以三个取值看起来都等于强行用最大刷新率。现在 D3D8 的 CreateDevice/Reset 钩子用同样的逻辑选择刷新率；如果该分辨率下没有合适的模式，则让运行时决定，而不是报错退出。

- **dx8 游戏同样强制关闭垂直同步**（`1dd1bb2`）

  设置 `D3DPRESENT_INTERVAL_IMMEDIATE`（以及 `D3DSWAPEFFECT_DISCARD`、后台缓冲数 0）以前只在 D3D9 钩子里做，所以在 dgVoodoo2 或不装 wrapper 的情况下，游戏仍按自己请求的呈现间隔显示，replay 快进也被刷新率卡住。现在 D3D8 钩子也做同样的事（仅全屏——D3D8 要求窗口模式下那两个 FullScreen_\* 字段必须保持为 0）。

### 修复

- **独占全屏的 dx8 游戏切出去再切回来会直接退出**（`d0cc56a`）

  游戏会先释放自己的 `D3DPOOL_DEFAULT` 资源、再调用 `Reset`，失败就直接退出主循环。而 D3D8 的 Reset 只要还有 DEFAULT 池资源存活就会失败，overlay 自己的资源却是在 Reset 之后才释放的，于是全屏 dx8 游戏一旦重新获得焦点就退出了。

- **dx8 游戏暂停时 overlay 会跑到暂停画面的角落**（`ed756d7`）

  状态块并不携带 viewport，overlay 绘制时用的是游戏当前设置的 viewport，而妖妖梦的暂停画面会把画面绘制到更小的一块区域。现在它每次绘制前都会重设自己的 viewport 与变换——和 thprac 的 overlay 做法一致。

### 变更

- **「无法钩住 Direct3DCreate9」的提示改为控制台输出，并优先推荐 dgVoodoo2**（`d4296fc`、`5df484c`）

  在 overlay 和其它 dx8 设置都不再依赖 D3D9 之后，不经过 D3D9 的 dx8 游戏其实只少了 D3D9Ex 那条降延迟链路，值得在调试控制台留一行，但不必每次启动都弹框打断。推荐顺序改为 dgVoodoo2 优先，因为它的 flip 模型呈现在社区实测里延迟最低。

- **参考配置改为全部使用默认值**（`dee2a1e`）

  `openinputlagpatch.ini` 里放的是测试时用的值（`BltPrepareTime` 0、`Sleep` 0、窗口模式那几项等），和它自己注释里写的默认值对不上，对照起来很困惑。现在 22 个键全部等于默认值，文件只起"展示选项"的作用，不会再自带开启任何东西；README 也一并改成优先推荐 dgVoodoo2。

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
