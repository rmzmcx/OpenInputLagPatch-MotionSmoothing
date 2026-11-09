# x64 更新履歴

[簡体字中国語](CHANGELOG_x64.zh-CN.md) | [English](CHANGELOG_x64.en.md) | 日本語

本ファイルは独立した x64 ブランチの機能と変更を記録します。x86 作品の更新履歴とは分けて
管理します。

## 2026-11-09

### 追加

- **独立した x64 DLL、ローダー、設定システムを追加**（`3f2f217`）

  `openinputlagpatch_x64.dll`、`oilp_loader_x64.exe`、SHA-256 によるバージョン判定、および
  グローバル設定 `%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch_x64.ini` を追加しました。
  ローダーはゲーム起動後のインジェクションと、実行中の対応プロセスへのアタッチの両方に対応します。

- **x64 ランタイムで GameFPS、Replay 低速再生、overlay、コンソール、Flip モデル機能を維持**（`3f2f217`）

  `GameFPS` はゲームのメインループのフレーム制限を引き継ぎます。Replay 中に左 Shift を押すと
  `ReplaySlowRate` の倍率で低速再生し、ゲーム標準の Replay 表示に倍率を出します。キーを離すと
  BGM を再同期します。`ShowOverlay` は別ウィンドウを作らずゲームの BackBuffer に実際の
  Present FPS とフレーム処理時間を描画するため、flip 表示を低下させません。
  `DebugConsole` はデバッグコンソールを開き、`ForceFlipDiscard` はスワップチェーン作成前に
  `DXGI_SWAP_EFFECT_FLIP_DISCARD` を要求します。

- **ゲーム内 U キーによる診断出力を維持**（`3f2f217`）

  `oilp_diag.txt` には x64 バージョン、設定、フック状態、スワップチェーンモデル、Replay
  低速再生、BGM 再同期、自機位置、直近 300 ゲームフレームの情報を記録します。

### 変更

- **overlay の旧補間フレームレート行を当面維持**（`3f2f217`）

  現在の x64 ビルドには補間がないため、この行は補間目標値ではなく実際の Present FPS を
  表示します。

### 削除

- **th06nc x64 の補間および Render Replay 実装を削除**（`3f2f217`）

  オブジェクト外挿、追加フレーム、追加 Present、描画チェーン再生、キャプチャーターゲットの
  履歴保護、D3D11 リソース追跡、x64 補間設定を削除しました。x64 の ini は
  `[Interpolation]` を読み込みません。共有 limiter に残る補間入口は既存の x86 ビルド専用で、
  x86 の補間機能には影響しません。
