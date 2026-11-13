# 里々ゴーストの会話を YAYA に移植する（tools/satori2yaya.py）

既存の里々（Satori）ゴーストの会話データを YAYA の辞書に変換し、ukagaka3d の 3D 連携と組み合わせるためのツールです。

## 使い方

```powershell
python tools\satori2yaya.py <里々ゴースト>\ghost\master out\satori_talk.dic `
    --reserve OnBoot,OnFirstBoot,OnClose,OnGhostChanged `
    --skip homeurl,sakura.portalsites --report out\report.txt
```

- 入力：`dic*.txt` と `satori_conf.txt`（Shift_JIS）。出力：UTF-8 の YAYA 辞書。
- `--reserve`：指定したイベントは関数を生成しません。3D の起動や終了の処理と組み合わせるため、自分で書いた辞書から `SatoriCall("OnBoot")` のように呼びます。なお、里々の OnBoot は `＊起動` に対応させています。
- `--skip`：除外する見出しや単語群。たとえば `homeurl` は、元の 2D 版のネットワーク更新先なので外します（3D 版が 2D 版で上書きされるのを防ぐため）。
- `--wait-scale`：自動で入れるウェイトの倍率（0 で入れない）。

生成した辞書を使うには、`reference` 配列を用意して、イベントIDと同じ名前の関数を呼ぶ SHIORI の受け口が必要です。`samples/yaya-ghost/ghost/master/shiori3_mini.dic` を使えます。

## 対応している構文

| 里々 | 変換後 |
|---|---|
| `＊名前`（同じ名前が複数） | `Satori_名前` 関数（候補の中からランダムに選ぶ） |
| `＊`（名前なし） | `Satori_RandomTalk`（`nonoverlap`：一巡するまで同じものは出ない） |
| `：` | 話者の切り替え（`\0` ↔ `\1`。2回目以降はスコープ切り替え時の文字列を挟む） |
| `：` のない行 | 同じ話者のまま改行（`\n`） |
| `（数字）` | `\s[n]` |
| `（Ｒn）` | `reference[n]` |
| `（現在月）（現在日）（現在曜日）` など | `GETTIME` から取得 |
| `（ユーザ名）` などの変数 | `satori.var.名前` → `satori_conf.txt` の `＄` の初期値 |
| `＞見出し【タブ】条件` | `if 条件 { return SatoriCall("見出し") }`。`＝＝ ＆＆ ｜｜` などの全角の演算子も変換する |
| `＞（２）セリフ` など、存在しない見出しへのジャンプ | セリフとして変換する（レポートに記録） |
| `＃＊見出し`（見出しごとコメントアウト） | その本文も無視する |
| `＠単語群` | `Satori_Word_名前`。英数字だけの名前なら SHIORI のリソース（`username` など）にもする |
| 会話時サーフェス戻し | 各トークの先頭に `\0\s[0]` を付ける |
| 自動挿入ウェイト | 「、」→ `\w3`、「。！？」→ `\w6`（倍率は変更可） |
| 喋り間隔 | `Satori_TalkInterval()`（ランダムトークの間隔。呼び出し側で使う） |

## 対応していないもの（必要に応じて手で書く）

- 里々の組み込み関数全般（`compare_head` などの文字列関数、`＄` による実行時の代入、`（乱数）`、SAORI の呼び出し）
- なでられ／つつかれ／ころころ（当たり判定に連動するマウス反応）。見出し（`Satori_0atamaなでられ` など）は生成しますが、呼び出しは配線されていません。
- 時報、見切れ、重なりなどの自動イベント
- 変数の保存と復元（YAYA のグローバル変数として保存されます）

変換で問題があった箇所は、`--report` で出力されるレポートで確認できます。

## 3D 連携の例（表情番号 → モーフ）

トークの `\s[n]` に合わせて、SSP が `OnSurfaceChange` を送ってきます。これを使って、2D シェルの表情番号を 3D のモーフに対応させられます。

```c
OnSurfaceChange
{
    _s = TOINT(reference[0])
    if _s == 6 {
        _ok = Call3D("set_morphs", 1, "笑い", 1.0, "にこり", 0.6)
    }
    elseif _s == 0 {
        _ok = Call3D("reset_morph")
    }
}
```

透明シェルには、元のシェルと同じ番号の透明なサーフェスを用意してください。用意していない番号では、`OnSurfaceChange` が来ない場合があります。
