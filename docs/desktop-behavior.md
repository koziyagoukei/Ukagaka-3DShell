# デスクトップ行動

SSP サンプルのバルーンで「デスクトップ行動」を選びます。「歩く」は今いる画面内を約 240 ピクセル移動し、端に達した次の歩行では向きを変えます。「手前の窓に座る」は、先に着席先のアプリを手前にしてから選びます。ゴーストのメニューにフォーカスが移った場合は、直前に選択した対象を使います。

自律行動と画面またぎは起動ごとに OFF です。画面またぎを ON にすると「別の画面へ歩く」が使え、自律歩行も別画面を選べます。着席中は対象ウィンドウが画面を移動すると、その画面に追従します。

## 座標と描画

- 歩行の座標は Win32 の仮想デスクトップ上の物理ピクセルです。移動速度は px/s で、実測経過時間を使って進みます。到着地点は作業領域内に収め、画面をまたぐ歩行は到着地点まで直進します。VMD の歩行中も既存の固定ステップ物理を使います。
- 歩行中は進行方向に合わせてモデルの見える向きを変えます。既定ではセンターまたはルートボーンの水平移動をカメラで相殺します。`useRootMotion` を ON にすると VMD のモデル内の水平移動も表示します。どちらの設定でもウィンドウの移動距離は画面座標と指定速度で決まります。
- 着席では `EnumWindows` で候補を取得し、不可視、最小化、DWM でクロークされたもの、小さいもの、システム UI、SSP、3D ウィンドウを除きます。DWM の可視フレームを使い、取得できない場合は `GetWindowRect` を使います。[EnumWindows](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-enumwindows) · [DWM フレーム属性](https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
- 着席の基準は「座面に触れる体の表面」です。モデルを読み込んだときに、骨盤（`下半身`）と太ももの付け根側半分（`左足`・`右足`、準標準モデルの `左足D`・`右足D`）に付いている頂点から代表点を選んでおきます。実行中は代表点だけを現在の姿勢で変換し、画面上でいちばん低く見える点をフレーム上端に合わせます。左右の位置は両太ももの付け根の中点です。これらのボーンがないモデルでは、`下半身`→`腰`→`センター` の関節の高さを使います。
- 衣装が体より下に広がるモデルなどは、[`configure_seat`](#着席位置の調整) でモデルごとに補正できます。`sit_on_window` に `anchorBone` を指定した場合は、従来どおりそのボーンの関節の高さを基準にします。
- 3D ウィンドウは整数ピクセル単位で対象に追従し、端数は描画時に投影をずらして合わせます。物理 tick 間の補間にも同じ値を使うため、座ったまま体が動くモーションでも、お尻が上端から浮いたり揺れたりしません。上端が画面の作業領域に近い場合は、キャラクターが見える位置に収めます。
- 着席と立ち上がりでは、姿勢を切り替え前のポーズからボーン単位でクロスフェードし、3D ウィンドウは弧を描いて移動します（既定 350ms、`transitionMs` で変更、0 で瞬時）。歩行・待機・つまみの切り替えも 250ms でクロスフェードします。
- 着席対象の移動、サイズ変更、最小化、終了は `SetWinEventHook` で受け取り、250ms ごとの確認で補います。対象を見失うか最小化したら着席前の位置に戻ります。DPI が変わったら `WM_DPICHANGED` の推奨サイズで 3D ウィンドウを更新します。[SetWinEventHook](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwineventhook) · [WM_DPICHANGED](https://learn.microsoft.com/windows/win32/hidpi/wm-dpichanged)
- 着席先かゴーストにフォーカスがある間はキャラクターをその上に表示します。ほかのアプリに切り替えても表示を保つ場合は、バルーンの「常に最前面」または 3D ウィンドウの Shift＋右クリックメニューを使います。歩行中のバルーンは出発位置に留まるため「移動停止」を選べます。到着時と着席中はキャラクターに追従します。
- サンプルではモデル部分を左クリックで 180ms 長押しすると `grab.vmd` を再生します。そのままドラッグでき、ボタンを離すかキャプチャが解除されると待機モーションに戻ります。短いクリックではつまみモーションが始まりません。

## IPC と SAORI

従来の IPC v1 エンベロープを使います。デスクトップ行動は背景透過モードで使えます。

| コマンド | IPC の `params` | SAORI の引数（コマンド名の後） |
|---|---|---|
| `move_to` | `x`, `y` または `nextMonitor: true`。省略時は画面内の短い横歩き | [x] [y] [speed] [walk] [idle] [useRootMotion] [nextMonitor] |
| `stop_move` | なし。歩行を止めて待機 | — |
| `sit_on_window` | `target`（`foreground` または列挙した ID）、`position`（`top-left`／`top-center`／`top-right`）、`motion` または `sitMotion`、`idleMotion`、`anchorBone`、`anchorOffsetY` | [target] [position] [sit] [idle] [anchorBone] [anchorOffsetY] |
| `stand_up` | なし。着席前の位置に戻って待機 | — |
| `set_auto_behavior` | `enabled`（必須）、`waitSeconds`、`speed`、`walkMotion`、`idleMotion`、`sitMotion`、`sitChance`、`useRootMotion` | enabled [waitSeconds] [speed] [walk] [idle] [sit] [sitChance] [useRootMotion] |
| `set_cross_monitor` | `enabled`（必須） | enabled |
| `set_topmost` | `enabled`（必須）。常に最前面を切り替える | enabled |
| `configure_seat` | `anchorBone`、`contactBones`（配列）、`useContact`、`offsetX`、`offsetY`、`yaw`、`inset`、`transitionMs`、`reset`。省略した項目は現在の値のまま | [anchorBone] [offsetX] [offsetY] [yaw] [inset] [transitionMs] [contactBones] [useContact] [reset] |
| `configure_grab` | `motion`、`idleMotion`、`holdMs`（既定 180、80〜1000ms）。`motion` を省略すると無効 | [motion] [idle] [holdMs] |
| `get_behavior_status` | なし。状態、座標、作業領域、DPI、歩行先または着席先を返す | — |
| `list_windows` | なし。選択可能な `target`、タイトル、PID を返す | — |

`move_to` は `speed`（既定 180、範囲 1〜3000 px/s）、`walkMotion`、`idleMotion`、`useRootMotion` も受け取ります。自律行動の設定値は、以降の指示で省略したときの既定値にもなります。座標は 3D ウィンドウの左上を表し、左側や上側のディスプレイでは負になることがあります。VMD を省略した場合や許可フォルダ内にファイルがなかった場合は基本姿勢で移動・着席します。SAORI は短いモーション名を設定済みのモーションフォルダから解決します。

`get_behavior_status` の状態は `Idle`、`Walking`、`Sitting`、`Transition` です。SAORI は状態名を `Value0`、詳細 JSON を `Value1` に返します。他の行動コマンドも成功時に `Value1` で結果を返します。

## 着席位置の調整

`configure_seat` はモデルごとの着席位置を設定します。着席中に呼ぶとその場で反映されるので、座らせたまま値を変えて合わせられます。設定はランタイムが終了するまで保たれ、モデルを読み替えても引き継ぎます。

| 項目 | 既定 | 内容 |
|---|---|---|
| `anchorBone` | 空 | 左右位置の基準ボーン。空なら両太ももの付け根の中点 |
| `contactBones` | 空 | 座面に触れるボーン名の配列。空なら骨盤と太ももの付け根側半分。スカートの裾など、体より下に出る部分のボーンを足せる |
| `useContact` | true | false にすると `anchorBone` の関節の高さだけを使う |
| `offsetX` / `offsetY` | 0 | モデル座標での補正。`offsetY` を増やすと浮き、減らすと沈む。`offsetX` を増やすと画面の右へ寄る |
| `yaw` | 20 | 着席中の体の向き（度） |
| `inset` | 40 | `top-left`／`top-right` で窓の角から離す距離（96dpi でのピクセル） |
| `transitionMs` | 350 | 着席・立ち上がりの遷移時間 |
| `reset` | false | true で既定値に戻してから、ほかの項目を適用する |

応答には、実際に使われる方式（`mode`：`contact`／`bone`／`none`）、基準、接地ボーン、代表点の数が入ります。SAORI では空の引数が「変更なし」、`-` が「空にする」です。`ukagaka3d.json` に `"seat": { ... }` を書いておくと、SAORI が起動時に送ります。

```c
// 0.4 だけ浮かせる（サンプルの Uka の設定）
_ok = Configure3DSeat("", "", 0.4)
// 裾のボーンも座面に触れる部分として扱う
_ok = Configure3DSeat("", "", "", "", "", "", "下半身,左足D,右足D,左裾1,右裾1")
```

YAYA からは `Configure3DSeat`、`Move3DTo`、`Move3DToNextMonitor`、`Stop3DMove`、`Sit3DOnWindow`、`Stand3DUp`、`Set3DAutoBehavior`、`Set3DCrossMonitor`、`Set3DTopmost`、`Configure3DGrab`、`Get3DBehaviorStatus`、`List3DWindows` を使用します。

```c
_ok = Move3DTo(200, 400, 180, "walk", "idle", 0)
_ok = Sit3DOnWindow("foreground", "top-right", "sit", "idle", "腰", -0.2)
```
