# YAYA 6 サンプルゴーストの導入

`samples/yaya-ghost/` は、SSP 上で 3D キャラクターを動かすための最小のゴーストです。SHIORI は YAYA 6、標準モデルは CG-CA「うか」です。[モデルの配布元](https://github.com/mmdagent-ex/uka/blob/main/README.ja.md)

## できること

- 起動すると 3D ランタイムが立ち上がり、挨拶のモーション（1 回）のあと、待機モーションをループ再生する。バルーンには「こんにちは」とメニューが出る。
- メニュー（バルーンの選択肢）から、笑顔（モーフ「笑い」）、表情を戻す、あいさつ、うなずく、待機、止める、状態表示、終了を選べる。
- 「デスクトップ行動」から歩行・移動停止・着席・立ち上がりを指示できる。自律行動、画面またぎ、常に最前面はそれぞれ ON／OFF を選べる。モデルを長押ししてドラッグすると、つまみモーションが再生される。[操作と座標の仕様](desktop-behavior.md)
- Uka の頭を左クリックすると会話と笑顔、頭をマウスでなでると別の会話が出る。身体をダブルクリックすると会話メニュー、右クリックすると SSP のメニューが開く。
- 終了するときは、別れの挨拶をしてから 3D ランタイムも終了する。

## 構成

```text
ukagaka3d_sample/
├── install.txt
├── ghost/master/
│   ├── descript.txt          shiori,yaya.dll
│   ├── yaya.txt              YAYA の基礎設定（UTF-8）
│   ├── shiori3_mini.dic      最小の SHIORI/3.0 受け口（自作。公式テンプレートは不要）
│   ├── ukagaka3d.dic         3D 操作の共通関数（Play3DMotion など）
│   ├── desktop_behavior.dic  歩行・着席の共通メニュー
│   ├── ghost.dic             イベントとメニュー
│   ├── ukagaka3d.json        SAORI の設定
│   ├── hit-regions.json     Uka の部位・なで設定
│   ├── ukagaka3d_saori.dll   ※組み立て時にコピー（x86）
│   ├── yaya.dll              ※組み立て時にコピー（YAYA 6、BSD-3-Clause）
│   └── 3d/                   ※組み立て時に配置（ukagaka3d.exe、うかの PMX・テクスチャ、うかに合わせて生成した VMD）
└── shell/master/             透明シェル（384×682 の透明な surface0／surface10）
```

## 組み立てと起動

1. `powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Test` で x64 と x86 をビルドし、サンプル用モーションと自動テスト用の合成モデルを生成します。
2. YAYA 6 の `yaya.dll` を用意する。[YAYA のリリースページ](https://github.com/YAYA-shiori/yaya-shiori/releases)から取得し、LICENSE も同じフォルダに置く。
3. 組み立て：

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\package-ghost.ps1 -YayaDll C:\path\to\yaya.dll [-Nar]
   ```

   既定の出力先は `build\ghost\ukagaka3d_sample`（`-Nar` を付けると .nar も作る）です。初回は Uka の固定版を取得して `build/cache/` に保存し、SHA-256 を検証します。組み立て直すときは、`-Output` に新しいパスを指定してください。

   Uka のテクスチャは、メモリを抑えるため既定で 2048×2048 に縮小します（内蔵 GPU で 1,397MB → 432MB）。`-UkaTextureSize 4096` で原寸のまま、`1024` でさらに小さくできます。縮小の内容はモデルのフォルダの `CREDITS.ja.md` に記録します。配布ファイル一式（NAR、ランタイム、テンプレート、SHA-256）は `scripts\make-release.ps1` で作れます。
4. 起動：
   - SSP の `--ghost` オプションでフォルダを直接指定する（SSP のフォルダは変更しない）。

     ```powershell
     ssp.exe --ghost "C:\...\build\ghost\ukagaka3d_sample"
     ```

   - または、.nar を SSP にドラッグ＆ドロップしてインストールする。

### 自分のモデルで試す（ローカル限定）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-ghost.ps1 -YayaDll C:\path\to\yaya.dll `
    -ModelDir "C:\MMD\MyModel" -ModelFile "MyModel.pmx" -Output "C:\...\my_ghost"
```

- モデルはゴーストのフォルダの中（`3d/model/user/`）にコピーされます。SAORI はゴーストのフォルダの外を読めないためです。
- `hit-regions.json` の球の位置と、`ghost.dic` のモーフ名を新しいモデルに合わせます。[モデル差し替えの手順](author-guide/01-models.md)
- 「眼鏡」メニューを使う場合は `ukagaka3d.json` の `accessories` にある `parentBone` と `position` もモデルに合わせます。[アクセサリーの設定](author-guide/08-accessories.md)
- 再配布できないモデルを使った出力は、公開しないでください。

## 辞書から 3D を操作する

`ukagaka3d.dic` の関数は、成功すると 1、失敗すると 0 を返します。失敗の理由は `uk3d.error` に入ります。

YAYA では、変数に代入しない関数呼び出しは「出力候補」になり、トークに混ざってしまいます。そのため、トーク関数の中では必ず変数に代入してください。

```c
OnBoot
{
    if Launch3D() == 0 {
        return "\0\s[0]3D を起動できませんでした：" + uk3d.error + "\e"
    }
    _ok = Play3DMotion("greet", 0, "idle")   // 1 回再生 → idle をループ再生
    _ok = Set3DMorph("笑い", 1.0)
    "\0\s[0]こんにちは。\e"
}
```

| 関数 | 内容 |
|---|---|
| `Launch3D()` / `Launch3D("3d/model/x.pmx")` | 起動 |
| `Load3DModel(path)` | モデルの読み替え |
| `Play3DMotion(name [, loop [, next]])` | モーション。`name` だけなら `3d/motion/name.vmd` |
| `Stop3DMotion()` | 停止（基本姿勢） |
| `Set3DMorph(name [, weight])` / `Reset3DMorph([name])` | 表情 |
| `Call3D("set_morphs", 1, "笑い", 1.0, "にこり", 0.5)` | 複数のモーフを一度に設定 |
| `Status3D()` | `uk3d.value` に running / stopped |
| `Shutdown3D()` | 終了 |

SAORI の詳細な仕様は [ipc-protocol.md](ipc-protocol.md) を参照してください。

## SSP との表示合わせ

- **透明シェル**：surface0 と surface10 は、384×682 の完全に透明な PNG です。2D の絵は表示されません。
- **3D ウィンドウの追従**：`followWindowClass: "Tmainform"` を指定すると、3D ウィンドウは SSP の本体側シェルのウィンドウ（クラス `Tmainform`）に、位置と大きさを毎秒 20 回合わせます。
  - SSP は DPI に合わせてシェルを拡大するため、3D の大きさも合わせて変わります。
  - バルーンは、シェルの `sakura.balloon.offsetx/y` の位置に出るので、3D キャラの横に表示されます。
- **マウス操作**：
  - 3D キャラを左ドラッグすると、SSP のシェルウィンドウごと移動し、3D がそれに追従します。SSP の「ゴースト本体移動」メニューでも動かせます。
  - 右クリックは SSP のメニューを開きます。クリック・なで・ダブルクリックは Direct SSTP でゴーストの辞書に届きます。[イベント名と Reference](author-guide/03-interactions.md)
  - 3D ランタイム自体のメニュー（物理演算や最前面の切り替え）は、Shift＋右クリックで開きます。
- **確認できていないこと**：
  - バルーンの細かい位置は、シェルの大きさと offset の値で決まります。3D モデルの頭の位置と自動で合わせる仕組みはありません（今回の必須条件の対象外）。
  - SSP がシェルを画面の端に吸着させたときの挙動は未検証です。マルチモニターでの歩行と着席追従は [検証記録](progress.md) を参照してください。

## ライセンス

- YAYA：BSD-3-Clause（整備班）。元になった AYA Ver.5 の使用許諾は readme-original.txt にあります。組み立て時に `yaya_LICENSE` などとしてゴーストのフォルダにコピーします。yaya.dll 本体は、このリポジトリには含めていません。
- Uka のモデルとテクスチャは CC BY 4.0 です。パッケージに作者のクレジット、利用ガイドライン（公式の README）、ライセンスへのリンク、改変内容（`CREDITS.ja.md`）を同梱します。[モデルの配布元](https://github.com/mmdagent-ex/uka/blob/main/README.ja.md)
- 辞書（`*.dic`）、透明シェル、生成したモーションは、このプロジェクトのファイルです。
