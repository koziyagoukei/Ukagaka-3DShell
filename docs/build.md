# ビルド手順

## 必要なもの

| ツール | 備考 |
|---|---|
| Visual Studio 2022 以降、または Build Tools for Visual Studio | 「C++ によるデスクトップ開発」ワークロード（MSVC x64、Windows 10/11 SDK、CMake／Ninja を含む） |
| Git | CMake の FetchContent で依存ライブラリを取得するときに使う |
| インターネット接続 | 初回の configure 時のみ（libmmd と stb を GitHub から取得する） |

動作確認した環境：Visual Studio Build Tools 2026 18.11（MSVC 19.51、「C++ によるデスクトップ開発」と「C++ CMake ツール」）、Windows SDK 10.0.26100.0、同梱の CMake 4.3.1 と Ninja 1.13.2。v0.1.0-beta の配布ファイルはこの環境でビルドしました。`scriptsuild.ps1` は安定版の Visual Studio を優先し、見つからないときだけプレビュー版を使います。

## アーキテクチャ

| プリセット | 内容 |
|---|---|
| `msvc-x64` | 3D ランタイム、ツール、テスト、SAORI の x64 版（テスト用） |
| `msvc-x86` | `ukagaka3d_saori.dll`（SSP は 32bit のため x86。CRT は静的リンク）と x86 テスト。x86 テストは x64 のランタイムとテスト資産を使うので、x64 を先にビルドする |

`scripts\build.ps1` は、既定で x64 と x86 の両方を順にビルドします（`-Arch x64` または `-Arch x86` で片方だけ）。

## 方法 1：スクリプト（推奨）

`scripts/build.ps1` は、Visual Studio を自動で見つけて vcvars64 の環境を取り込みます。Developer PowerShell を開く必要はありません。

```powershell
# Debug と Release をビルドする
powershell -ExecutionPolicy Bypass -File scripts\build.ps1

# Release だけビルドしてテストする
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release -Test

# クリーンビルド（build\msvc-x64 を削除してから）とすべてのテスト
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean -Test

# ウィンドウを開く GUI テストを除いてテストする（CI やリモート環境向け）
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Test -NoGuiTests
```

## 方法 2：Developer PowerShell for VS

```powershell
cmake --preset msvc-x64          # configure（Ninja Multi-Config）
cmake --build --preset debug
cmake --build --preset release
ctest --preset release           # すべてのテスト
ctest --preset release -LE gui   # GUI テストを除く
```

できあがるファイル：

- `build\msvc-x64\Debug\ukagaka3d.exe`
- `build\msvc-x64\Release\ukagaka3d.exe`

exe が使う DLL は、OS 標準のもの（d3d11、dcomp、dwmapi、user32、ole32 など）だけです（`dumpbin /dependents` で確認済み）。MSVC ランタイムは依存ライブラリを含めて静的リンク（`/MT`）しているので、VC++ 再頒布可能パッケージは要りません。

## テストの内容

| テスト | 内容 |
|---|---|
| `generate_assets` | `make_test_assets` で合成 PMX／VMD／テクスチャ（日本語のフォルダ名・ファイル名）、壊れたファイルを生成する |
| `unit_tests` | 引数の解析、UTF-8／UTF-16 変換、カメラのフィット、画像のデコード（PNG／TGA／BMP）、PMX/VMD の読み込み、モーションで頂点が動くこと、エラー時の動作。Phase 0.5：`--fps`／`--physics` の解析と不正な値、固定ステップ（30／120fps で tick 数が同じ、遅延の上限）、一時停止と再開、先頭へ戻したときの物理リセット、ループ境界（状態の保持）、物理 ON／OFF、物理テストモデルの読み込み、Bullet がないときのフォールバック、50 回の読み込み・破棄でメモリが増えないこと |
| `cli_fps_*`、`cli_physics_value` | 不正な `--fps`／`--physics` の値で終了コード 1 になる |
| `gui_physics_*`、`gui_no_physics_120fps` | 物理 ON の 30fps と無制限、物理 OFF の 120fps、剛体のないモデルでの `--physics`、それぞれが正常終了する |
| `cli_*` | exe の終了コード（`--help`／`--version`／引数なし／不明な引数／不正な値／モデルがない／壊れた PMX／PMX ではないファイル／壊れた VMD／VMD がない） |
| `gui_*`（ラベル `gui`） | 実際にウィンドウを開いて 3 秒描画し、最後のフレームを PNG で保存して正常終了するか（通常ウィンドウの静止表示とモーション、背景透過のモーション） |
| `verify_captures`（ラベル `gui`） | 保存した PNG にモデルが写っているか、背景透過モードで背景のアルファが 0 になっているか、縁が半透明になっているか |
| `ipc_saori_tests`（x64） | SAORI の解析と DLL の呼び出し、パイプの接続制御とタイムアウト、異常終了の検知、PMX・VMD・モーフの操作 |
| `saori_x86` / `yaya_ghost_x86`（x86） | 32bit DLL の読み込み・解放と x64 ランタイムとの通信。YAYA 6 の DLL がある場合は、サンプル辞書の応答とランタイムの起動・終了も確認 |
| `satori2yaya`（x64） | Python 3 がある場合に、里々の会話辞書から YAYA への変換を確認 |

GUI テストはデスクトップのあるセッションで実行してください。

## 依存ライブラリのバージョン固定

`cmake/Dependencies.cmake` で、取得するコミットを固定しています。

- libmmd：`f478f985ca506fa9d265b6ba8f2359009b087548`（タグ v0.3.2、Phase 0 から変更なし）。物理モジュールは ON。テストとインストールは OFF
- Bullet：3.25 のリリース tar.gz（SHA-256 `c45afb63…ceb77`）。libmmd が取得するものと同じリリースです。こちらで先に宣言して、ハッシュの固定と `/MT`（静的 CRT）でのビルドを指定しています。Bullet 本体、デモ、拡張、テストはビルドしません。初回はおよそ 140MB をダウンロードします
- `-DUKA3D_ENABLE_BULLET=OFF` にすると Bullet なしでビルドします。その場合、物理演算は「利用不可」となり、アニメーションだけで動きます
- stb：`4e9b368bfc071e97206ea5c6adba68dd0cbf8897`

更新するときは、コミットハッシュを書き換えたうえで、クリーンビルドとすべてのテストを実行してください。

## トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| `Could not create named generator Visual Studio 18 2026` | 古い CMake を使っている。プリセット `msvc-x64`（Ninja）を使えば VS ジェネレーターは不要 |
| `cl` が見つからない／`CMAKE_CXX_COMPILER` のエラー | vcvars64 の環境で実行していない。`scripts\build.ps1` を使うか、Developer PowerShell から実行する |
| PATH 上の別の CMake（MSYS2 など）が使われる | `scripts\build.ps1` は VS 同梱の CMake を優先する。手動で実行するときは `where cmake` で確認する |
| `fxc` が見つからない | Windows SDK がインストールされていない。Visual Studio Installer で Windows 10/11 SDK を追加する |
| 依存ライブラリの取得に失敗する | Git とネットワークを確認する。プロキシ環境では `git config --global http.proxy` を設定する |
