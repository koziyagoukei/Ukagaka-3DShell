# 04 SHIORI とつなぐ

## YAYA 6

サンプルを土台にする場合は `ghost/master` に `yaya.dll`、`ukagaka3d_saori.dll`、`ukagaka3d.json` を置きます。サンプルの `descript.txt` は `shiori,yaya.dll` を指定しています。`yaya.txt` に次の登録があることを確認します。

```text
dic, shiori3_mini.dic
dic, ukagaka3d.dic
dic, desktop_behavior.dic
dic, ghost.dic
```

既存の YAYA ゴーストへ組み込む場合は、現在使っている SHIORI の受け口を維持してください。`shiori3_mini.dic` はサンプル専用です。`ukagaka3d.dic` を登録し、辞書に `CRLF` が未定義の場合は次の関数を足します。既存テンプレートで Reference の取り出し方が異なる場合は、そのテンプレートに合わせます。

```c
CRLF
{
    CHR(13, 10)
}
```

起動イベントで `Launch3D()`、SHIORI の終了処理で `_ok = Shutdown3D()` を呼びます。既存の `OnBoot` や `unload` には処理を追記してください。サンプルでは SAORI のロードと要求文の組み立てを `Call3D` が担当します。操作は成功時に 1、失敗時に 0 を返します。理由は `uk3d.error` です。関数の戻り値を会話に混ぜないよう、`_ok` などに代入します。

### 会話と動きを合わせる

イベントが来た時点でモーションや表情を指定し、その後に会話を返すと、動きとバルーンを同じ反応として出せます。この例はサンプルの `OnMouseDoubleClick` と置き換えて試せます。

```c
OnMouseDoubleClick
{
    if reference[3] == "0" && reference[4] == "Body" {
        _ok = Play3DMotion("greet", 0, "idle")
        return "\0こんにちは。\e"
    }
    return ""
}
```

動作指示は名前付きパイプ、3D からのイベントは SSP の Direct SSTP を使います。ローカルの送信先は起動中のゴーストのフォルダから選ばれます。イベント名と値は [クリック・なで・ドラッグ](03-interactions.md)、要求と応答の詳細は [開発者向け IPC 仕様](../ipc-protocol.md) に記載しています。

## 里々

里々での起動、操作、イベント受信は実機未検証です。導入を試す場合は、里々の SAORI 登録に DLL のパスと呼び出し名を設定し、最初の引数を `launch`、再生時は `play_motion` と VMD の名前にします。終了時のコマンドは `shutdown` です。使用中の里々の版に合う登録方法と戻り値の扱いを確認してください。[里々公式仕様書](https://ukatech.github.io/satori-docs/) · [SAORI コマンド一覧](../ipc-protocol.md)

受信の確認では、里々の辞書に `OnMouseClick` または `OnUk3dPet` のトークを設けて、里々の Reference 変数 `Ｒ４` に判定名が届くか調べます。サンプルの YAYA 辞書へ移す方法もあります。[里々から YAYA への会話変換](../satori-to-yaya.md)
