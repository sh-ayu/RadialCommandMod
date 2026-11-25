# RadialCommandMod

[English README is here (README.md)](README.md)

Minecraft Bedrock（統合版 / Windows x64）向けの **LeviLamina クライアント MOD** です。  
キーを長押し（またはトグル）して画面中央に円形メニューを開き、**マウスをクイッと振るだけで `/hub`・定型チャット・よく使うコマンド・連続マクロを即実行**できます。接続先サーバーごとのプリセット自動切り替えと、**ゲーム内で円形プレビューをクリックしながら編集できるビジュアル GUI エディタ**を備えています。

> **対応・検証状況**: **LeviLamina 26.60.8**（Minecraft Bedrock **1.26.60.5** / Windows x64）向けにビルドされています。コアロジックのスタンドアロン単体テスト（`radial_tests`）および LeviLauncher 実機環境での動作確認済みです。

---

## 主な機能

- **視点完全ロックの相対フリック円形メニュー（Hold / Toggle 両対応）**
  - デフォルト `Alt` キー（`Tab` / `R` / `V` / `G` / `CapsLock` / `Mouse4` / `Mouse5` / `MouseMiddle` 等へ変更可）で瞬時に展開。
  - 表示中はマウス移動を吸い上げて視点を完全固定し、中央デッドゾーンの外へ振って離すだけで即実行。角度ヒステリシス＆デッドゾーンのシュミットトリガにより境界線でのチラつきを防ぎます。
  - Bedrock ネイティブの UI トライアングルメッシュ描画による **1 ドローコール GPU レンダリング** を採用し、隙間のない滑らかな円形表示と軽量なフレームレートを両立。
  - **長押しモード（Hold）** と **トグル開閉モード（Toggle）** の両方をサポート。
- **接続先サーバーごとのプリセット自動切り替え**
  - 外部サーバーのドメイン・IP・ポート・サーバー名、Realms、ローカルワールド名を自動検知。
  - ワイルドカード（`*` / `?`）・スコープ（`any` / `external` / `local`）・優先度 `(priority, specificity)` で最適なプリセットへ自動切り替え。
  - 初期プリセットとして **Default / The Hive / CubeCraft / Local World** を同梱。
- **ホイール & 数字キーでの即席プリセット切り替え**
  - 円形メニュー表示中に **マウスホイール** または **`1`〜`9` キー** でプリセットを瞬時に切り替え（1画面 2〜12 スロット可変）。
- **階層型サブメニュー・連続マクロ・独立クールダウン・動的プレースホルダ**
  - **サブメニュー（最大 4 階層）**: 左クリックまたはホバー維持（既定 `320ms`）で展開。展開直後の慣性誤爆ガード付き。右クリック / `Backspace` / 中央クリックで親へ戻る。
  - **連続マクロ（最大 16 ステップ）**: ステップごとのミリ秒遅延付き連続実行（例：`gg` → `250ms` 後に `/hub`）。
  - **独立クールダウン**: グローバル連打防止とスロット個別のクールダウンを分離管理。
  - **動的プレースホルダ**: `{player}`, `{x}`, `{y}`, `{z}`, `{server}`, `{world}` を実行時に自動展開（例：`XYZ: {x} {y} {z}`）。
- **直感的な 3 タブ ゲーム内ビジュアル GUI エディタ（`F12` または `/rc editor`）**
  - **1. ホイール**: リアルタイム円形プレビューの扇形をクリックして選択・サブメニュー展開・**16 色カラーパレット**（2 行 × 8 色）・分割数/透明度の即反映・`テスト実行` ボタン。
  - **2. サーバー**: 複数ルール編集・優先度/ポート指定・`+ 今のサーバー用を作成`・`現在値` ワンクリック入力・リアルタイム `一致 / 不一致` 判定バッジ。
  - **3. 設定**: キー捕捉（`Alt` / マウスサイドボタン含む）・ドラッグ対応スライダー・ON/OFF トグルスイッチ・初期設定リセット。
  - **快適な編集 UX & 日本語 / 英語バイリンガル UI**: `auto`（OS 言語自動判定）+ ヘッダー `[JA/EN]` ワンクリック切替、フッター 1 行ホバーヒント、点滅シアンキャレット（クリック位置移動・ダブルクリック全選択・横スクロール）、`Tab` / `Shift+Tab` 項目移動、`Ctrl+Z` Undo、未保存確認バナー、他ウィンドウ切替・ポーズ画面からの編集内容保持、JIS/US キーボード配列対応（`ToUnicodeEx`）、`Ctrl+A/C/V` UTF-8 クリップボード対応。

---

## 導入方法

### 1. `lip` を使う場合（推奨）

```bat
lip install github.com/sh-ayu/RadialCommandMod
```

アップデート時も `mods/RadialCommandMod/config/config.json` は自動保護されます。

### 2. GitHub Releases の ZIP を手動配置する場合

1. [Releases](https://github.com/sh-ayu/RadialCommandMod/releases) から `RadialCommandMod-client-windows-x64.zip` をダウンロードして展開します。
2. `RadialCommandMod` フォルダごと LeviLauncher インスタンスの `mods/` に配置します：
   ```text
   %LOCALAPPDATA%\LeviLauncher\Instances\<インスタンス名>\mods\RadialCommandMod\
   ├── RadialCommandMod.dll
   ├── manifest.json
   ├── config.sample.json
   ├── CHANGELOG.md
   ├── LICENSE
   ├── README.md
   └── README_JA.md
   ```
3. LeviLauncher からゲームを起動すると、初回起動時に `mods/RadialCommandMod/config/config.json` が自動生成されます。

---

## 操作方法

| 操作 | 動作 |
|---|---|
| `Alt`（既定）長押し | 円形メニューを開く（トグルモード時は単押しで開閉） |
| マウスを振る → キーを離す | 選択中セクターを実行（Hold モード） |
| 左クリック | サブメニューを開く / 選択中セクターを即実行 |
| 右クリック / `Backspace` | サブメニューから 1 段戻る（ルートならキャンセル） |
| `Esc` / 中央デッドゾーンで離す | キャンセルして閉じる |
| マウスホイール / `1`〜`9` | プリセットを手動切り替え |
| `F12` または `/rc editor` | ゲーム内ビジュアル GUI エディタを開く |

---

## クライアントコマンド（`/rc`）

| コマンド | 説明 |
|---|---|
| `/rc` または `/rc help` | コマンド一覧を表示 |
| `/rc status` | 現在の状態・モード・キー設定・アクティブプリセット・設定パスを表示 |
| `/rc editor` | ゲーム内ビジュアル GUI エディタを開く |
| `/rc server` | 現在検知している接続先情報（ホスト/IP/ポート/サーバー名/ワールド名）とマッチ結果を表示 |
| `/rc presets` | 登録済みプリセット一覧を表示 |
| `/rc preset <id\|name\|auto>` | プリセットを手動切り替え（`auto` で自動判定へ復帰） |
| `/rc bind <key>` | ホイール起動キーを変更（例：`Alt`, `Tab`, `R`, `V`, `G`, `Mouse4`, `Mouse5`） |
| `/rc editorkey <key\|None>` | エディタ起動キーを変更（既定：`F12`、`None` で無効化） |
| `/rc mode <hold\|toggle>` | 長押しモード（`hold`）とトグルモード（`toggle`）を切り替え |
| `/rc lang <auto\|ja\|en>` | UI 言語を切り替え |
| `/rc sensitivity <0.2..4.0>` | 相対フリック感度を変更 |
| `/rc cooldown <ms>` | グローバルクールダウン（ミリ秒）を変更 |
| `/rc toggle` | MOD の ON / OFF を切り替え |
| `/rc reload` / `/rc reset` | `config.json` を再読み込み / デフォルトへ初期化 |

---

## 設定ファイル（`config.json`）

JSON / JSONC（`//` 行コメント、`/* ... */` ブロックコメント、末尾カンマ）に対応しています。範囲外の値は安全にクランプされ、書式が壊れていた場合は `config.json.corrupt-<時刻>` にバックアップしてデフォルトを再生成します。

### トップレベル設定

| キー | 既定値 | 範囲 / 選択肢 | 説明 |
|---|---|---|---|
| `enabled` | `true` | `true` / `false` | MOD 全体の有効/無効 |
| `language` | `"auto"` | `"auto"`, `"ja"`, `"en"` | UI 言語（`auto` は OS 言語から判定） |
| `activationMode` | `"hold"` | `"hold"`, `"toggle"` | ホイールの開閉方式 |
| `activationKey` | `"Alt"` | キー名または VK 数値 | ホイール起動キー（`Alt`, `Tab`, `R`, `Mouse4` 等） |
| `editorKey` | `"F12"` | キー名 / `"None"` | エディタ起動キー（`"None"` で無効） |
| `flickSensitivity` | `1.00` | `0.20`〜`4.00` | 相対フリック感度 |
| `outerRadius` | `92.00` | `50.0`〜`180.0` | ホイールの外半径（GUI px） |
| `innerRadius` | `32.00` | `14.0`〜`85.0` | ホイールの内半径（GUI px） |
| `deadzoneRadius` | `22.00` | `8.0`〜`80.0` | 中央キャンセル領域の半径（GUI px） |
| `backgroundOpacity` | `0.78` | `0.15`〜`1.00` | ホイール背景の不透明度 |
| `fontScale` | `0.95` | `0.50`〜`1.80` | セクターラベルの文字倍率 |
| `holdDelayMs` | `0` | `0`〜`500` | 押下から開くまでの遅延（ms） |
| `submenuHoverMs` | `320` | `0`〜`1500` | サブメニュー自動展開ホバー時間（`0` で無効） |
| `globalCooldownMs` | `350` | `0`〜`10000` | 連打防止クールダウン（ms） |
| `executeOnRelease` | `true` | `true` / `false` | Hold モードでキー離し時に実行 |
| `executeOnLeftClick` | `true` | `true` / `false` | ホイール表示中の左クリックで実行/展開 |
| `autoSwitchPreset` | `true` | `true` / `false` | 接続先サーバーごとの自動プリセット切替 |
| `showPresetToast` | `true` | `true` / `false` | 切替・実行時の上部トースト表示 |
| `showNumberHints` | `true` | `true` / `false` | `1`〜`9` プリセット切替ヒントの表示 |
| `invertWheelScroll` | `false` | `true` / `false` | マウスホイール切替方向の反転 |
| `verboseLog` | `false` | `true` / `false` | 詳細ログ出力 |
| `defaultPresetId` | `"default"` | プリセット ID | ルール未一致時のフォールバックプリセット |

### 動的プレースホルダ変数

コマンド・チャット・マクロの各 `value` で以下が使えます：

| 変数 | 展開内容 |
|---|---|
| `{player}` | 自分のプレイヤー名 |
| `{x}`, `{y}`, `{z}` | 現在座標（整数） |
| `{server}` | 現在の接続先表示名 |
| `{world}` | 現在のワールド名 |

---

## 安全性・マルチサーバーでの動作

- 本 MOD は純粋なクライアントサイド利便性ツールです。
- ユーザーが明示的にホイール操作を行ったタイミングでのみ、通常のチャット画面から送信するのと同じ標準パケット（`TextPacket` / `CommandRequestPacket`）を 1 回送信します。認証バイパスやゲームデータ改ざんは一切行いません。
- チャット・インベントリ・ポーズ・サーバーフォーム・死亡・ベッド画面では自動的に抑止され、誤爆を防ぎます。

---

## トラブルシューティング・既知の制限

- **日本語を GUI エディタで入力したい**: オーバーレイ入力欄は OS の IME 変換ウィンドウを直接出さないため、メモ帳やチャット欄で入力した日本語を **`Ctrl+V` で貼り付け**てください（英数字および JIS 記号 `:`, `@`, `_`, `/`, `*` 等は直接入力に対応）。
- **他 MOD とキーが競合する**: `F12` でエディタを開き `3. 設定` タブ、または `/rc bind <key>`・`/rc editorkey <key|None>` で即座に変更できます。
- **ライブアンロード非対応**: MOD の更新・削除時は Minecraft を終了してから行ってください。

---

## ソースからのビルド手順

Windows x64、Visual Studio Build Tools、LLVM (`clang-cl`)、[xmake](https://xmake.io/) 3.0 以上が必要です。

```bat
:: 1. スタンドアロン単体テスト（LeviLamina 不要）
xmake f -m release --with_mod=n -y
xmake build radial_tests
xmake run radial_tests

:: 2. LeviLamina 26.60.8 クライアント MOD DLL のビルド
xmake f -m release --with_mod=y -y
xmake build RadialCommandMod

:: 3. 配布用 ZIP の作成（dist/RadialCommandMod-client-windows-x64.zip）
powershell -ExecutionPolicy Bypass -File scripts/package.ps1
```

## ライセンス

[MIT License](LICENSE)
