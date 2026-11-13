# 08 アクセサリーを装着する

アクセサリーは本体とは別の PMX です。本体の PMX を編集せず、帽子や眼鏡などを装着できます。サンプルでは「眼鏡」の「かける」「外す」で試せます。

## PMX と設定の準備

アクセサリーの原点を装着の基準にし、前向きのモデルでは −Z が手前、+Y が上になるように作ります。PMX と参照するテクスチャをまとめて `ghost/master/3d/accessories/` に置きます。

`ghost/master/ukagaka3d.json` のルートオブジェクトに、次の設定を追加します。ほかの設定との間にはカンマを入れてください。

```json
"accessories": [
  {
    "id": "glasses",
    "model": "3d/accessories/glasses.pmx",
    "parentModel": "main",
    "parentBone": "頭",
    "position": [0, 0.4, -1.8],
    "rotation": [0, 0, 0],
    "scale": 1.0,
    "visible": false
  }
],
"saveAccessories": false
```

- `id` は 1～64 文字の英数字、ピリオド、下線、ハイフンで指定します。`main` は本体用です。アクセサリーは最大32個です。
- `model` とオプションの `motion` は `ghost/master` 基準です。単体起動で `--accessory-config` を使う場合は、その JSON のフォルダが基準です。
- `parentModel` は `main` または先に定義したアクセサリー ID です。空文字ならシーンに独立して配置します。`parentBone` は日本語または英語のボーン名です。空文字なら親の原点に従います。
- `position` はボーン原点からの X・Y・Z のずれです。MMD の座標単位で指定します。`rotation` は X・Y・Z 軸の角度（度）で、Z、X、Y の順に適用します。`scale` は正の一様倍率です。初期値はそれぞれ `[0,0,0]`、`[0,0,0]`、`1` です。
- `visible` は既定で `false` です。自身や親アクセサリーが非表示または外れていると描画・姿勢更新を停止します。本体 PMX を差し替えると、本体の装着品は一旦外れます。
- `motion` を指定すると、アクセサリー内のボーンとモーフをその VMD で動かせます。`loop` は既定で `true` です。`physics` は既定で `false` です。有効化した場合はアクセサリー固有のローカル物理を計算します。本体とアクセサリーの剛体間の衝突、衣装全体のスキニング同期は未対応です。

## YAYA で操作する

サンプルの `ukagaka3d.dic` を読み込み、`Launch3D()` の成功後に呼び出します。

```c
OnMyGlassesOn
{
    if Attach3DAccessory("glasses") == 0 {
        return "\0装着できませんでした。" + uk3d.error + "\e"
    }
    _ok = Play3DMotion("nod", 0, "idle")
    "\0眼鏡をかけました。\e"
}

OnMyGlassesOff
{
    _ok = Detach3DAccessory("glasses")
    "\0眼鏡を外しました。\e"
}
```

| ヘルパー | 用途 |
|---|---|
| `Load3DAccessory(id, json)` | PMX を追加。JSON は設定の1要素と同じキー（`id` は第1引数） |
| `Attach3DAccessory(id [, parentModel [, parentBone]])` | 装着して表示。空の引数は既定の装着先を使用し、`-` は指定を解除 |
| `Detach3DAccessory(id)` | 装着を解除して非表示。PMX は保持 |
| `Unload3DAccessory(id)` | 登録とリソースを解放。直下の子も外す |
| `Set3DAccessoryVisible(id, 1 または 0)` | 装着したまま表示を切り替え |
| `Set3DAccessoryTransform(id, json)` | 位置・回転・倍率のうち、JSON に含まれる値だけ変更 |
| `Get3DAccessoryStatus(id)` | `uk3d.value` に `visible`、`hidden`、`detached` のいずれか |
| `List3DAccessories()` | `uk3d.detail` にアクセサリー一覧の JSON |

例えば `_ok = Set3DAccessoryTransform("glasses", '{"position":[0,0.45,-1.8],"scale":1.05}')` で装着位置と倍率を調整できます。会話中だけ持たせる場合はモーション開始時に表示し、会話・動作の終了を扱う辞書イベントで非表示にします。

## 保存・描画と配布

`saveAccessories` を `true` にすると、設定と同じフォルダに `ukagaka3d_accessories_state.json` を作り、装着・表示と変換を保存します。次回は JSON に定義済みの同じ ID に反映します。装着先と、実行中に追加した PMX の登録は作者の設定で管理します。調整をやり直すときは保存ファイルを退避してください。

同じ PMX の静的な GPU メッシュと、同じパスのテクスチャを共有します。姿勢・動的頂点バッファ・物理はモデルごとに確保します。動かす必要のないアクセサリーは静的な小さい PMX にし、再装着しないものは登録を解放してください。半透明の描画順はモデルの奥行き中心を基準とするため、大きく交差する半透明面では順序が合わない場合があります。

表示されないときは、`ukagaka3d_runtime.log` で PMX パスとボーン名を確認し、`Get3DAccessoryStatus` で実効表示を確認します。表面に埋まる場合は `position` の Z と `scale` を調整してください。アクセサリー上からも本体の操作を判定します。アクセサリーだけが映る部分はクリックを透過します。

PMX と付属テクスチャの利用条件を素材ごとに確認してください。サンプルの眼鏡は本プロジェクトが生成する合成 PMX です。Uka のクレジットは既存の配布物に同梱されます。
