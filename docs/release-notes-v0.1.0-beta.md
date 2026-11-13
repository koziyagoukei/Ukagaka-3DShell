# ukagaka-3d-shell v0.1.0-beta（リリースノート案）

伺か（SSP）のゴーストを MMD の PMX モデルで 3D 表示するランタイムの、最初のベータ版です。
無料・非営利のオープンソースプロジェクトで、ライセンスは MIT です。

> ベータ版です。作者の環境（Windows 11、内蔵 GPU 1 台、SSP 2.9.33、YAYA 6.8.3）で確認した範囲で公開します。
> ほかの環境やモデルでは、不具合が出る可能性があります。

## ダウンロード

| ファイル | 内容 | 対象 |
|---|---|---|
| `ukagaka3d-0.1.0-beta-sample-uka.nar` | サンプルゴースト（CG-CA Uka）。SSP にドラッグ＆ドロップしてインストール | すぐに試したい人 |
| `ukagaka3d-0.1.0-beta-ghost-template.zip` | モデルを除いたゴースト一式、作者向けマニュアル、API リファレンス、モーション生成ツール | ゴースト作者 |
| `ukagaka3d-0.1.0-beta-runtime.zip` | 3D ランタイム、SAORI、YAYA 用の関数辞書、モーション生成ツール | 既存のゴーストに組み込む人 |
| `SHA256SUMS.txt` | 上のファイルの SHA-256 | |

VC++ 再頒布可能パッケージなど、追加のインストールは不要です。SSP は別途用意してください。

## 主な機能

- PMX 2.0／2.1 を背景が透けるウィンドウで表示。VMD、表情、IK、MMD の物理演算
- SAORI 経由で YAYA 6 の辞書からモーション・表情・移動を操作
- クリック・ダブルクリック・なで・長押しドラッグを、部位名つきのイベントとしてゴーストに通知
- デスクトップ歩行、窓の上端への着席（窓の移動に追従）、自律行動、DPI の異なる画面をまたぐ移動
- モデルに合わせた待機・歩行・着席・つまみモーションの自動生成（`uka3d_motiongen`）

## 確認済みと未確認

- **確認済み**：NAR のインストール、起動・会話・メニュー・終了・再起動、メニューからのゴースト切り替え、モーションの切り替え、クリック・ダブルクリック・なで・ドラッグ（OS の入力経路を通した自動操作）、歩行、着席と追従、拡大率 150% と 125% の 2 画面、30／60／120fps、長時間の常駐
- **未確認**：人の手による操作感、里々からの利用、Windows 10・ほかの GPU・まっさらな Windows、上下や 3 画面以上の画面配置、作者が試していない PMX モデル

詳しくは README の「対応状況」と「既知の制限」をご覧ください。

## クレジット

- サンプルのモデル：CG-CA Uka (c) 2023-2024 by Nagoya Institute of Technology, Moonshot R&D Goal 1 Avatar Symbiotic Society（CC BY 4.0、https://github.com/mmdagent-ex/uka）。テクスチャを 2048×2048 に縮小しています
- YAYA（BSD 3-Clause）、libmmd（MIT）、Bullet Physics（zlib）、nlohmann/json（MIT）、stb（MIT）
- このプロジェクトは非公式のものです

## 不具合の報告

GitHub の Issues に、Windows のバージョン、GPU、画面の拡大率、SSP と SHIORI のバージョン、`ukagaka3d_runtime.log` と `ukagaka3d_saori.log` を添えて報告してください。
