# 协作规则（AGENTS.md）

本文件记录在这个工作区 / 仓库里协作时必须遵守的约定，供以后的会话（包括其它 AI）读取。
（Windows 文件名不区分大小写，写 `agent.md` 读到的也是本文件；这里用 `AGENTS.md` 是因为
这类工具默认会自动查找这个名字。）
书面写作规则：

When you are writing, please

- be brief
- reduce concluding sentences
- reduce parallel words
- reduce semicolons: adversative and progressive sentences should use correct and various conjunctions
- reduce negatively emphasising semantics like "..., not ..." / "..., rather than ..." / "..., without ..."
- stop reasserting a certain rule
- reduce abuse of semantics like "retain" / "remain" / "keep"
- reduce abuse of identical sentence structures
- reduce bold or highlighting
- stop using non-UTF8 symbols
- prevent a mechanical feel
- be nature and more human-like

---

代码工作与问题解决规则：

- 回答要简洁且突出重点，步骤要明晰，不要重复总结。
- 遇到不确定的情况时积极联网查找信息，不要猜测。
- 修改代码时严格检查之前对同一个任务做过的所有工作，从整体上优化算法，能合并的就合并，不要把功能堆成屎山。

---

文档清洁规则：

- 不要把不相关的内容加到各点下。
- 不要重复同一内容以强调。
- 不要把提示词和你的思考过程作为规则加入文档内强调。
- 不要把文档和代码堆成屎山。
- 每次文档生成或修改完毕后按此清洁规则做文档清洁，以减少屎山的堆积。

---

代码清洁规则：

- 更新算法时，旧框架应当 prune。导致的无用代码可删除，不需要保留。
- 旧方案除无法迁移到新方案的部分，应当全部优化。后续若新方案的功能完成，框架应当被 prune。
- 同类文件过多时，应当合并同类项，减少复杂的嵌套调用。
- 每次代码生成或修改完毕后按此清洁规则做代码清洁，以减少屎山的堆积。

## 工作区与仓库

- 工作区：`E:\东方MotionSmoothing相关\`
- 主仓库（oilp = 上游 [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch) 的 fork）：
  `E:\东方MotionSmoothing相关\Github\OpenInputLagPatch-MotionSmoothing\`
- **只做本地 commit。** push、打 tag、发 GitHub release 都由用户本人完成，不要推送、不要动远端。
- 需求原文：仓库根目录的 `interpolation.md`，**只读、禁止修改**；补帧的实现进展写进 commit 信息。
- 逆向笔记：`E:\东方MotionSmoothing相关\_analysis\th15\th15_interp_notes.md`（不在 git 仓库里），
  逆向结论继续往这里追加。
- thprac 参考仓库：`E:\东方MotionSmoothing相关\Github\rueee-thprac`，只用来参考 / 查常量，**不要修改**。

## 目录与发布规则（用户明确要求）

1. `Release/` 只放发布相关的东西：`openinputlagpatch.dll`、`oilp_loader.exe`、发布 zip，
   以及各版本的文档与打 zip 用的暂存目录：
   - `RELEASE_NOTES_v<ver>.md`、`UPDATES&USAGES_v<ver>.md`（见第 3、4 条）
   - `dist/`：打 zip 用的暂存目录，zip 里就是它里面的那几个文件
   - `OpenInputLagPatch-<变体>_v<ver>_<日期>_<commit>.zip`，例如
     `OpenInputLagPatch-NoMS_v1.6_2026-10-08_679e118.zip`（`NoMS` = 那一版还不含补帧，
     v1.4–v1.6 都是；补帧版的变体名要和用户确认，暂定 `MS`）
   - tag 由用户自己打，历史上是 `v1.4`、`v1.5`、`V1.6`（大小写不一致）
   `.gitignore` 已经忽略 `Release/` 与 `build/`——**不要动 `.gitignore`**。
2. `build/` 放所有中间文件（obj / tlog / iobj）与测试构建；编译器输出到这里，不要污染 `Release/`。
   **发布 zip 里只有三个文件，平铺、不要多套一层目录**：`openinputlagpatch.dll`、
   `oilp_loader.exe`、`UPDATES&USAGES_v<ver>.md`。
   `RELEASE_NOTES_v<ver>.md` **不放进去**（它只作为 release 正文，见第 3 条）。
   ini 从仓库根目录复制（`dist/` 里那份可能是旧的）。
3. `RELEASE_NOTES_v<ver>.md`：写「v<ver> 改动 / 安装 / 配置 / 校验」，作为 GitHub release 的正文，
   **不进压缩包**。
4. `UPDATES&USAGES_v<ver>.md`：就是 `RELEASE_NOTES` 去掉「校验 / Checksums」那一节，**进压缩包**。
   两份都是三段 `<details>`（依次为简体中文、日本语、English，summary 写
   `简体中文 / Simplified Chinese`、`日本語 / Japanese` 与 `English / 英文`），标题形如
   `# OpenInputLagPatch - Motion Smoothing v1.7（简体中文）` /
   `# OpenInputLagPatch - Motion Smoothing v1.7（日本語）` /
   `# OpenInputLagPatch - Motion Smoothing v1.7`，正文小节的写法照
   `Release/RELEASE_NOTES_v1.6.md`、`Release/UPDATES&USAGES_v1.6.md` 抄。
5. commit 用**长说明**，写清「为什么」，不是 changelog。
   **每次修改后先等用户确认，确认之后再提交本地 commit。**
6. `CHANGELOG.md` / `CHANGELOG.zh-CN.md`（中英两版）：一般等用户确认功能没问题之后才写。
   只记录已经在游戏里实测确认的改动，标题用 `- **一句话**（`commit`）`，正文分段讲「为什么」，
   结尾要写清还没做的部分。

## 构建 / 部署 / 实测流程

- 构建（在仓库根目录）：
  `& 'E:\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' openinputlagpatch.sln /m /p:Configuration=Release /v:minimal /nologo`
  这条命令在沙箱里会因 MSBuild 的 FileTracker 需要越权而失败，要用 `require_escalated` 跑。
- 产物在 `build/`：`openinputlagpatch.dll`、`oilp_loader.exe`（以及 pdb / lib / exp）。
- 改完代码要**部署到游戏目录**再让用户实测。th15 的目录：
  `E:\东方MotionSmoothing相关\原版游戏\[th15] 东方绀珠传 (汉化版+日文版)\`
  （游戏正在运行时复制会失败，需先让用户关掉游戏。）
- 用户实测后按 `U` 会在游戏目录导出 `oilp_diag.txt`；先看这个文件再下结论，不要凭猜测下判断。
- 游戏版本：th15 日文原版 v1.00b，SHA256
  `67A642357C8777089F468AAB9C7A0AE346EBDB62849D842A7B7B18D1E6910364`。

## 逆向工具

- 优先使用 IDA-MCP 工作流，并使用Ida-Sequential技能来操控IDA。
- 如果 IDA-MCP 不可用，先说明阻塞点并询问我，不要静默改用别的数据库。
- 只有在现有结论已写在文档里时，才可直接引用；任何新逆向都需要先询问并得到我的确认。
- 辅助工具仍可用 dumpbin /DISASM；按助记符文本搜索，MSVC 可能把“基址+静态偏移”折成绝对地址。
- Ghidra 路径(次要选择，IDA无法使用再考虑)：
  E:\东方MotionSmoothing相关\ghidra_12.1.4_PUBLIC
  中文路径会让 log4j 失败，需要 %TEMP% 下 ASCII junction；脚本在
  _analysis\ghidra_scripts\Q.java。
