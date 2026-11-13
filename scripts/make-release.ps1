<#
.SYNOPSIS
  Builds the distribution files of a release candidate into build\release\<Label>.

.DESCRIPTION
  Prerequisites: scripts\build.ps1 -Test (x64 + x86, Release) and YAYA 6 (yaya.dll with its LICENSE).
  Produces:
    ukagaka3d-<Label>-sample-uka.nar        sample ghost (CG-CA Uka, 2048 textures) for SSP
    ukagaka3d-<Label>-runtime.zip           runtime, SAORI, helper dictionary, motion generator, licenses
    ukagaka3d-<Label>-ghost-template.zip    ghost template without a model, author guide, API reference
    SHA256SUMS.txt, FILES.txt               checksums and the list of files inside each archive
  Nothing is uploaded or published.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-release.ps1 -YayaDll ..\yaya\yaya.dll -Label 0.1.0-beta-rc1
#>
param(
    [Parameter(Mandatory = $true)][string]$YayaDll,
    [Parameter(Mandatory = $true)][string]$Label,
    [ValidateSet('4096', '2048', '1024')][string]$UkaTextureSize = '2048'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root "build\release\$Label"
if (Test-Path -LiteralPath $out) { throw "Output already exists: $out (remove it or choose another label)" }
New-Item -ItemType Directory -Force $out | Out-Null
$work = Join-Path $out '_work'
New-Item -ItemType Directory -Force $work | Out-Null
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

function New-Archive([string]$folder, [string]$archive) {
    # ZIP entries with '/' separators (Compress-Archive in PowerShell 5.1 writes '\').
    $base = (Resolve-Path -LiteralPath $folder).Path.TrimEnd('\')
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File) {
            $entry = $file.FullName.Substring($base.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $zip.Dispose()
    }
}
function Write-Utf8([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n").Replace("`n", "`r`n"), (New-Object Text.UTF8Encoding($true)))
}

# ---- 1. Sample ghost (NAR) ---------------------------------------------------
$sample = Join-Path $work 'ukagaka3d_sample'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package-ghost.ps1') -YayaDll $YayaDll `
    -Output $sample -UkaTextureSize $UkaTextureSize
if ($LASTEXITCODE -ne 0) { throw 'package-ghost.ps1 failed' }
New-Archive $sample (Join-Path $out "ukagaka3d-$Label-sample-uka.nar")

# ---- 2. Runtime ---------------------------------------------------------------
$runtime = Join-Path $work 'runtime'
New-Item -ItemType Directory -Force (Join-Path $runtime 'bin'), (Join-Path $runtime 'yaya') | Out-Null
Copy-Item (Join-Path $root 'build\msvc-x64\Release\ukagaka3d.exe') (Join-Path $runtime 'bin')
Copy-Item (Join-Path $root 'build\msvc-x86\Release\ukagaka3d_saori.dll') (Join-Path $runtime 'bin')
Copy-Item (Join-Path $root 'build\msvc-x64\tools\Release\uka3d_motiongen.exe') (Join-Path $runtime 'bin')
Copy-Item (Join-Path $root 'samples\yaya-ghost\ghost\master\ukagaka3d.dic') (Join-Path $runtime 'yaya')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $runtime 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $runtime
Write-Utf8 (Join-Path $runtime 'README.txt') @"
ukagaka-3d-shell $Label ランタイム一式

bin\ukagaka3d.exe         3D ランタイム（Windows x64）。ゴーストの ghost\master\3d\ に置きます
bin\ukagaka3d_saori.dll   SAORI（x86。SSP から読み込まれます）。ゴーストの ghost\master\ に置きます
bin\uka3d_motiongen.exe   PMX に合わせて待機・歩行・着席・つまみの VMD を作るツール
yaya\ukagaka3d.dic        YAYA から 3D を操作する関数（Launch3D、Play3DMotion など）

使い方は ghost-template の docs\author-guide\ と、
https://github.com/koziyagoukei/Ukagaka-3DShell を参照してください。
VC++ 再頒布可能パッケージなどの追加インストールは不要です（MSVC ランタイムは静的リンク）。
ライセンス：MIT（LICENSE.txt）。第三者ソフトウェアは THIRD_PARTY_NOTICES.md。
"@
New-Archive $runtime (Join-Path $out "ukagaka3d-$Label-runtime.zip")

# ---- 3. Ghost template (the sample without the model) --------------------------
$template = Join-Path $work 'ghost-template'
$ghost = Join-Path $template 'my3dghost'
Copy-Item -Recurse $sample $ghost
$master = Join-Path $ghost 'ghost\master'
Remove-Item -Recurse -Force (Join-Path $master '3d\model\uka')
New-Item -ItemType Directory -Force (Join-Path $master '3d\model\user') | Out-Null
Write-Utf8 (Join-Path $master '3d\model\user\ここにモデルを置く.txt') @"
PMX とテクスチャのフォルダをここに置き、ghost\master\ukagaka3d.json の "model" を
"3d/model/user/（PMX のファイル名）" に書き換えてください。
モデルの利用規約（再配布・改変・ゴーストへの組み込みの可否）を必ず確認してください。
"@
# Motions fitted to Uka are replaced by the generic ones; regenerate them for your model with uka3d_motiongen.
$assets = Join-Path $root 'build\msvc-x64\test-assets\モーション'
foreach ($kv in @{ '待機テスト.vmd' = 'idle.vmd'; '歩行.vmd' = 'walk.vmd'; '着席.vmd' = 'sit.vmd'; 'つまみ.vmd' = 'grab.vmd' }.GetEnumerator()) {
    Copy-Item -Force (Join-Path $assets $kv.Key) (Join-Path $master "3d\motion\$($kv.Value)")
}
$json = [IO.File]::ReadAllText((Join-Path $master 'ukagaka3d.json'), [Text.Encoding]::UTF8)
$json = [regex]::Replace($json, '"model":\s*"[^"]*"', '"model": "3d/model/user/model.pmx"')
[IO.File]::WriteAllText((Join-Path $master 'ukagaka3d.json'), $json, (New-Object Text.UTF8Encoding($false)))
foreach ($f in @('install.txt', 'ghost\master\descript.txt')) {
    $p = Join-Path $ghost $f
    $t = [IO.File]::ReadAllText($p, [Text.Encoding]::UTF8).Replace('name,ukagaka3d sample', 'name,my 3d ghost').Replace('directory,ukagaka3d_sample', 'directory,my3dghost').Replace('sakura.name,うか', 'sakura.name,（名前）')
    [IO.File]::WriteAllText($p, $t, (New-Object Text.UTF8Encoding($false)))
}
Remove-Item -Force (Join-Path $ghost 'readme.txt')
New-Item -ItemType Directory -Force (Join-Path $template 'docs'), (Join-Path $template 'tools') | Out-Null
Copy-Item -Recurse (Join-Path $root 'docs\author-guide') (Join-Path $template 'docs\author-guide')
foreach ($doc in @('ipc-protocol.md', 'desktop-behavior.md', 'interaction-transport.md', 'satori-to-yaya.md')) {
    Copy-Item (Join-Path $root "docs\$doc") (Join-Path $template 'docs')
}
Copy-Item (Join-Path $root 'build\msvc-x64\tools\Release\uka3d_motiongen.exe') (Join-Path $template 'tools')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $template 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $template
Write-Utf8 (Join-Path $template 'README.txt') @"
ukagaka-3d-shell $Label ゴースト制作テンプレート

my3dghost\                 モデルを除いたゴースト一式（YAYA 6、SAORI、3D ランタイム、透明シェル、辞書）
docs\author-guide\         作者向けマニュアル（00 から順に読んでください）
docs\ipc-protocol.md       SAORI と IPC のリファレンス
docs\desktop-behavior.md   歩行・着席などのデスクトップ行動 API
tools\uka3d_motiongen.exe  モデルに合わせたモーションを作るツール

1. my3dghost\ghost\master\3d\model\user\ に PMX とテクスチャを置き、ukagaka3d.json の "model" を書き換えます。
2. tools\uka3d_motiongen.exe （PMX のパス） my3dghost\ghost\master\3d\motion で、待機・歩行・着席・つまみを作ります。
3. hit-regions.json の当たり判定と、ghost.dic の会話をモデルに合わせて変えます。
4. 名前（install.txt と ghost\master\descript.txt）を変えて、フォルダを SSP に入れます。

同梱の YAYA は BSD 3-Clause License です（ghost\master\yaya_LICENSE.txt）。
"@
New-Archive $template (Join-Path $out "ukagaka3d-$Label-ghost-template.zip")

# ---- 4. Checksums and file lists -------------------------------------------------
$sums = foreach ($f in Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -in '.nar', '.zip' } | Sort-Object Name) {
    '{0} *{1}' -f (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLower(), $f.Name
}
[IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
$list = foreach ($f in Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -in '.nar', '.zip' } | Sort-Object Name) {
    "## $($f.Name) ($([Math]::Round($f.Length / 1MB, 1)) MB)"
    $zip = [IO.Compression.ZipFile]::OpenRead($f.FullName)
    try { foreach ($e in $zip.Entries | Sort-Object FullName) { '{0,12:N0}  {1}' -f $e.Length, $e.FullName } } finally { $zip.Dispose() }
    ''
}
Write-Utf8 (Join-Path $out 'FILES.txt') ($list -join "`n")
Remove-Item -Recurse -Force $work
Get-Content (Join-Path $out 'SHA256SUMS.txt')
Write-Host "Release files: $out" -ForegroundColor Green
