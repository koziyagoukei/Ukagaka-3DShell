# アーキテクチャと技術調査（Phase 0）

## 1. 環境調査の結果（2026-11-08）

| 項目 | 結果 |
|---|---|
| OS | Windows 11 Pro 10.0.26300（x64） |
| Visual Studio | Visual Studio Build Tools 2026 18.11（MSVC 19.51.36260）。開発中は Visual Studio 2026 Insiders（MSVC 19.50）も使用 |
| Windows SDK | 10.0.26100.0（fxc.exe を含む） |
| CMake | VS 同梱の 3.31.6（PATH 上の `cmake` は devkitPro の MSYS2 版 4.3.3 で、MSVC 用の Visual Studio ジェネレーターを持っていない） |
| Ninja | VS 同梱 1.12 系 |
| Git / GitHub CLI | git 2.56.0、gh 2.103.3（認証済み） |
| GPU（検証機） | Intel UHD Graphics（機能レベル 11.1） |

**ビルド方式の決定**：Ninja Multi-Config と MSVC（`CMakePresets.json` のプリセット `msvc-x64`）。

- 同梱の CMake 3.31 は「Visual Studio 18 2026」ジェネレーターに対応していないため、VS ジェネレーターは使わない。
- `scripts/build.ps1` は vswhere で VS を見つけ、vcvars64 の環境を取り込み、VS 同梱の CMake／Ninja を PATH の先頭に置く。
- この環境では PATH 上の `cmd` も MSYS2 版に解決されたため、スクリプトは `%ComSpec%` を使って cmd.exe を呼ぶ。

## 2. libmmd の評価

調べた対象：<https://github.com/haturatu/libmmd>（v0.3.2、コミット `f478f985`）。ヘッダー（`include/mmd/*.hpp`）と実装を直接読んで確認した。

| 確認項目 | 結果 |
|---|---|
| ライセンス | MIT（MikuMikuDayo 由来の部分も MIT）。THIRD_PARTY_NOTICES.md あり |
| 依存関係 | C++20 のみ（Windows では iconv は不要）。物理演算を使う場合だけ Bullet（任意） |
| ビルド | CMake 3.25 以上。サブプロジェクトとして使える（`mmd::core`、`mmd::animation`）。CI には Windows／MSVC のビルドもある |
| PMX | `mmd::pmx::load(path)` → `PmxModel`（頂点・インデックス・テクスチャパス・マテリアル・ボーン・モーフ・剛体・ジョイント） |
| VMD | `mmd::loadVmd(path)` → `VmdMotion`（ボーン／モーフ／カメラ／ライト／IK のキー、`lastFrame`） |
| 評価 | `MmdAnimator::evaluateInto(frame, t, dt, gpuSkinning=false)` で、ボーン・モーフ（グループ／フリップを含む）・IK・付与・SDEF/QDEF の **CPU スキニング済み頂点**と、マテリアルモーフ適用後のマテリアル値が得られる |
| GPU スキニング | `gpuSkinning=true` でボーン行列（回転＋平行移動）とモーフの重みを取得できる。Phase 0 では使わない |
| テクスチャ | パス文字列（UTF-8）と `resolveTexturePath()` まで。画像のデコードは行わない → stb_image と WIC で補う |

### libmmd を使ううえでの注意点と対処

1. **ソースの文字コード**：libmmd のソースには BOM なしの UTF-8 で日本語のボーン名（`"左腕"` など）が書かれている。日本語版 Windows の MSVC は、そのままだと CP932 として読んでしまう。
   → `cmake/Dependencies.cmake` で `mmd_core` と `mmd_animation` に `/utf-8` を付けた。
2. **`pmx::resolveTexturePath()` の文字コード**：UTF-8 の `std::string` から `std::filesystem::path` を作っているため、Windows ではシステムの ANSI コードページ（CP932）として解釈される。
   → `MmdModel::resolveTexturePath()` で UTF-8 → UTF-16 に明示的に変換して使う。あわせて、アプリのマニフェストで `activeCodePage=UTF-8` も指定している（念のための二重対策）。
3. **ログ出力**：libmmd は `std::cout`／`std::cerr` に `[DEBUG]`・`[INFO]`・`[WARN]` 付きで出力する（30 フレームごとのボーン診断を含む）。
   → `log::initialize()` でストリームバッファを差し替え、行ごとにアプリのログへ流す。`[DEBUG]` は `--debug` のときだけ記録する。
4. **検証が厳しい**：`pmx::validate()` は、表示枠の参照切れなど描画に影響しないものもエラーとして扱う。
   → 警告としてログに出すだけにして、描画に必要なこと（インデックスの範囲、マテリアル面数の合計）だけを独自にチェックしている。
5. **ファイル I/O**：Windows では `std::ifstream(std::filesystem::path)` を使うため、UTF-16 のパスで正しく開ける（日本語パスで確認済み）。

## 3. モジュール構成

```text
main.cpp ──► app::parseCommandLine ──► app::runApp
                                          │
          ┌───────────────────────────────┼─────────────────────────────┐
          ▼                               ▼                             ▼
  mmd::MmdModel                  platform::ShellWindow          renderer::D3DContext
  (libmmd ラッパー)                (Win32 ウィンドウ／入力)          (デバイス／スワップチェーン／DComp)
          │                                                             │
          └──────────────► renderer::ModelRenderer ◄────────────────────┘
                           (GPU バッファ／シェーダー／マテリアル／TextureCache)
```

- **mmd/**：libmmd のデータ構造だけに依存する。Direct3D と Win32 には依存しない（例外は UTF 変換ユーティリティだけ）。
- **renderer/**：Direct3D 11 に依存する。ウィンドウについては HWND を受け取るだけ。
- **platform/**：Win32 のウィンドウ処理、ログ、文字コード変換。描画には依存しない。
- **app/**：上の 3 つを組み合わせ、メインループ・入力・メニューを担当する。

将来の SSP 連携（SAORI／SHIORI からのコマンドを受けるなど）は `app/` の外側に層を追加する想定。Phase 0 では、そのための抽象化は入れていない。

## 4. 描画パイプライン

1. **スキニング**：libmmd が CPU でスキニングを行う（`gpuSkinning=false`）。位置と法線を動的頂点バッファ（ストリーム 0、`D3D11_USAGE_DYNAMIC`、毎フレーム `WRITE_DISCARD`）に書き込む。UV とエッジ倍率は不変バッファ（ストリーム 1）に置く。
2. **インデックス**：頂点が 65535 以下なら 16 ビット、それより多ければ 32 ビット。
3. **マテリアル**：PMX の順番どおりに描画する（MMD と同じで、半透明の前後関係も PMX の並びに従う）。描画フラグ `0x01` は両面描画（カリングなし）、それ以外は裏面カリング。左手系で時計回りが表面（D3D の既定）。
4. **シェーディング**（`shaders/model.hlsl`）：MMD の標準シェーダーに近い式にしている。
   - 基本色：`saturate(diffuse.rgb × lightColor + ambient)`。アルファは `diffuse.a`
   - テクスチャ：材質モーフの乗算・加算値を反映
   - スフィアマップ：ビュー空間の法線で UV を作り、乗算（sph）または加算（spa）
   - トゥーン：`tex(toon, (0, 0.5 − N·L × 0.5))`。共有トゥーン（toon01〜10.bmp）は MMD 本体の付属ファイルで再配布できないため、**手続き的に作ったグラデーションで代用**している
   - スペキュラ：Blinn-Phong（ハーフベクトル）
   - ライト：MMD の既定値（方向 (−0.5, −1.0, 0.5)、色 154/255）
5. **輪郭線**：インバーテッドハル法。法線方向に「エッジサイズ × 頂点エッジ倍率 × 1 ピクセルのワールド長」だけ押し出し、表面をカリングして描く。画面上でほぼ一定の太さになる。
6. **アルファ**：シェーダーはプリマルチプライドカラーを出力する。ブレンドは `ONE / INV_SRC_ALPHA`（色・アルファとも）。アルファが 1/255 未満のピクセルは `discard` し、深度を書き込まない。
7. **アンチエイリアス**：4x MSAA のレンダーターゲットに描き、`ResolveSubresource` でバックバッファへ転送する（フリップモデルのスワップチェーンは MSAA を直接持てないため）。
8. **カメラ**：バインドポーズのバウンディングボックス全体が画角に収まる距離を、縦と横の両方から計算する（左手系、`LookAtLH` と `PerspectiveFovLH`、画角 30°）。

## 5. 背景透過の方式

### 検討した方式

| 方式 | 評価 |
|---|---|
| 黒背景＋カラーキー（`LWA_COLORKEY`） | 指示書で禁止。半透明やアンチエイリアスの縁も扱えない → 不採用 |
| `UpdateLayeredWindow`＋GPU からの毎フレーム読み出し | ピクセル単位のアルファは扱えるが、毎フレーム GPU→CPU のコピーとビットマップ転送が必要で遅い → 不採用 |
| `DwmExtendFrameIntoClientArea`＋リダイレクトサーフェス | フリップモデルとアルファの組み合わせの挙動が環境によって違う → 不採用 |
| **DirectComposition＋`CreateSwapChainForComposition`** | DWM がプリマルチプライドアルファのまま合成する。GPU だけで完結し、フリップモデルで低遅延 → **採用** |

### 実装（`D3DContext`・`ShellWindow`）

- ウィンドウは `WS_POPUP` で、拡張スタイルに `WS_EX_NOREDIRECTIONBITMAP` を付ける（GDI のリダイレクトサーフェスを持たない）。
- スワップチェーンは `DXGI_FORMAT_B8G8R8A8_UNORM`、`DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`、`DXGI_ALPHA_MODE_PREMULTIPLIED`。
- `DCompositionCreateDevice` → `CreateTargetForHwnd(topmost=TRUE)` → ビジュアルの `SetContent(swapChain)` → `Commit` の順に設定する。
- 背景は (0, 0, 0, 0) でクリアする。アルファが 0 のピクセルにはデスクトップがそのまま見える。

### マウス操作（クリック透過・ドラッグ）

- 毎フレーム、カーソルの下にある 1 ピクセルをバックバッファからステージングテクスチャへコピーする。CPU が待たされないよう、3 枚のリングバッファで 2 フレーム遅れで読み出す。
- アルファが 8 未満なら、`WS_EX_LAYERED | WS_EX_TRANSPARENT` を付けてクリックを下のウィンドウへ通す。モデルの上では外す。`WS_EX_LAYERED` を付けている間も DirectComposition の描画は表示されたままであることを確認した。
- ドラッグは自前で実装している（`WM_LBUTTONDOWN` で `SetCapture` し、`WM_MOUSEMOVE` で `SetWindowPos`）。`HTCAPTION` を使うとシステムのモーダルループに入ってアニメーションが止まるため。
- 通常ウィンドウモードでのサイズ変更や移動中（モーダルループ中）は、`WM_ENTERSIZEMOVE` でタイマーを動かして描画を続ける。

## 6. ログとエラー処理

- ログは UTF-8 で、`ukagaka3d.log`（既定は exe と同じフォルダ）とコンソールの両方に出力する。WARN 以上は stderr に出す。
- 記録する内容：起動時のオプション、PMX/VMD の読み込み結果（頂点数・マテリアル数・一致したボーントラック数など）、D3D11 の初期化結果（アダプタ・機能レベル・MSAA）、スワップチェーンの方式、テクスチャの読み込み結果、終了コード、致命的エラー（原因の詳細付き）。
- 読み込みエラーは `LoadError`（日本語メッセージ）として扱い、終了コードは 2。D3D のエラーは `D3DError`（HRESULT とシステムメッセージ）で、終了コードは 3。
- コンソールサブシステムの exe なので、ターミナルから起動したときは出力がそのまま見える。エクスプローラーから起動したとき（自分しかコンソールを使っていないとき）は `FreeConsole` でコンソールを閉じ、エラーをメッセージボックスでも表示する。

## 7. 既知の制約（Phase 0）

- （Phase 0.5 で解消）物理演算は第 8 節を参照。
- 共有トゥーン（toon01〜10）は近似のグラデーション。MMD の付属ファイルを読み込めるようにするのは今後の課題。
- 地面影、セルフシャドウ、MME（エフェクト）には対応していない。
- 半透明の前後関係は PMX のマテリアル順に従う（MMD と同じ）。ソートは行っていない。
- （Phase 0.5 で解消）フレームレート制限は第 8 節を参照。
- マルチモニターで DPI が異なる場合の、モニター間移動時のサイズ調整（`WM_DPICHANGED`）は未実装。

## 8. Phase 0.5：フレームレート制御と物理演算

### 8.1 モジュールの追加

| モジュール | 役割 |
|---|---|
| `app/Playback` | 純粋なロジック（Win32／D3D に依存しない）。経過時間から「評価するモーションフレーム」と「libmmd に渡すシミュレーション時間」を決める。固定ステップの蓄積、遅延の上限、一時停止、先頭への巻き戻し、ループ境界の判定、補間係数 |
| `platform/FramePacer` | フレームレートの上限。VSync 分周（`Present(N)`）か、高精度ウェイタブルタイマー＋`Present(0)` を選ぶ。メッセージが来たら待機を解除する |
| `mmd/MmdModel`（拡張） | `MmdPhysics` を持ち、`setupPhysics`／`setPhysicsEnabled`／`resetPhysics`／`evaluateAcrossLoop` を提供する。物理オブジェクトはアニメーターより先に作り、後に破棄する（アニメーターが参照しているため） |
| `renderer/ModelRenderer`（拡張） | 直前と現在のスキニング結果を CPU 側に保持し、補間した頂点をアップロードする |

### 8.2 1 フレームの処理順

```text
メッセージ処理 → (最小化／非表示なら待機) → FramePacer.frameDue? → Playback.update(実経過時間)
  → seek   : resetPhysics → evaluate(frame, 0)              … libmmd が物理をリセットし、剛体を姿勢に合わせて配置
  → advance: evaluate(frame, n/60)                          … アニメーション → IK → キネマティック剛体 → Bullet → 物理ボーン → 物理後 IK → CPU スキニング
  → wrap   : evaluateAcrossLoop(frame, n/60)                … 動的剛体の状態をループ境界をまたいで保持
→ ModelRenderer.setFrame → upload(補間係数) → 描画 → Present(syncInterval) → FramePacer.framePresented
```

CPU スキニングは libmmd の `evaluateInto` の中で、物理を反映したボーンに対して行われる（物理の後にスキニング）。そのため、更新の順序はライブラリが保証する。

### 8.3 VSync との関係

- VSync 分周モードでは、待つのは `Present(N)` だけで、タイマーは使わない。
- タイマーモードでは `Present(0)` を使う。そのため、タイマーの後にさらに VBlank を待つ「二重待ち」は起きない。
- `IDXGIDevice1::SetMaximumFrameLatency(1)` で、キューに積むフレームを 1 つにしている。CPU が GPU より先に進みすぎないようにし、遅延も減らす。
- 最小化中、非表示中、`DXGI_STATUS_OCCLUDED` のときは描画しない（`MsgWaitForMultipleObjectsEx` で 100ms ごとに確認）。

### 8.4 物理の時間管理

- `PhysicsSettings::fixedTimeStep` は、1/60 秒に ×1.0001 の余裕を持たせた値にしている。libmmd は `ceil(dt / fixedTimeStep)` で分割数を決めるため、`dt = n/60` を float で計算したとき、n+1 回に分割されないようにするため。
- 1 回の更新で進めるのは最大 5 ステップまで。それ以上の遅れは捨てる（`Playback::droppedSeconds`）。
- 30fps のときは 2 ステップ分を 1 回の評価で進める。libmmd は、その間のキネマティック剛体の動きを補間しながら 2 回に分けて計算する。120fps で 1 ステップずつ進めた場合との差は、約 1% であることをテストで確認した。

## 9. Phase 1：SSP／YAYA 連携

### 9.1 構成

```text
SSP（x86）
 ├─ YAYA 6（yaya.dll）… 辞書: shiori3_mini.dic（SHIORI/3.0 の受け口）＋ ukagaka3d.dic（3D の共通関数）＋ ghost.dic
 │    └─ REQUESTLIB → ukagaka3d_saori.dll（x86、静的 CRT）── SAORI/1.0
 │                          │ CreateProcess（CREATE_NO_WINDOW）＋名前付きパイプ（JSON）
 │                          ▼
 └─ （シェルウィンドウ Tmainform）◄── 追従 ──  ukagaka3d.exe（x64）
```

| モジュール | 役割 |
|---|---|
| `src/ipc/` | プロトコル（フレーミング、JSON、エラーコード）、`PipeServer`（ランタイム側、専用スレッド）、`PipeClient`（タイムアウト付きの同期クライアント）、`PathPolicy`（許可フォルダの判定）。x86 と x64 の両方でビルドする |
| `src/saori/` | `SaoriProtocol`（SAORI/1.0 の解析と整形、UTF-8／Shift_JIS）、`SaoriBridge`（コマンド、起動と監視、パスの解決、設定ファイル）、`dllmain.cpp`（エクスポート関数） |
| `src/app/App.cpp` | IPC のコマンドを UI スレッドで実行する（`processIpcQueue`）。モデルの差し替え、モーションの切り替え（物理の状態を保ったまま）、モーフ、`next` モーション、親プロセスの監視、ウィンドウの追従 |

通信の詳細は [ipc-protocol.md](ipc-protocol.md) を参照してください。

### 9.2 設計上の判断

- **SHIORI に依存しない**：SAORI-universal の DLL は、SHIORI の種類に関係なく呼べます。IPC は JSON なので、GUI など SAORI 以外のクライアントからも使えます。
- **YAYA の公式テンプレートに依存しない**：YAYA 6 本体（C++）は、REQUESTLIB の文字列をそのまま SAORI に渡します。`EXECUTE SAORI/1.0` の要求文は、辞書側（`ukagaka3d.dic` の `Call3D`）で組み立てます。SHIORI/3.0 の解析も、自作の `shiori3_mini.dic` で行います。
- **x86／x64 の分離**：SSP と YAYA は 32bit です。3D ランタイム（libmmd、Bullet、D3D11）は x64 のままにし、プロセスを分けて名前付きパイプでつなぎます。
- **モーションの切り替えで物理をリセットしない**：`Playback::restart` で、次の評価をループ境界と同じ扱い（`evaluateAcrossLoop`）にします。挨拶から待機に切り替わるときに、髪やマントが跳ねません。
- **表情の一括設定**：`set_morphs` で「すべて戻す→複数を設定」を1回のコマンドで行います。途中の顔が描画されることはありません。
- **SSP の表示との合わせ方**：透明シェルと 3D ウィンドウを、同じ矩形に重ねます（`--follow-window-class Tmainform`）。
  - SSP のバルーン位置の計算（シェル基準の offset）や DPI 倍率が、そのまま 3D に当てはまります。
  - マウスは、3D 側で受けた右クリックとダブルクリックを SSP のシェルへ `PostMessage` で転送します。左ドラッグは、SSP のシェルウィンドウを `SetWindowPos` で動かします。
- **終了の順序**：OnClose では別れのトークとモーションだけを返し、`\-` でゴーストが閉じたあとの SHIORI unload（`shiori3_mini.dic` の `unload` と SAORI の `unload`）でランタイムを止めます。親プロセス（SSP）が異常終了しても、ランタイムは親の終了を検知して自分で終了します。

### 9.3 既知の制約

- SSP のシェルクラス名 `Tmainform` は SSP の実装に依存します（SSP 2.9.32 で確認）。
- 左ドラッグで SSP のシェルを外から動かす処理は実装済みですが、Claude の画面操作ツールではこのウィンドウにドラッグを送れないため、未確認です。
- 3D 側からゴーストへのイベント（クリックした部位など）、なでられ判定、視線追従は対象外です。
- 3D ランタイムの exe も SAORI DLL も、MSVC ランタイムを静的リンクしています（依存先は OS 標準の DLL だけ）。VC++ 再頒布可能パッケージのない PC でも動きます。以前は exe だけ動的リンクで、再頒布可能パッケージが必要でした。

## 品質改善：スキニングの置き換えと着席の再設計

計測と比較の結果は [progress.md](progress.md) にあります。

### スキニング（`src/mmd/Skinning`）

- 計測では、1 tick（CG-CA Uka、約 5 万頂点）4.95ms のうち約 4.45ms が libmmd の CPU スキニングでした。libmmd は毎 tick、全頂点の構造体（1 頂点 172 バイト、合計約 8MB）をコピーしてから、影響ボーンごとにクォータニオンで変換します。物理（Bullet）は 0.38ms、アニメーション＋IK は 0.12ms でした。
- 現在は、libmmd にはボーン姿勢と頂点モーフの重みだけを計算させ（`gpuSkinning = true`、`VertexOutput::omitUnchangedGpu`）、頂点の変形は `Skinner` が行います。
  - ボーンごとに 3×4 行列を作り、位置・法線・ウェイトだけの配列を走査して、描画用のバッファへ直接書き込みます。
  - BDEF1／2／4 は線形ブレンド、SDEF と QDEF は libmmd と同じ式です。無効なボーンやウェイトの扱いも合わせています。
  - 頂点モーフは、重みが変わったときだけ、影響する頂点を基本形から作り直します（差分の積み上げをしないので、長時間動かしても誤差がたまりません）。
- libmmd の結果との一致は自動テストで確認しています（`tests/quality_tests.cpp`）。合成モデルの全ウェイト種別、モーフ、物理ありの 150 tick とループ境界で、位置の差は 10⁻⁵ 未満です。Uka の全頂点でも最大 4×10⁻⁶ でした。
- ソフトボディを持つモデルは、頂点を libmmd が変形する必要があるため、従来の経路に切り替わります。
- `tools/bench_eval.cpp`（`uka3d_bench`）で、ウィンドウなしで 1 tick の内訳を計測できます。

### 姿勢のクロスフェード（`PoseBlender`）

- モーションを切り替えた瞬間の表示ボーンを保存し、新しいモーションの姿勢へ一定時間かけてブレンドします。
- ブレンドは親ボーンからの相対（ローカル）空間で行います。回転は球面補間、親からのオフセットは線形補間にして、親から順に組み立て直します。頂点やワールド座標で補間すると回転の途中で手足が縮みますが、この方法ではボーンの長さが保たれます（テストで確認）。
- 物理は切り替え先のモーションでそのまま進め、表示だけをブレンドします。

### 着席（`src/app/SeatAnchor`、`App::placeSeat`）

- **接地基準**：骨盤と太ももの付け根側半分に付いている頂点から、球面上の 96 方向それぞれでいちばん遠い点を代表点として選んでおきます（Uka で 102 点）。実行中は代表点だけを姿勢で変換して投影し、画面上でいちばん低い点を座面とします。全頂点を調べる必要がなく、腰ボーンの有無にも左右されません。
- **位置合わせ**：ウィンドウは対象の上端から計算した整数ピクセルの位置に置き、残りの端数（±0.75px 以内）はカメラの投影行列に平行移動を足して合わせます（`Camera::ndcOffsetX/Y`）。`project()` と `ray()` も同じ行列を使うので、当たり判定は描画と一致します。
- **追従**：対象ウィンドウの移動・サイズ変更は WinEvent で受け取り、その場でウィンドウを動かします。対象を 16ms ごとに動かす試験で、追従のずれは最大 11px から 1px になりました。
- **遷移**：着席時は現在位置から座る位置へ、立ち上がり時は元の位置へ、弧を描いて移動します。その間、姿勢はクロスフェードします。

### モーションの生成（`src/motion/MotionGenerator`、`tools/motion_gen.cpp`）

以前のサンプルモーションは、合成モデル向けに角度を直接書いたものでした。脚の長さも足の形も考えていないので、うかでは歩行中に足が前後に大きく滑り（接地中の足の速さが歩く速さの平均 369%）、床へ 0.2 沈んでいました。待機は脚 IK に任せていたため、腰が下がると IK が膝を曲げ切れず、足が 0.1 沈んでいました。

生成器はモデルから脚（太もも・すね・足首の位置）、足裏の前後の長さ、腕の初期角度を測り、次のように作ります。

- **歩行**：かかとの接地、足裏全体の接地、かかとの持ち上げ、つま先の蹴り出しを、それぞれ接地点を中心に回します。接地中の足は、歩く速さと同じだけ後ろへ動きます。遊脚は足裏のいちばん低い点の高さで持ち上げます。腰の高さは、どの瞬間も両脚が届く範囲に収めます。1 周期の長さは、3D ウィンドウの移動速度とカメラの拡大率から決めます。
- **待機**：呼吸と上半身・頭のゆっくりした動き。腰を少し落とし、脚は同じ計算で足首を元の位置に固定します。
- **着席・つまみ**：太ももを水平にしてすねを揺らす座り姿勢と、つまみ上げられて振り子のように揺れる姿勢です。
- **確認**：`measureContact` が VMD を libmmd で再生し、接地中の足の速さと、床より下に出た頂点の深さを測ります。自動テストでは合成した脚付きモデルで同じ値を確認します。

うかでの結果（同じ測り方）：歩行の足の滑りは平均 369% → 0.9%（最大値は足が床を離れる瞬間の 1 サンプル）、床より下の頂点は 0.20 → 0.001、待機の足の沈みは 0.10 → 0、足のずれは 0.05 → 0 になりました。
