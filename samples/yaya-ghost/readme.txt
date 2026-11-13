ukagaka-3d-shell サンプルゴースト（うか） v0.1.0-beta
=====================================================

伺か（SSP）で MMD の PMX モデルを 3D 表示するランタイム「ukagaka-3d-shell」の
サンプルゴーストです。ベータ版のため、動作が不安定な場合があります。

■ 動作環境
- Windows 10 / 11（64 ビット）
- SSP（2.9.33 で確認）
- Direct3D 11 に対応した GPU
追加のランタイム（VC++ 再頒布可能パッケージなど）のインストールは不要です。

■ 使い方
- 起動後、キャラクターをダブルクリックするとメニューが出ます。
- 頭や顔のクリック、頭をなでる（ボタンを押さずに往復）、長押しドラッグに反応します。
- 「デスクトップ行動」から、歩行、手前の窓への着席、自律行動などを試せます。
- SSP のメニューは、キャラクターを右クリックすると開きます。

■ 同梱物とライセンス
- 3D ランタイム（ghost\master\3d\ukagaka3d.exe）と SAORI（ghost\master\ukagaka3d_saori.dll）：
  MIT License（ghost\master\3d\LICENSE.txt）。使用している第三者ソフトウェアの
  ライセンスは ghost\master\3d\THIRD_PARTY_NOTICES.md にあります。
- YAYA（ghost\master\yaya.dll）：BSD 3-Clause License（yaya_LICENSE.txt、yaya_readme-original.txt）
- 3D モデル：CG-CA Uka
  CG-CA Uka (c) 2023-2024 by Nagoya Institute of Technology, Moonshot R&D Goal 1 Avatar Symbiotic Society
  CC BY 4.0（https://creativecommons.org/licenses/by/4.0/deed.ja）
  出典：https://github.com/mmdagent-ex/uka
  テクスチャを縮小しています。改変内容は ghost\master\3d\model\uka\CREDITS.ja.md、
  利用ガイドラインは同じフォルダの README.ja.md を確認してください。
- モーション（ghost\master\3d\motion）：このプロジェクトのプログラムで生成したオリジナルです。

このゴーストとランタイムは非公式のプロジェクトで、うかの公式とは関係ありません。

■ 不具合の報告
https://github.com/koziyagoukei/Uka-3D-Shell/issues
（ukagaka3d_runtime.log と ukagaka3d_saori.log を添えてください。ghost\master にあります）
