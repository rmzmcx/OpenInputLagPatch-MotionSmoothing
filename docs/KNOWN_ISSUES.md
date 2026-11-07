# 已知未解决问题 / Known unresolved issues

这里记录已经确认但还没有修复的问题。已经修复的内容看
`CHANGELOG/CHANGELOG.en.md` / `CHANGELOG/CHANGELOG.zh-CN.md` / `CHANGELOG/CHANGELOG.ja.md`。

This file records confirmed bugs that are still open. Fixed changes are listed in
`CHANGELOG/CHANGELOG.en.md` / `CHANGELOG/CHANGELOG.zh-CN.md` / `CHANGELOG/CHANGELOG.ja.md`.

## 高倍率补帧长时间游玩后补帧下滑 / Presentation rate drops at high interpolation targets

**状态 / Status:** 未解决，等待现场日志 / Open, waiting for a reproduction log.

**现象 / Symptoms**

- th17 补帧目标设为 300fps（显示器也是 300Hz）时，游玩一段时间后补帧显著下滑，最低到 180fps 左右；
  调到 240fps 左右没有这个问题。
- th16 进入五道中前半时也会出现掉帧。
- th15 游玩时逻辑帧掉帧的概率变高。

At a 300fps interpolation target (on a 300Hz display), th17's presentation rate drops after
playing for a while, down to about 180fps; a 240fps target does not show it. th16 also drops
frames when stage 5's mid-stage starts, and th15 drops logic frames more often.

**已知 / What is known**

- 游戏逻辑、随机数和 replay 不受影响；问题在呈现频率/CPU 负载。
- 补帧路径里没有找到会随游玩时间无限增长的分配或容器。
- 当前的时间预算用“游戏自己那趟的耗时”估算额外趟，但额外趟还要重跑渲染链、外推子弹/自机/激光并画
  overlay，成本更高；目标 300fps 时每游戏帧有 4 趟额外呈现，单趟成本接近 3.3ms 的间距时就会连续
  触发截止判断，呈现率掉到 3 次/帧左右。

The game logic, randomness and replays are unaffected; this is about presentation rate and CPU
load. No unbounded allocation or container was found in the interpolation path. The frame budget
uses the game's own pass time as the estimate for an extra pass, but an extra pass also re-runs the
render chain, extrapolates bullets/player/lasers and draws the overlay. At 300fps there are four
extra presentations per game frame; when one approaches the 3.3ms spacing, the deadline check skips
presentations and the rate falls to about three per frame.

**需要 / Needed**

- th17 在 300fps 目标下，下滑前后各按一次 U 导出的 `oilp_diag.txt`。
- th16 五道中前半掉帧时的 `oilp_diag.txt`。
- th15 出现逻辑掉帧时的 `oilp_diag.txt`。
- 对应的显示器刷新率、CPU 和 `Interpolation` 设置。

`oilp_diag.txt` before and after the drop in th17 at 300fps, from th16 when stage 5's mid-stage
drops, and from th15 when the logic frame drops, together with the display refresh rate, CPU and
`Interpolation` setting.

**临时办法 / Workaround:** 把补帧目标降到 240fps 左右（反馈者在这个值下没有复现）。 / Lower
the interpolation target to about 240fps (the reporter does not see it at that value).
