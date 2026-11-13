# 06 組み立てと配布

## NAR を作る

リポジトリのルートで実行します。`-YayaDll` を手元の YAYA 6 に置き換えてください。YAYA のライセンスファイルは DLL の隣に置きます。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Test
powershell -ExecutionPolicy Bypass -File scripts\package-ghost.ps1 -YayaDll 'C:\YAYA\yaya.dll' -Output 'build\ghost\uka-custom' -Nar
```

`build/ghost/uka-custom.nar` を SSP にドラッグして導入します。`-Output` は新しいフォルダを指定してください。既存の作業フォルダで設定や辞書を編集したあとに NAR を作り直す場合は、編集済みフォルダの **中身** を ZIP にまとめ、拡張子を `.nar` にします。アーカイブ直下に `install.txt` が入るようにしてください。

Windows PowerShell 5.1 の `Compress-Archive` は、ZIP 内のフォルダの区切りを `\` で書くため使わないでください（ZIP では `/` が正しく、展開するツールによってはフォルダ構造が崩れます）。PowerShell 7 の `Compress-Archive` か、次のコマンドを使います。

```powershell
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$src = (Resolve-Path 'build\ghost\uka-custom').Path
$zip = [IO.Compression.ZipFile]::Open("$PWD\build\ghost\uka-edited.nar", 'Create')
Get-ChildItem $src -Recurse -File | ForEach-Object {
    $name = $_.FullName.Substring($src.Length + 1).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $name)
}
$zip.Dispose()
```

3D ランタイムと SAORI は MSVC ランタイムを静的リンクしているので、VC++ 再頒布可能パッケージを同梱する必要はありません。

## フォルダ構成

```text
uka-custom/
├── install.txt
├── ghost/master/
│   ├── descript.txt
│   ├── yaya.dll
│   ├── yaya.txt
│   ├── ghost.dic、ukagaka3d.dic など
│   ├── ukagaka3d_saori.dll
│   ├── ukagaka3d.json
│   ├── hit-regions.json
│   └── 3d/
│       ├── ukagaka3d.exe
│       ├── THIRD_PARTY_NOTICES.md
│       ├── model/uka/       PMX、テクスチャ、Uka の README
│       └── motion/          VMD
└── shell/master/            透明な 2D シェル
```

`ghost.dic` で会話、`hit-regions.json` で部位、`ukagaka3d.json` でモデルを変更できます。自分のモデルを組み立て時にコピーするには、`package-ghost.ps1` に `-ModelDir 'C:\MMD\MyModel' -ModelFile 'MyModel.pmx'` を追加します。生成後に部位設定をそのモデルに合わせます。自分のゴーストとして配布する前に `install.txt` と `ghost/master/descript.txt` の名前や格納先を見直してください。

## ライセンスとクレジット

- サンプルの Uka モデルとテクスチャは CC BY 4.0 です。同梱の `THIRD_PARTY_NOTICES.md` と Uka の README を残し、変更した場合は変更点を記載します。[公式配布元](https://github.com/mmdagent-ex/uka/blob/main/README.ja.md)
- YAYA の配布には、YAYA と元の AYA のライセンス表示を残します。組み立て時にコピーされたライセンスを確認してください。
- サンプルの VMD はプロジェクトの合成モーションです。外部の PMX、VMD、テクスチャに差し替える場合は、作者がゴーストへの同梱・再配布を許可しているか素材ごとに確認し、指定のクレジットを添えます。
