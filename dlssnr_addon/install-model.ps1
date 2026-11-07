# SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
# SPDX-License-Identifier: MIT
#
# Makes the neural rendering model in this folder (dlssnr-amd) from your own nvngx_dlssnr.dll,
# with DLSSNR-AMD's extractor (model-tools\dlssnr_extract_model.exe). Only the DLL's weight data is
# read; it is never loaded or run. 310.9.16 gives both networks, 310.8.0 CC_SILVER_AARDWOLD only.
#
#   install-model.bat [nvngx_dlssnr.dll or a zip that contains it]
# (or drop the DLL or zip on install-model.bat; without one, a window asks for it)
param([string]$Dll)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

# The checksums DLSSNR-AMD 0.0.8.2's installer checks the models against.
$ModelSha = '2B41C888CF4155B8958C665BA64018AB0BD25C85FC71A2B6DB86D0D04D1F7FBD'
$SmallSha = '7E966E2639DBEC6596AA0BAC3B8761137D2C40CA8CB4ACA30DF7CA10043C6E33'

function Fail([string]$message) {
    Write-Host "install-model: $message" -ForegroundColor Red
    exit 1
}
function Test-Sha([string]$path, [string]$sha) {
    (Test-Path -LiteralPath $path -PathType Leaf) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $sha
}

$extractor = Join-Path $here 'model-tools\dlssnr_extract_model.exe'
if (-not (Test-Path -LiteralPath $extractor -PathType Leaf)) { Fail "Missing $extractor (unpack the whole NR add-on)." }
if (-not $Dll) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Choose nvngx_dlssnr.dll (310.9.16 or 310.8.0) or a zip that contains it'
    $dialog.Filter = 'nvngx_dlssnr.dll or zip (*.dll;*.zip)|*.dll;*.zip'
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'No nvngx_dlssnr.dll chosen.' }
    $Dll = $dialog.FileName
}
$Dll = $Dll.Trim('"')
if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) { Fail "Not found: $Dll" }

$work = Join-Path $env:TEMP ('shadps4-nr-model-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $src = (Resolve-Path -LiteralPath $Dll).Path
    if ([System.IO.Path]::GetExtension($src) -ieq '.zip') {
        Write-Host 'Unpacking the zip ...'
        Expand-Archive -LiteralPath $src -DestinationPath (Join-Path $work 'zip') -Force
        $found = @(Get-ChildItem -LiteralPath (Join-Path $work 'zip') -Recurse -File -Filter 'nvngx_dlssnr.dll')
        if ($found.Count -ne 1) { Fail "The zip should hold exactly one nvngx_dlssnr.dll; found $($found.Count)." }
        $src = $found[0].FullName
    }
    Write-Host 'Extracting the model (about a minute) ...'
    $out = Join-Path $work 'out'
    New-Item -ItemType Directory -Path $out | Out-Null
    & $extractor $src $out
    $model = Join-Path $out 'dlssnr.bin'
    $small = Join-Path $out 'dlssnr-small.bin'
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $model)) {
        Fail 'Extracting failed (see above). Only NVIDIA''s own nvngx_dlssnr.dll 310.9.16 or 310.8.0 works, not a modified one.'
    }
    if (-not (Test-Sha $model $ModelSha)) { Fail 'The extracted model has the wrong checksum; nothing was installed.' }
    $haveSmall = Test-Path -LiteralPath $small
    if ($haveSmall -and -not (Test-Sha $small $SmallSha)) {
        Fail 'The extracted CC_SMALL_FP8 model has the wrong checksum; nothing was installed.'
    }
    Copy-Item -LiteralPath $model -Destination (Join-Path $here 'dlssnr.bin') -Force
    if ($haveSmall) { Copy-Item -LiteralPath $small -Destination (Join-Path $here 'dlssnr-small.bin') -Force }
    $networks = if ($haveSmall) { 'CC_SMALL_FP8 and CC_SILVER_AARDWOLD' } else { 'CC_SILVER_AARDWOLD' }
    Write-Host "Done: $networks in $here. Start the game, pick AMD FSR in F1 and tick Neural rendering." -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
