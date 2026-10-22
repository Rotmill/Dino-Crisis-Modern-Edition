# Сборка DinoCrisisME_Setup.exe.
#
# Всё содержимое мода складывается в payload.zip и вшивается в exe ресурсом:
# у пользователя один файл, распаковывать нечего. Компилятор берётся из состава
# .NET Framework — он есть в любой Windows, ставить ничего не нужно.
#
#     powershell -ExecutionPolicy Bypass -File build_setup.ps1
#
# Результат: <пакет>\Game\DinoCrisisME_Setup.exe (рядом README.txt).

# -Out <файл> (10.10.2026) - собрать установщик в другое место (проверка, пока Game\DinoCrisisME_Setup.exe открыт)
param([string] $Out = '')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root    = Split-Path -Parent $PSScriptRoot        # _source
$Package = Split-Path -Parent $Root                # папка пакета
# 06.10.2026 (игрок: «установщик игры и инструмент для моддинга идут раздельно»): публикуемое для игры - в Game\
# (установщик и README.txt), для приложения модов - в Studio\ (rebuild.ps1, шаг 10)
$GameOut = Join-Path $Package 'Game'
New-Item -ItemType Directory -Path $GameOut -Force | Out-Null
if (-not $Out) { $Out = Join-Path $GameOut 'DinoCrisisME_Setup.exe' }

$stage = Join-Path $env:TEMP ('dcme_stage_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$zip   = Join-Path $env:TEMP ('dcme_payload_' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.zip')

try {
    # ── содержимое ──────────────────────────────────────────────────────────
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Copy-Item (Join-Path $Root 'mod') $stage -Recurse -Force
    New-Item -ItemType Directory -Path (Join-Path $stage 'installer') -Force | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'dcme-install.ps1') (Join-Path $stage 'installer') -Force
    Copy-Item (Join-Path $GameOut 'README.txt') $stage -Force

    # Кэш Python в поставку не идёт.
    #
    # ⛔ Никаких масок вроде «*.obj» здесь быть не должно: камерная коллизия —
    # это 121 файл .obj, и такая «чистка» вырезала её из установщика целиком.
    # Папка mod — не дерево сборки, объектников и pdb в ней не бывает.
    Get-ChildItem $stage -Recurse -Include '__pycache__','*.pyc' -Force |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

    # 11.10.2026: деинсталлятор игры (Uninstall.cs) - в пакет рядом с dcme-install.ps1, тот кладёт его в папку игры
    $cscU = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $iconU = Join-Path $Root 'mod\project\dc_icon.ico'
    & $cscU /target:winexe /platform:x64 /optimize+ /nologo /r:System.Windows.Forms.dll "/win32icon:$iconU" `
        "/out:$(Join-Path $stage 'installer\Uninstall.exe')" (Join-Path $PSScriptRoot 'Uninstall.cs')
    if ($LASTEXITCODE -ne 0) { throw "csc (Uninstall.cs) завершился с кодом $LASTEXITCODE" }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
    $mb = (Get-Item $zip).Length / 1MB
    Write-Host ("payload.zip: {0:N1} MB" -f $mb)

    # ── компиляция ──────────────────────────────────────────────────────────
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if (-not (Test-Path $csc)) { throw "csc.exe не найден: $csc" }

    # Значок — тот же, что у самой игры: один файл на мод и установщик.
    $icon = Join-Path $Root 'mod\project\dc_icon.ico'
    if (-not (Test-Path $icon)) { throw "нет значка: $icon" }

    $args = @(
        '/target:winexe', '/platform:x64', '/optimize+', '/nologo',
        '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll',
        '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll',
        "/win32icon:$icon",
        "/resource:$(Join-Path $PSScriptRoot 'bg.jpg'),bg.jpg",
        "/resource:$zip,payload.zip",
        "/out:$Out",
        (Join-Path $PSScriptRoot 'Setup.cs')
    )
    & $csc @args
    if ($LASTEXITCODE -ne 0) { throw "csc завершился с кодом $LASTEXITCODE" }

    Write-Host ("{0}: {1:N0} байт" -f (Split-Path $Out -Leaf), (Get-Item $Out).Length)
}
finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
}
