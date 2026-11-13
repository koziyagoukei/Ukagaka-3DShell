# 通信仕様（SAORI と IPC）

ukagaka3d は 2 段構成になっています。

```text
SSP（32bit）
 └─ SHIORI（YAYA など）
     └─ ukagaka3d_saori.dll（SAORI-universal、x86）   … 第1層：SAORI/1.0
          │  名前付きパイプ \\.\pipe\ukagaka3d.<インスタンスID>
          ▼                                         … 第2層：IPC プロトコル v1（JSON）
        ukagaka3d.exe（3D ランタイム、x64、別プロセス）
```

- 第2層（IPC）は SHIORI に依存しません。今後の GUI や、YAYA 以外の SHIORI からも同じプロトコルで制御できます。
- SAORI DLL は SSP（32bit）のプロセスに読み込まれるため、x86 でビルドします。3D ランタイムは x64 の別プロセスです。

---

## 1. SAORI 層（ukagaka3d_saori.dll）

デスクトップ歩行、着席、画面またぎ、自律行動の追加コマンドは [デスクトップ行動](desktop-behavior.md) に記載しています。既存の通信形式とプロトコル v1 を使います。

### 1.1 エクスポート関数

| 関数 | 内容 |
|---|---|
| `BOOL loadu(HGLOBAL h, long len)` | UTF-8 のパスで読み込む（YAYA は `load` よりこちらを優先して呼ぶ）。h は DLL 側で解放する |
| `BOOL load(HGLOBAL h, long len)` | ANSI のパス（旧仕様）。動作は `loadu` と同じ |
| `BOOL unload()` | このSAORIが起動した 3D ランタイムを終了する（`shutdownOnUnload`） |
| `HGLOBAL request(HGLOBAL h, long *len)` | SAORI/1.0 の要求を処理する。応答は GMEM_FIXED で確保し、呼び出し側が解放する |

呼び出し規約は `__cdecl`、名前は `.def` ファイルで装飾なしにしています。CRT は静的リンク（/MT）なので、SSP 側に VC++ ランタイムは不要です（依存 DLL は KERNEL32 だけ）。

DLL のインターフェースは [UKADOC の DLL 仕様](https://ukatech.shillest.net/ukadoc-spec/spec/dll/) を参照してください。サンプル辞書では YAYA の `LOADLIB` と `REQUESTLIB` で DLL を読み込み、SAORI/1.0 の要求を送ります。[YAYA の SAORI ドキュメント](https://github.com/YAYA-shiori/yaya-docs/blob/main/other/saori-usage.md)

### 1.2 要求・応答の形式

```text
EXECUTE SAORI/1.0
Charset: UTF-8
Sender: ゴースト名
SecurityLevel: Local
Argument0: play_motion
Argument1: greet
Argument2: 0
Argument3: idle

```

```text
SAORI/1.0 200 OK
Charset: UTF-8
Result: 1
Value0: OK
Value1: {"model":{...},"motion":{...},...}

```

- **Charset**：`UTF-8` か `Shift_JIS`（CP932）に対応します。応答は要求と同じ文字コードで返します。Charset ヘッダーがない場合は、UTF-8 として正しく読めれば UTF-8、読めなければ Shift_JIS とみなします。
- **SecurityLevel**：ヘッダーで `Local` 以外が指定されていれば `400 Bad Request` で拒否します。省略した要求も受け付けます。
- **GET Version SAORI/1.0**：`200 OK`。Result にバージョン（例：`ukagaka3d_saori/0.1.1-beta`）を返します。
- **要求行が不正なとき**：`400 Bad Request`（Result に理由）。
- **Result の約束事**：コマンドが成功すれば `1`、失敗すれば `0` です。
  - `Value0`：メッセージ、または主な値
  - `Value1`：失敗時はエラーコード、成功時は詳細の JSON
- Value の中の CR／LF は空白に置き換えます（ヘッダー形式が壊れないようにするため）。

### 1.3 コマンド（Argument0）

| コマンド | 引数 | 内容 |
|---|---|---|
| `launch` | [モデル] | 3D ランタイムを起動して接続する。モデルを省略すると `ukagaka3d.json` の `model` を使う。起動済みなら `Value0=already_running` |
| `status` | — | `Value0` に状態（`running` / `stopped` / `starting` / `unresponsive`）、`Value1` に状態の JSON |
| `load_model` | モデル | PMX を読み替える。失敗したときは前のモデルのまま |
| `configure_face` | [まばたき 1/0] [視線 1/0] [反応距離 px] [まばたきのモーフ] | 自動まばたきと視線追従の設定。空の引数は変更なし |
| `play_motion` | モーション [loop] [次のモーション] [カメラ] | VMD を再生する。loop は `1`（既定）か `0`。`0` で次のモーションを指定すると、再生が終わったあとそれをループ再生する。カメラは省略時、VMD と同じ名前の `.camera.json` があればそれを使う。`none` でカメラを動かさない。ファイルを指定することもできる |
| `stop_motion` | — | モーションを止めて基本姿勢にする（物理演算は続く） |
| `set_morph` | モーフ名 [重み] | 表情モーフの重みを 0〜1 で設定する（既定 1.0） |
| `set_morphs` | reset 名前 重み [名前 重み …] | 複数のモーフを一度に設定する。reset が `1` なら先にすべて戻す。モデルにないモーフは無視する（`missingMorphs` に記録） |
| `reset_morph` | [モーフ名] | 1つ、または全部の上書きを解除する |
| `shutdown` | — | 3D ランタイムを終了する（3 秒以内に終わらなければ強制終了） |
| `ping` / `version` | — | 疎通確認／バージョン |

**パスの扱い**

- 相対パスは、ゴーストのフォルダ（DLL があるフォルダ、通常は `ghost/master`）を基準に解決します。
- 拡張子もフォルダもない名前（例：`greet`）は、モーション用フォルダの `<motionDir>/greet.vmd` を指します。
- 解決後のパスは、**ゴーストのフォルダ**（と `ukagaka3d.json` の `allowedRoots`）の中に限ります。
  - `..` による脱出、別ドライブの絶対パス、UNC パス（`\\server\…`）、代替データストリーム（`:`）は拒否します（`forbidden_path`）。
  - シンボリックリンクやジャンクションは、実体のパスに解決してから判定します。
- 3D ランタイム側でも、`--allowed-root` を使って同じ判定をもう一度行います（二重チェック）。

### 1.4 設定ファイル（ghost/master/ukagaka3d.json）

コメントを書けます。どの項目も省略可能です。

```jsonc
{
    "runtime": "3d/ukagaka3d.exe",          // 3D ランタイム
    "model": "3d/model/xxx.pmx",            // launch の既定モデル
    "motionDir": "3d/motion",               // モーション名を探すフォルダ
    "allowedRoots": [],                     // ゴーストのフォルダ以外に許可するフォルダ
    "window": { "width": 384, "height": 682, "transparent": true, "topmost": false,
                "followWindowClass": "Tmainform" },   // SSP の本体側シェルに追従する
    "camera": { "fit": "height", "zoom": 1.0 },       // height = 身長に合わせて大きく表示
    "fps": 60,
    "physics": true,
    "timeouts": { "launchMs": 5000, "requestMs": 3000, "loadMs": 10000 },
    "shutdownOnUnload": true
}
```

### 1.5 SSP の UI を止めないための工夫

SAORI は SSP の UI スレッドから同期的に呼ばれます。そこで、すべての待ち時間に上限を設けています。

| 処理 | 上限 |
|---|---|
| 起動から接続まで | `launchMs`（既定 5 秒） |
| 通常のコマンド | `requestMs`（既定 3 秒） |
| モデル・モーションの読み込み | `loadMs`（既定 10 秒） |

- 計測値：起動は約 170ms、通常のコマンドは数 ms です。
- ランタイムが落ちているときは、パイプに触る前にプロセスの終了を検知してすぐに返します（1.5 秒未満であることをテスト済み）。
- 応答がタイムアウトしたときは接続を閉じます（遅れて届いた応答と、次の要求の対応がずれるのを防ぐため）。

### 1.6 インスタンスID

`ghost-<ゴーストのフォルダの FNV-1a ハッシュ>-<SSP のプロセスID>` です。

- ゴーストごと、SSP ごとに別のパイプ名になるため、複数のゴーストを同時に起動してもぶつかりません。
- 同じ ID のランタイムを2つ起動しようとすると、後から起動した方はパイプを作れず、終了コード 5 で終了します。

---

## 2. IPC 層（プロトコル v1）

### 2.1 トランスポート

- 名前付きパイプ `\\.\pipe\ukagaka3d.<インスタンスID>`。ID は `[A-Za-z0-9._-]` の 1〜64 文字です。
- バイトモード。サーバー（3D ランタイム）は1インスタンスで、一度に1クライアントだけ受け付けます。切断されたら、次の接続を待ちます。
- 1 メッセージの形式は、4 バイトのリトルエンディアンの長さに、UTF-8 の JSON 本体が続くものです。上限は 1 MiB です。
- 不正な JSON や不正な形式を受け取っても接続は切らず、エラー応答を返します。上限を超えた長さのときだけ切断します。

### 2.2 アクセス制御

| 対策 | 内容 |
|---|---|
| DACL | 現在のユーザーにだけ GENERIC_ALL を与える保護付き DACL（`D:P(A;;GA;;;<ユーザーSID>)`） |
| リモート拒否 | `PIPE_REJECT_REMOTE_CLIENTS`。ネットワーク越しの接続を拒否する |
| 名前の乗っ取り防止 | `FILE_FLAG_FIRST_PIPE_INSTANCE`。既にある名前ではサーバーを作らない |
| 接続元の限定 | `--parent-pid` を指定すると、そのプロセス（SSP）からの接続だけを受け付ける（`GetNamedPipeClientProcessId`）。テストで確認済み |
| サーバーの確認 | クライアント側は `GetNamedPipeServerProcessId` で、自分が起動したプロセスかを確認する。偽のサーバーを拒否する |
| 偽装の制限 | クライアントは `SECURITY_IDENTIFICATION` で接続する。サーバーはクライアントを識別できるが、なりすますことはできない |
| 孤児化の防止 | 親プロセス（SSP）が終了すると、ランタイムも自動で終了する（約 60〜80ms で検知） |

### 2.3 メッセージ

要求：

```json
{"version":1,"requestId":"1001","command":"play_motion","params":{"path":"C:\\...\\greet.vmd","loop":false,"next":{"path":"C:\\...\\idle.vmd","loop":true}}}
```

成功：

```json
{"version":1,"requestId":"1001","success":true,"result":{ ... }}
```

失敗：

```json
{"version":1,"requestId":"1001","success":false,"error":{"code":"forbidden_path","message":"..."}}
```

- `requestId`：1〜128 文字の文字列。応答に同じ値が返る。
- `version`：1 以外は `unsupported_version` になる。

### 2.4 コマンド

| command | params | result |
|---|---|---|
| `ping` | — | `{"pong":true,"pid":..}` |
| `status` | — | 状態の JSON（下記） |
| `load_model` | `path` | 状態 |
| `load_accessory` | `id`, `model`。ほかのキーは [作者向け設定](author-guide/08-accessories.md) と同じ | アクセサリー状態 |
| `attach_accessory` | `id`, 任意の `parentModel`, `parentBone` | アクセサリー状態 |
| `detach_accessory` | `id` | アクセサリー状態 |
| `unload_accessory` | `id` | 空オブジェクト |
| `set_accessory_visible` | `id`, `visible` (bool) | アクセサリー状態 |
| `set_accessory_transform` | `id`, 任意の `position`, `rotation`, `scale` | アクセサリー状態 |
| `get_accessory_status` | `id` | アクセサリー状態 |
| `list_accessories` | — | `{"accessories":[...]}` |
| `play_motion` | `path`, `loop`（既定 true）, `next`（{path, loop, camera}）, `camera`（パスか false。省略時は隣の `.camera.json`） | 状態（`motion.camera` に使用中のカメラトラック） |
| `configure_face` | `blink`（bool）, `gaze`（bool）, `gazeRadius`（96dpi px、0 = 自動）, `blinkMorph`（モーフ名）。省略した項目は変更なし | `face`（blink, blinkMorph, gaze, gazeRadius, gazeWeight） |
| `stop_motion` | — | 状態 |
| `set_morph` | `name`, `weight`（既定 1） | 状態 |
| `set_morphs` | `weights`（{名前: 重み}）, `reset`（既定 true） | 状態と `missingMorphs` |
| `reset_morph` | `name`（省略するとすべて） | 状態 |
| `shutdown` | — | `{"shuttingDown":true}`（応答を返してから終了する） |

状態の JSON：

```json
{"pid":1234,"instanceId":"ghost-...","model":{"name":"..","path":"..","vertices":..,"morphs":..,"rigidBodies":..},
 "motion":{"path":"..","frame":12.5,"lastFrame":90,"loop":false,"finished":false,"next":".."} | null,
 "paused":false,"physics":{"enabled":true,"status":"有効"},"fps":{"target":60,"measured":59.9},
 "morphOverrides":{"笑い":1.0},"transparent":true}
```

アクセサリー状態には `id`、`model`、`parentModel`、`parentBone`、`attached`、`visible`、`effectiveVisible`、`position`、`rotation`、`scale`、`physics`、`motion`、`loop` が入ります。`load_accessory` のパスは設定ファイルの場所、または `--allowed-root` の先頭を基準に解決します。

SAORI では `load_accessory` と `set_accessory_transform` に `id` と JSON オブジェクトを文字列で渡します。`attach_accessory` は `id [parentModel] [parentBone]`（空欄は維持、`-` は解除）、`set_accessory_visible` は `id 1/0` を取ります。`detach_accessory`、`unload_accessory`、`get_accessory_status` は `id` を取り、`list_accessories` は引数なしです。照会の JSON は `Value1`、`get_accessory_status` の要約は `Value0`（`visible` / `hidden` / `detached`）に入ります。

### 2.5 エラーコード

| code | 意味 |
|---|---|
| `invalid_json` | JSON として解釈できない |
| `invalid_request` | 形式が不正（version／requestId／command がない、params がオブジェクトでない、メッセージが大きすぎる、許可されていないクライアント） |
| `unsupported_version` | プロトコルのバージョンが違う |
| `unknown_command` | 未知のコマンド |
| `invalid_params` | パラメーターが不正 |
| `not_found` | ファイルやモーフが見つからない |
| `forbidden_path` | 許可されたフォルダの外 |
| `load_failed` | PMX／VMD の読み込みに失敗した（壊れている、形式が違うなど） |
| `timeout` | 応答がタイムアウトした |
| `not_running` | ランタイムが動いていない、または接続が切れた |
| `io_error` / `internal` | 通信エラー／内部エラー |

### 2.6 スレッドの扱い

- パイプの送受信は、ランタイムの専用スレッドで行います。
- 受け取った要求はキューに入れ、`PostMessage`（`WM_APP+1`）で UI スレッドに知らせます。
- UI スレッドはウィンドウプロシージャの中でコマンドを実行し、結果を `std::promise` で返します。描画と同じスレッドで処理するので、モデルや物理の状態とぶつかることはありません。
- ウィンドウのドラッグ中やメニュー表示中（モーダルループ中）でも、メッセージは配送されるので処理は進みます。
- UI スレッドの応答待ちは最大 20 秒です。終了時は、受け付けを止めて残りの要求に応答してから、パイプを閉じます。

### 2.7 今後の拡張

- 新しいコマンドは、`command` 名を追加するだけで、後方互換を保ったまま増やせます。
- 互換性のない変更をするときは、`version` を上げます。
- 3D 側からゴーストへのイベント通知（クリックした部位など）は、今回の対象外です。今後は、同じパイプにサーバーからの通知メッセージ（`requestId` なし、`event` フィールドあり）を加える形を想定しています。
