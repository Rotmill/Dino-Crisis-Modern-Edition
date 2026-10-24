<#
    Dino Crisis ME - установка мода.

    Здесь лежит вся работа; окно (install.ps1) только собирает два пути и
    зовёт этот файл. Скрипт можно запустить и без окна:

        powershell -ExecutionPolicy Bypass -File dcme-install.ps1 `
                   -DiscPath "D:\Dino Crisis (Track 1).bin" `
                   -InstallPath "C:\Games\DinoCrisisME"

    Что он делает по шагам:
      1  проверяет инструменты сборки: компилятор (Clang из llvm-mingw) с записанными планами сборки, Python,
         заголовки Vulkan и SDL2 едут в пакете мода - ставить не нужно ничего (10.10.2026); без компилятора в
         пакете (или с -Toolchain msvc) - прежний путь через Visual Studio Build Tools и CMake
      2  берёт каркас psxrecomp на закреплённом коммите: скачивает архив сам
         либо распаковывает тот, что пользователь скачал вручную (-RecompZip)
      3  накладывает патч мода и кладёт наши файлы рантайма
      4  кладёт SDL2 dev pack из пакета мода (нет в пакете - скачивает)
      5  достаёт SLUS_009.22 из ОБРАЗА ПОЛЬЗОВАТЕЛЯ
      6  собирает рекомпилятор и рекомпилирует игру
      7  переписывает генкод под PGXP и накладывает патч наших хуков
      8  распаковывает комнаты из образа в дерево сборки
      9  упаковывает данные игры (настройки, подгонку, game.toml, лицензии,
         коллизию, комнаты) в dcme_data.pak — он ляжет внутрь программы
     10  собирает DinoCrisisME.exe
     11  раскладывает готовую папку и делает ярлык

    Ни один файл игры в поставку не входит: и исполняемый файл, и геометрия
    комнат берутся из образа, который пользователь предоставил сам.

    08.10.2026 (игрок: «установщик один, поддерживает английскую версию,
    русскую, дальше я, возможно, добавлю ещё версии - список образов»): образ
    узнаётся по sha256 его SLUS_009.22 (шаг «Распознаю образ игры»), список -
    mod\versions\<id>\version.txt. Мод написан под USA v1.1; для другой версии
    поверх него ложатся её runtime.dcpatch (адреса рантайма по её карте),
    hooks.dcpatch (хуки для кода из её exe) и faces\ (рот под её озвучку) -
    их собирает rebuild.ps1, шаг 8b. Незнакомый образ - отказ до сборки.

    17.09.2026 (игрок: «dc_user.cfg, input.ini, SLUS_009.22 оставляй, всё
    остальное зашивай в exe; это релиз мода, тут не должно быть ни дебага, ни
    отладочных файлов»): в data\ ложатся только эти три файла, остальное —
    внутри DinoCrisisME.exe. Шрифт интерфейса игра берёт прямо из папки шрифтов
    Windows, копий рядом с игрой нет.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $DiscPath,
    [Parameter(Mandatory = $true)] [string] $InstallPath,
    [string] $RecompZip,           # архив каркаса, скачанный вручную; пусто — качаем сами
    [switch] $SkipPrereq,          # не пытаться доустанавливать через winget
    [switch] $KeepBuildTree,       # не удалять исходники и объектники после сборки
    [switch] $NoShortcut,          # не трогать рабочий стол
    [switch] $RemoveTools,         # удалить программы сборки, которые доустановил сам
    [ValidateSet('en','ru')]
    [string] $Lang = 'en',         # язык журнала: по умолчанию английский
    [switch] $DryRun,              # только проверки, ничего не менять
    [ValidateSet('auto','llvm','msvc')]
    [string] $Toolchain = 'auto',  # 10.10.2026: чем собирать - auto: Clang из пакета, если он там есть, иначе MSVC
    [scriptblock] $OnLog,          # окно передаёт сюда свой приёмник строк
    [scriptblock] $OnProgress      # -//- для полосы прогресса: (шаг, всего, текст)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Язык сообщений. По умолчанию английский — мод рассчитан не только на
# русскоязычных; -Lang ru переключает. Каждая строка живёт рядом со своим
# переводом, чтобы они не расходились при правках.
$script:RU = ($Lang -eq 'ru')
function L { param([string] $En, [string] $Ru) if ($script:RU) { $Ru } else { $En } }

# Вывод в UTF-8. Когда установщик читает наш поток, консоль по умолчанию отдаёт
# его в кодировке OEM, и не-ASCII превращается в «???» ещё до окна и до файла
# журнала. Ставим явно.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$OutputEncoding = [System.Text.Encoding]::UTF8

# 09.10.2026: Python пишет в трубу в кодировке ANSI, а мы читаем UTF-8: путь
# python.exe под кириллическим именем пользователя (C:\Users\Иван\...) доходил
# битым, и стоящий Python «не находился». Просим у него UTF-8.
$env:PYTHONIOENCODING = 'utf-8'

# Архивы с GitHub отдаются только по TLS 1.2. Обычно .NET берёт протоколы
# системы (SystemDefault = 0); если на машине задан свой список - добавляем 1.2.
try {
    $sp = [Net.ServicePointManager]::SecurityProtocol
    if ([int] $sp -ne 0) { [Net.ServicePointManager]::SecurityProtocol = $sp -bor [Net.SecurityProtocolType]::Tls12 }
} catch { }

# ─────────────────────────────── настройки ───────────────────────────────
$ModRoot       = Split-Path -Parent $PSScriptRoot           # корень пакета
$ModDir        = Join-Path $ModRoot 'mod'

# Каркас рекомпилятора. Коммит закреплён: патч мода собран ровно против него,
# на другом коммите он не ляжет.
#
# Берём ZIP-архив коммита, а не клон: Git пользователю ставить незачем, а
# содержимое архива — ровно те же файлы репозитория, что и в клоне, только без
# истории и без развёртки переводов строк (в архиве они такие, как в
# репозитории; патч мода собран против этого состояния).
$PsxRecompCommit = 'f69b2dde99813a54cdc9287dd83f7c7fd035b32d'
$PsxRecompDir    = "psxrecomp-$PsxRecompCommit"          # имя папки внутри архива
$PsxRecompUrl    = "https://github.com/mstan/psxrecomp/archive/$PsxRecompCommit.zip"
$PsxRecompPage   = 'https://github.com/mstan/psxrecomp'  # показывается при ручной загрузке

$Sdl2Version = '2.30.3'
$Sdl2Url     = "https://github.com/libsdl-org/SDL/releases/download/release-$Sdl2Version/SDL2-devel-$Sdl2Version-VC.zip"

# Шрифты интерфейса установщик больше не кладёт (17.09.2026): игра читает
# системный Bahnschrift прямо из папки шрифтов Windows (на Windows без него —
# Segoe UI, затем Arial), а Shippori Mincho с Google Fonts не качается вовсе.

# Dear ImGui в релизе НЕ УЧАСТВУЕТ. Раньше на нём висели перекрестие, экранные
# сообщения и три отладочных окна; теперь прицел рисуется своим кодом на SDL
# (runtime/src/dc_reticle.c), сообщения ушли в наш слой (dc_ui_text), шрифт
# растеризуется оригинальным stb_truetype, а окна отключены определением
# MCM_NO_IMGUI в CMakeLists проекта. Вернуть их — убрать это определение и
# скачать библиотеку с коммита ниже: релизный тег v1.92.9 не годится, на нём
# наш бэкенд SDL_Renderer молча перестаёт строить геометрию.
$ImGuiCommit = '5220a3f48d39c000da8b46fb0fccf3dd62695c51'   # 1.92.9 WIP

$TotalSteps = 14

# ─────────────────────────────── журнал ──────────────────────────────────
$script:StepNo = 0

function Write-Log {
    param([string] $Message, [ValidateSet('info','ok','warn','err','step')] [string] $Level = 'info')
    $line = switch ($Level) {
        'step' { "== $Message" }
        'ok'   { "   [ok] $Message" }
        'warn' { "   [!]  $Message" }
        'err'  { "   [x]  $Message" }
        default { "   $Message" }
    }
    $color = switch ($Level) { 'step' {'Cyan'} 'ok' {'Green'} 'warn' {'Yellow'} 'err' {'Red'} default {'Gray'} }
    Write-Host $line -ForegroundColor $color
    if ($OnLog) { & $OnLog $line $Level }
}

function Start-Step {
    param([string] $Title)
    $script:StepNo++
    Write-Log ((L "Step" "Шаг") + " $script:StepNo " + (L "of" "из") + " ${TotalSteps}: $Title") 'step'
    if ($OnProgress) { & $OnProgress $script:StepNo $TotalSteps $Title }
}

$script:LastToolExit = 0

function Invoke-Tool {
    <# Запускает программу, пишет её вывод в журнал и падает на ненулевом коде.

       В поток НИЧЕГО не возвращает намеренно: функция-шаг отдаёт наружу путь
       (`return $dir`), и если бы каждый вызов инструмента дописывал в тот же
       поток свой код возврата, вызывающий получил бы массив `0 0 0 0 <путь>`.
       Код последнего запуска лежит в $script:LastToolExit. #>
    param(
        [Parameter(Mandatory)] [string] $File,
        [string[]] $Arguments = @(),
        [string] $WorkDir,
        [switch] $Quiet,
        [switch] $AllowFailure
    )
    $prev = $null
    if ($WorkDir) { $prev = (Get-Location).Path; Set-Location $WorkDir }
    try {
        Write-Log "> $File $($Arguments -join ' ')"
        # 2>&1 склеивает потоки, но в Windows PowerShell 5.1 каждая строка stderr
        # нативной программы становится ErrorRecord: при ErrorActionPreference=Stop
        # сборка падала бы на первом же предупреждении компилятора, ещё до того как
        # станет известен код возврата. Поэтому на время вызова ослабляем режим.
        $keep = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try   { $out = & $File @Arguments 2>&1 }
        finally { $ErrorActionPreference = $keep }
        $code = $LASTEXITCODE
        $script:LastToolExit = $code
        if (-not $Quiet) {
            foreach ($l in $out) { if ("$l".Trim()) { Write-Log ("  " + $l) } }
        }
        if ($code -ne 0 -and -not $AllowFailure) {
            if ($Quiet) { foreach ($l in $out) { if ("$l".Trim()) { Write-Log ("  " + $l) } } }
            # 21.10.2026 (у друга игрока: «python.exe exited with code 1», а причины в окне нет - окно показывает только шаги):
            # последние строки вывода программы - и в окно, обрезанные (полностью они в install-log.txt)
            $tail = @(@($out | ForEach-Object { "$_".Trim() } | Where-Object { $_ }) | Select-Object -Last 8)
            foreach ($l in $tail) { if ($l.Length -gt 240) { $l = $l.Substring(0, 237) + '...' }; Write-Log $l 'err' }
            if (@($out | Where-Object { "$_" -match 'WinError 4551|APP CONTROL:' }).Count -gt 0) { Write-AppControlHelp 'err' }
            throw ((L "`"$File`" exited with code $code" "«$File» завершился с кодом $code"))
        }
    } finally {
        if ($prev) { Set-Location $prev }
    }
}

function Get-CommandPath {
    param([string] $Name)
    $c = Get-Command $Name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Test-PythonExe {
    <# Запускает кандидата и возвращает путь настоящего python.exe (sys.executable),
       если это Python 3.8+; иначе $null. Заглушка Microsoft Store
       (WindowsApps\python.exe без установленного Python) печатает «Python was not
       found...» и выходит с кодом 9009 - её отбрасываем. #>
    param([string] $Exe, [string[]] $Pre = @())
    $keep = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Pre -c "import sys;print('%d.%d'%sys.version_info[:2]);print(sys.executable)" 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $out) { return $null }
        $lines = @($out)
        if ($lines.Count -lt 2 -or $lines[0] -notmatch '^3\.(\d+)$' -or [int]$Matches[1] -lt 8) { return $null }
        $real = $lines[1].Trim()
        if ($real -and (Test-Path -LiteralPath $real)) { return $real }
        return $null
    } catch { return $null }
    finally { $ErrorActionPreference = $keep }
}

function Find-BundledPython {
    <# 10.10.2026: Python из пакета мода (tools\make_portable_python.py пакета): установщику нужна только стандартная
       библиотека, игроку ставить Python больше не нужно. #>
    $p = Join-Path $ModDir 'tools\python\python.exe'
    if (Test-Path -LiteralPath $p) { return (Test-PythonExe $p) }
    return $null
}

function Find-BundledVulkan {
    <# 10.10.2026: заголовки Vulkan и библиотека импорта загрузчика из пакета мода (tools\make_vulkan_bundle.py пакета):
       картинку показывает DirectX 11, но рендер - Vulkan, и для сборки нужны только они, а не весь Vulkan SDK;
       vulkan-1.dll приходит с драйвером видеокарты. #>
    $d = Join-Path $ModDir 'vulkan'
    if ((Test-Path -LiteralPath (Join-Path $d 'Include\vulkan\vulkan.h')) -and (Test-Path -LiteralPath (Join-Path $d 'Lib\vulkan-1.lib'))) { return $d }
    return $null
}

function Find-BundledLlvm {
    <# 10.10.2026 (игрок: «давай пробовать бесплатный компилятор, чтобы не качать Visual Studio Build Tools»; затем
       «убери CMake и Ninja»): компилятор C/C++ в пакете мода - llvm-mingw (Clang + LLD + заголовки и библиотеки
       MinGW-w64, всё можно раздавать; tools\make_llvm_bundle.py пакета) и два записанных плана сборки
       (mod\build\recompiler.plan.json, game.plan.json; их снимает rebuild.ps1, шаг 8c). Сборка идёт по ним
       mod\tools\run_plan.py - ни CMake, ни Ninja игроку не нужны. Путь к папке llvm или $null. #>
    $cc = Join-Path $ModDir 'tools\llvm\bin\x86_64-w64-mingw32-clang.exe'
    $ok = (Test-Path -LiteralPath $cc) -and (Test-Path -LiteralPath (Join-Path $ModDir 'tools\run_plan.py')) -and
          (Test-Path -LiteralPath (Join-Path $ModDir 'build\recompiler.plan.json')) -and
          (Test-Path -LiteralPath (Join-Path $ModDir 'build\game.plan.json'))
    if ($ok) { return (Join-Path $ModDir 'tools\llvm') }
    return $null
}

function Get-BuildJobs {
    <# 21.10.2026 (у друга игрока установка встала на «Recompiling the game to C», «питон вешает комп»): компиляторов
       сразу - не больше процессоров и не больше, чем помещается в свободную память: по $PerJobMB на каждый, $BigMB - самому
       большому файлу, гигабайт - системе. Замер пробной установки (32 сразу): все компиляторы вместе в пике 1.7 ГБ, самый
       большой (генкод) - 1 ГБ. Свободную память не узнать - не больше 4. #>
    param([int] $PerJobMB, [int] $BigMB)
    $cpu = [Math]::Max(1, [Environment]::ProcessorCount)
    try {
        $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
        $freeMB = [int]($os.FreePhysicalMemory / 1024)
        $jobs = [int][Math]::Max(1, [Math]::Min($cpu, [Math]::Floor(($freeMB - 1024 - $BigMB) / $PerJobMB)))
        Write-Log ("$jobs " + (L "compilers at a time" "компиляторов сразу") + " ($freeMB MB " + (L "free" "свободно") + ", $cpu " + (L "processors" "процессоров") + ")")
    } catch {
        $jobs = [int][Math]::Min($cpu, 4)
    }
    return $jobs
}

# Сборка по записанному плану (Clang из пакета): run_plan.py подставляет пути и выполняет команды, компиляции - параллельно
function Invoke-Plan {
    param($Tools, [string] $Plan, [string] $SrcRoot, [int] $PerJobMB, [int] $BigMB)
    $jobs = Get-BuildJobs $PerJobMB $BigMB
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\run_plan.py'), (Join-Path $ModDir "build\$Plan"),
        "SRC=$SrcRoot", "LLVM=$($Tools.Llvm)", "VULKAN=$($Tools.Vulkan)", "JOBS=$jobs")
}

function Find-Python {
    <# Настоящий Python 3: все python/python3 в PATH (первым часто стоит заглушка
       Microsoft Store), обычные папки установки (туда ставит и winget), затем py -3. #>
    $cands = New-Object System.Collections.Generic.List[string]
    foreach ($n in @('python', 'python3')) {
        Get-Command $n -All -CommandType Application -ErrorAction SilentlyContinue |
            ForEach-Object { if ($_.Source -and -not $cands.Contains($_.Source)) { $cands.Add($_.Source) } }
    }
    foreach ($root in @("$env:LOCALAPPDATA\Programs\Python", $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $root -or -not (Test-Path $root)) { continue }
        Get-ChildItem $root -Directory -Filter 'Python3*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending |
            ForEach-Object { $p = Join-Path $_.FullName 'python.exe'; if ((Test-Path $p) -and -not $cands.Contains($p)) { $cands.Add($p) } }
    }
    foreach ($c in $cands) { $r = Test-PythonExe $c; if ($r) { return $r } }
    $py = Get-CommandPath 'py'
    if ($py) { $r = Test-PythonExe $py @('-3'); if ($r) { return $r } }
    return $null
}

# ──────────────────────── шаг 1: инструменты ─────────────────────────────
function Find-MsvcCMake {
    <# CMake из состава Visual Studio: он есть даже когда отдельный не ставили. #>
    foreach ($base in @("${env:ProgramFiles}\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio")) {
        if (-not (Test-Path $base)) { continue }
        $hit = Get-ChildItem -Path $base -Recurse -Filter 'cmake.exe' -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -like '*CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' } |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

function Find-VulkanSdk {
    if ($env:VULKAN_SDK -and (Test-Path $env:VULKAN_SDK)) { return $env:VULKAN_SDK }
    $root = 'C:\VulkanSDK'
    if (Test-Path $root) {
        $hit = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
               Sort-Object Name -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

function Find-CMake {
    $c = Get-CommandPath 'cmake'
    if ($c) { return $c }
    # MSI Kitware кладёт его сюда, а PATH правит не всегда
    $p = Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $p) { return $p }
    return (Find-MsvcCMake)
}

function Find-Msvc {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    # как у Python: строка в stderr при ErrorActionPreference=Stop оборвала бы всю установку
    $keep = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null }
    catch { $p = $null }
    finally { $ErrorActionPreference = $keep }
    if ($p) { return ($p | Select-Object -First 1) }
    return $null
}

# Что установщик доустановил САМ в этот запуск. Только это и разрешено потом
# удалять: если компилятор или Python стояли у человека раньше, они его, а не
# наши, и сносить их нельзя ни при каких галочках.
$script:InstalledByUs = New-Object System.Collections.ArrayList

function Install-ViaWinget {
    param([string] $Id, [string] $Human, [string[]] $Extra = @(), [string] $Note = '')
    $winget = Get-CommandPath 'winget'
    if (-not $winget) {
        Write-Log (L "$Human is missing and winget is unavailable - install it manually." "$Human не найден, и winget недоступен - поставь вручную.") 'err'
        return $false
    }
    # 09.10.2026: строка видна в окне установщика (уровень step), а не только в журнале - иначе окно надолго замирало
    # на «Не хватает: ...», и человек решал, что установка закрылась
    Write-Log ((L "Installing $Human via winget (may take a few minutes)..." "Ставлю $Human через winget (может занять несколько минут)...") + $Note) 'step'
    Invoke-Tool -File $winget -Arguments (@('install','--id',$Id,'-e','--accept-package-agreements','--accept-source-agreements','--disable-interactivity') + $Extra) -AllowFailure
    # 10.10.2026: программа встала, но Windows просит перезагрузку - коды 3010/1641 установщиков и коды winget
    # 0x8A150109 (перезагрузить, чтобы закончить), 0x8A15010A (нужна перезагрузка), 0x8A15010B (перезагрузка начата)
    $code = $script:LastToolExit
    $script:LastInstallReboot = ($code -eq 3010 -or $code -eq 1641 -or $code -eq -1978335991 -or $code -eq -1978335990 -or $code -eq -1978335989)
    $ok = ($code -eq 0 -or $script:LastInstallReboot)
    if ($ok) { [void] $script:InstalledByUs.Add(@{ Id = $Id; Human = $Human }) }
    return $ok
}
$script:LastInstallReboot = $false

# 10.10.2026: состояние каждой программы для окна установщика (Setup.cs: значок и подпись в списке под кнопкой Install;
# строку «[tool] ...» окно не показывает): ok - уже стоит, bundled - из пакета мода, missing - нет, installing - ставится,
# installed - поставлена, reboot - Windows просит перезагрузку, failed - не вышло
function Tool-State { param([string] $Key, [string] $State) Write-Host "[tool] $Key $State" }

function Remove-InstalledTools {
    <# Удаляет программы сборки, которые установщик поставил сам. #>
    Start-Step (L "Removing the build tools" "Удаление программ сборки")
    if ($script:InstalledByUs.Count -eq 0) {
        Write-Log (L "nothing to remove: every tool was already on this machine" `
                     "удалять нечего: все программы уже стояли на этой машине") 'ok'
        return
    }
    $winget = Get-CommandPath 'winget'
    if (-not $winget) { Write-Log (L "winget is unavailable - remove them by hand" "winget недоступен - удали их вручную") 'warn'; return }
    foreach ($t in $script:InstalledByUs) {
        Write-Log ((L "removing" "удаляю") + " $($t.Human)...")
        Invoke-Tool -File $winget -Arguments @('uninstall','--id',$t.Id,'-e','--accept-source-agreements','--disable-interactivity') -AllowFailure -Quiet
        if ($script:LastToolExit -eq 0) { Write-Log ("$($t.Human) " + (L "removed" "удалён")) 'ok' }
        else { Write-Log ("$($t.Human) " + (L "could not be removed - remove it by hand" "удалить не удалось - убери вручную")) 'warn' }
    }
}

function Update-ToolEnv {
    <# winget правит PATH и VULKAN_SDK только для новых процессов - подтягиваем их из реестра. #>
    $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path','User')
    $vk = [Environment]::GetEnvironmentVariable('VULKAN_SDK','Machine')
    if (-not $vk) { $vk = [Environment]::GetEnvironmentVariable('VULKAN_SDK','User') }
    if ($vk) { $env:VULKAN_SDK = $vk }
}

function Get-SmartAppControl {
    <# 21.10.2026 (лог друга игрока: «OSError: [WinError 4551] An Application Control policy has blocked this file» на сборке
       рекомпилятора): Smart App Control Windows 11 не пускает неподписанные программы, а мод собирается на ПК игрока -
       подписи у сборки и у самой игры быть не может. VerifiedAndReputablePolicyState: 0 - выкл., 1 - вкл., 2 - оценка;
       -1 - не узнать (Windows 10, нет ключа). #>
    try {
        $v = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name 'VerifiedAndReputablePolicyState' -ErrorAction Stop
        return [int]$v.VerifiedAndReputablePolicyState
    } catch { return -1 }
}

function Write-AppControlHelp {
    <# 21.10.2026 (игрок: «пугающе звучит для тех, у кого он включен»): спокойно - почему Windows не знает игру и что сделать.
       Ошибка 4551 при выключенном Smart App Control - другая политика ПК (рабочие, учебные), её установщик не обойдёт. #>
    param([string] $Level)
    if ((Get-SmartAppControl) -ne 1) {
        Write-Log (L "Windows did not let programs built on this PC run. Smart App Control is off here, so another app control policy of this PC stops them (often on work or school PCs)." "Windows не дала запустить программы, собранные на этом ПК. Интеллектуальное управление приложениями здесь выключено, значит, их не пускает другая политика этого ПК (так бывает на рабочих и учебных ПК).") $Level
        return
    }
    Write-Log (L "Windows 11 Smart App Control is on. It only lets through apps it already knows, and the game is built from your disc right on this PC, so to Windows it is a brand-new app." "Включено интеллектуальное управление приложениями Windows 11. Оно пропускает только знакомые ему программы, а игра собирается из твоего диска прямо на этом ПК, и для Windows это новая программа.") $Level
    Write-Log (L "To install and play, turn it off: Windows Security > App & browser control > Smart App Control settings > Off, then run setup again." "Чтобы установить и играть, выключи его: Безопасность Windows > Управление приложениями и браузером > Интеллектуальное управление приложениями > Выключить, и запусти установку снова.") $Level
}

function Test-Prerequisites {
    Start-Step (L "Checking build tools" "Проверка инструментов сборки")
    if ((Get-SmartAppControl) -eq 1) { Write-AppControlHelp 'warn' }   # 21.10.2026: заранее, до минут сборки

    # Git не нужен: каркас приходит ZIP-архивом закреплённого коммита.
    # 09.10.2026, первый запуск на чистом ПК: проверка взяла заглушку Microsoft Store за Python и оборвала
    # установку. Теперь каждую программу ищем по-настоящему и перепроверяем сразу после её доустановки.
    # Компилятор - раньше CMake: Build Tools с рекомендованными частями приносят и CMake. winget ставит
    # Build Tools БЕЗ компилятора C++ (в манифесте только --quiet --wait), поэтому нагрузка C++ - явно;
    # версия 2026 - та же Visual Studio 18, на которой собирается и проверяется мод.
    # 10.10.2026 (игрок: «что из этого можно включить сразу в мод?»): Python и заголовки Vulkan едут в пакете мода
    # (Find-BundledPython / Find-BundledVulkan; свои - только запасной путь), ставить остаётся компилятор C++ (его
    # лицензия не разрешает раздавать) и, если с ним не пришёл, CMake. Окну - строки «[tool] ключ состояние».
    # 10.10.2026: компилятор из пакета (llvm-mingw) и записанные планы сборки - тогда не нужны ни компилятор Microsoft,
    # ни CMake. -Toolchain msvc - прежний путь через Visual Studio Build Tools и CMake (их ставит winget).
    $llvm = $null
    if ($Toolchain -ne 'msvc') { $llvm = Find-BundledLlvm }
    if ($Toolchain -eq 'llvm' -and -not $llvm) { throw (L "-Toolchain llvm: the package carries no compiler or build plans (mod\tools\llvm, mod\build)." "-Toolchain llvm: в пакете нет компилятора или планов сборки (mod\tools\llvm, mod\build).") }
    if ($llvm) {
        $env:PATH = (Join-Path $llvm 'bin') + ';' + $env:PATH          # windres берёт clang для препроцессора рядом
        Tool-State 'msvc' 'bundled'
        Write-Log ((L "C/C++ compiler: Clang (llvm-mingw) included with the mod" "компилятор C/C++: Clang (llvm-mingw) из пакета мода"))
    }
    $tools = @(
        @{ Key = 'msvc'; Label = 'MSVC'; Human = 'Visual Studio Build Tools (C++)'; Id = 'Microsoft.VisualStudio.BuildTools'; Find = { Find-Msvc };
           Extra = @('--override', '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended');
           Note = (L " Several GB, 10-30 minutes; the progress is in the Visual Studio Installer window." `
                     " Это несколько ГБ, 10-30 минут; ход установки - в окне Visual Studio Installer.");
           Hint = (L "Open Visual Studio Installer, press Modify and add the C++ build tools (MSVC), then run setup again." `
                     "Открой Visual Studio Installer, нажми «Изменить» и добавь средства сборки C++ (MSVC), затем запусти установку заново.") },
        @{ Key = 'cmake'; Label = 'cmake'; Human = 'CMake'; Id = 'Kitware.CMake'; Find = { Find-CMake } },
        @{ Key = 'python'; Label = 'python'; Human = 'Python 3'; Id = 'Python.Python.3.12';
           Find = { $b = Find-BundledPython; if ($b) { $b } else { Find-Python } } },
        @{ Key = 'vulkan'; Label = 'Vulkan'; Human = 'Vulkan SDK'; Id = 'KhronosGroup.VulkanSDK';
           Find = { $b = Find-BundledVulkan; if ($b) { $b } else { Find-VulkanSdk } } }
    )
    if ($llvm) { $tools = @($tools | Where-Object { $_.Key -ne 'msvc' -and $_.Key -ne 'cmake' }) }   # по планам CMake не нужен
    $found = @{}
    $missing = @()
    foreach ($t in $tools) {
        $p = & $t.Find
        if ($p) {
            $found[$t.Key] = $p
            $bundled = $p.StartsWith($ModDir, [StringComparison]::OrdinalIgnoreCase)
            if ($bundled) { Tool-State $t.Key 'bundled'; Write-Log ("$($t.Label): " + (L "included with the mod" "из пакета мода")) }   # 10.10: только в журнал - окну это не нужно
            else          { Tool-State $t.Key 'ok';      Write-Log "$($t.Label): $p" 'ok' }
        } else { $missing += $t; Tool-State $t.Key 'missing' }
    }

    if ($missing.Count -gt 0) {
        Write-Log ((L "Missing: " "Не хватает: ") + (($missing | ForEach-Object { $_.Human }) -join ', ')) 'warn'
        if ($SkipPrereq) { throw (L "Required tools are missing and -SkipPrereq disabled installing them." "Нет обязательных инструментов, а доустановка отключена ключом -SkipPrereq.") }
        Write-Log (L "Setup installs them now and then continues by itself - keep this window open." `
                     "Установщик сейчас поставит их и сам продолжит - не закрывай это окно.") 'warn'
        foreach ($m in $missing) {
            Update-ToolEnv
            $p = & $m.Find                                   # CMake мог прийти вместе с Build Tools
            if (-not $p) {
                $extra = @(); if ($m.ContainsKey('Extra')) { $extra = $m.Extra }
                $note = ''; if ($m.ContainsKey('Note')) { $note = $m.Note }
                Tool-State $m.Key 'installing'
                $ok = Install-ViaWinget $m.Id $m.Human $extra $note
                Update-ToolEnv
                $p = & $m.Find
                if (-not $p) {
                    Tool-State $m.Key 'failed'
                    if ($m.ContainsKey('Hint')) { $msg = (L "$($m.Human): the C++ compiler is still not found." "$($m.Human): компилятор C++ так и не найден.") + ' ' + $m.Hint }
                    elseif ($ok) { $msg = L "$($m.Human) is still not found after installing. Reboot and run setup again." "$($m.Human) не найден и после установки. Перезагрузи компьютер и запусти установку заново." }
                    else         { $msg = L "Could not install $($m.Human). Install it manually and run setup again." "Не удалось поставить $($m.Human). Поставь вручную и запусти установку заново." }
                    throw $msg
                }
                Tool-State $m.Key 'installed'
                if ($script:LastInstallReboot) {
                    Tool-State $m.Key 'reboot'
                    Write-Log (L "Windows asks to restart the PC to finish installing $($m.Human). Setup goes on - the game will be ready; restart the PC when it is done." `
                                 "Windows просит перезагрузить компьютер, чтобы закончить установку $($m.Human). Установка мода продолжается - игра будет готова; перезагрузи компьютер после неё.") 'warn'
                }
                # окна установщиков программ закрывают наше: просим окно установщика вернуться наверх
                # (Setup.cs, строку «[front]» само окно не показывает)
                Write-Host '[front]'
            } else { Tool-State $m.Key 'ok' }               # пришёл вместе с другой программой
            $found[$m.Key] = $p
            Write-Log "$($m.Label): $p" 'ok'
        }
    }

    $env:VULKAN_SDK = $found['vulkan']                       # 10.10: заголовки и vulkan-1.lib - обычно из пакета мода
    $cmake = $null; if ($found.ContainsKey('cmake')) { $cmake = $found['cmake'] }
    return @{ Python = $found['python']; CMake = $cmake; Vulkan = $found['vulkan']; Llvm = $llvm }
}

# ─────────────────────────── вспомогательное ─────────────────────────────
function New-Dir { param([string] $Path) if (-not (Test-Path $Path)) { New-Item -ItemType Directory -Path $Path -Force | Out-Null } }

# 23.10.2026: Remove-Item в Windows PowerShell не берёт пути от 260 знаков (объектники сборки рекомпилятора при длинном
# пути установки): молча оставляет их или падает «Could not find a part of the path». Так от прошлой установки оставалась
# .build-src, и установка в ту же папку падала на шаге каркаса. rd берёт их с приставкой \\?\
function Remove-Tree {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $Path) {
        $full = [IO.Path]::GetFullPath($Path)
        $long = if ($full.StartsWith('\\')) { '\\?\UNC\' + $full.Substring(2) } else { '\\?\' + $full }
        cmd /c "rd /s /q `"$long`"" 2>$null | Out-Null
    }
}

function Get-File {
    param([string] $Url, [string] $Dest, [int] $ExpectedSize = 0)
    Write-Log ((L "downloading" "скачиваю") + " $Url")
    $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
    try { Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing }
    finally { $ProgressPreference = $old }
    $size = (Get-Item $Dest).Length
    if ($ExpectedSize -gt 0 -and $size -ne $ExpectedSize) {
        throw (L "Size of $Dest ($size) does not match the expected $ExpectedSize - the file on the server changed." "Размер $Dest ($size) не совпал с ожидаемым ($ExpectedSize) - файл на сервере изменился.")
    }
    Write-Log ((L "received" "получено") + (" {0:N0} " -f $size) + (L "bytes" "байт")) 'ok'
}

# ─────────────────────────── версии игры ─────────────────────────────────
# 08.10.2026: mod\versions\<id>\version.txt - строки «ключ = значение» (tools\make_version.py пакета): id, order, name,
# name_ru, exe, exe_sha256, exe_size; у не-базовой версии ещё runtime_patch, hooks_patch, faces (пути от её папки);
# subchannel = sbi - диску нужен файл служебного канала рядом с образом (LibCrypt, Европа).
$script:Version = $null

function Get-Versions {
    $list = @()
    foreach ($d in @(Get-ChildItem -LiteralPath (Join-Path $ModDir 'versions') -Directory -ErrorAction SilentlyContinue)) {
        $f = Join-Path $d.FullName 'version.txt'
        if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { continue }
        $v = @{ dir = $d.FullName; order = '99' }
        foreach ($line in [IO.File]::ReadAllLines($f, [Text.Encoding]::UTF8)) {
            if ($line -match '^\s*([a-z_0-9]+)\s*=\s*(.*?)\s*$') { $v[$Matches[1]] = $Matches[2] }
        }
        if ($v['id'] -and $v['exe_sha256']) { $list += ,$v }
    }
    return @($list | Sort-Object { [int]$_['order'] })
}

function Get-VersionName { param($V) if ($script:RU -and $V['name_ru']) { $V['name_ru'] } else { $V['name'] } }

function Set-VersionEntry {
    <# 08.10.2026: точка входа exe этой версии в тексте game.toml (Европа - 0x8001A1E8, USA v1.1 - 0x8001A1C8). По ней
       рекомпилятор начинает разбор, а игра узнаёт, что код игры пошёл (4:3 BIOS -> 16:9 мир). #>
    param([string] $Text)
    if (-not $script:Version) { return $Text }
    $v = $script:Version['entry_pc']
    if ($v) {
        if ($v -notmatch '^0x[0-9A-Fa-f]{8}$') { throw "entry_pc: $v" }
        $Text = $Text -replace '(?m)^(entry_pc\s*=\s*)"0x[0-9A-Fa-f]+"', ('${1}"' + $v + '"')
    }
    # 08.10.2026: где лежит и сколько занимает код exe версии - итальянская и испанская грузятся с 0x80010000, у испанской
    # код 0x9A000 (USA v1.1 и английская Европа - 0x80018000 и 0x99800). По ним рекомпилятор читает exe.
    foreach ($key in 'load_address', 'text_size') {
        $kv = $script:Version[$key]
        if (-not $kv) { continue }
        if ($kv -notmatch '^0x[0-9A-Fa-f]{8}$') { throw "${key}: $kv" }
        $Text = $Text -replace ('(?m)^(' + $key + '\s*=\s*)"0x[0-9A-Fa-f]+"'), ('${1}"' + $kv + '"')
    }
    # ID диска: по нему игра при запуске сверяет образ (иначе окно «на диске нет ожидаемого ID игры»); у версии - имя её
    # exe на диске (Европа - SLES_022.07). Имя файла exe в проекте и в data\ остаётся SLUS_009.22.
    $id = $script:Version['exe']
    if ($id -and $id -match '^[A-Za-z0-9_.\-]+$') { $Text = $Text -replace '(?m)^(id\s*=\s*)"[^"\r\n]*"', ('${1}"' + $id + '"') }
    return $Text
}

function Find-SubchannelFile {
    <# 08.10.2026: файл служебного канала диска (.sbi / .lsd) рядом с образом - там же и так же, как его ищет игра
       (cdrom.c, subq_load): "<образ>.sbi", "<образ без (Track N)>.sbi", то же с .lsd. Путь или $null. #>
    param([string] $Disc, [string] $Exe = '')
    # 24.10.2026 (игрок: немецкий диск, рядом «SLES_022.09.sbi» - не находился; «и по имени, и по внутренностям»): сначала .sbi
    # этой папки с CRC32 файла redump этого диска (любое имя), потом по имени образа, потом единственный .sbi/.lsd папки
    # (игра сверит его и скажет, если он от другого диска).
    $dir = Split-Path -Parent $Disc
    $all = @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -eq '.sbi' -or $_.Extension -eq '.lsd' })
    $crc = $null
    if ($Exe -and $script:SubqCrc.ContainsKey($Exe)) { $crc = $script:SubqCrc[$Exe] }
    if ($crc) {
        foreach ($f in $all) {
            if ($f.Extension -eq '.sbi' -and $f.Length -le 65536 -and (Get-Crc32Hex $f.FullName) -eq $crc) { return $f.FullName }
        }
    }
    foreach ($b in @(Get-SubchannelBases $Disc)) {
        foreach ($e in '.sbi', '.lsd') {
            $p = Join-Path $dir ($b + $e)
            if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
        }
    }
    if ($all.Count -eq 1) { return $all[0].FullName }
    return $null
}

# CRC32 файлов .sbi redump пяти дисков с LibCrypt (как k_subq_known в cdrom.c)
$script:SubqCrc = @{ 'SLES_022.07' = 'C9C7AB72'; 'SLES_022.08' = 'AAC2D94D'; 'SLES_022.09' = 'C3F2CA64'; 'SLES_022.10' = '2BEBA179'; 'SLES_022.11' = '7BE522A7' }

function Get-Crc32Hex {
    param([string] $Path)
    $c = [uint64]4294967295
    foreach ($x in [IO.File]::ReadAllBytes($Path)) {
        $c = $c -bxor [uint64]$x
        for ($k = 0; $k -lt 8; $k++) {
            if (($c -band [uint64]1) -ne 0) { $c = ($c -shr 1) -bxor [uint64]3988292384 } else { $c = $c -shr 1 }
        }
    }
    return ('{0:X8}' -f ($c -bxor [uint64]4294967295))
}

function Get-SubchannelBases {
    param([string] $Disc)
    $stem = [IO.Path]::GetFileNameWithoutExtension($Disc)
    $list = @()
    if ($stem -match '^(.*\S) \(Track [^)]+\)$') { $list += $Matches[1] }   # redump: «<игра> (Track 1).bin» и «<игра>.sbi»
    $list += $stem
    return $list
}

function Find-GameVersion {
    <# Какой это образ: sha256 исполняемого файла игры из него против списка версий. Незнакомый - отказ сразу, до
       скачивания и сборки. #>
    param($Tools)
    Start-Step (L "Recognising the disc image" "Распознаю образ игры")
    $all = @(Get-Versions)
    if ($all.Count -eq 0) { throw (L "The installer package has no list of game versions (mod\versions)." "В пакете установщика нет списка версий игры (mod\versions).") }
    $tmp = Join-Path $env:TEMP ('dcme_exe_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Dir $tmp
    $hash = ''
    try {
        foreach ($exeName in @($all | ForEach-Object { $_['exe'] } | Sort-Object -Unique)) {
            $out = Join-Path $tmp $exeName
            Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\dc_extract\extract_exe.py'), $DiscPath, $out, $exeName) -AllowFailure -Quiet
            if (-not (Test-Path -LiteralPath $out -PathType Leaf)) { continue }
            $hash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash.ToLowerInvariant()
            foreach ($v in $all) {
                if ($v['exe'] -eq $exeName -and $v['exe_sha256'] -eq $hash) {
                    Write-Log ((L "disc image:" "образ:") + " " + (Get-VersionName $v)) 'ok'
                    if ($v['subchannel'] -eq 'sbi') {
                        # LibCrypt: без служебного канала игра встаёт на первом экране (ключ не сходится)
                        $sbi = Find-SubchannelFile $DiscPath -Exe $v['exe']
                        if (-not $sbi) {
                            $want = Join-Path (Split-Path -Parent $DiscPath) ((@(Get-SubchannelBases $DiscPath))[0] + '.sbi')
                            throw (L "This disc is protected by LibCrypt: the game checks data kept in the disc's subchannel, and a .bin image does not hold it. Put the subchannel file of this disc next to the image: $want (the .sbi file of the disc dump, the same one the DuckStation emulator uses)." "Этот диск защищён LibCrypt: игра сверяет данные служебного канала диска (subchannel), а в образе .bin их нет. Положи файл служебного канала этого диска рядом с образом: $want (файл .sbi от дампа диска, тот же, что берёт эмулятор DuckStation).")
                        }
                        Write-Log ((L "subchannel file:" "служебный канал:") + " $sbi") 'ok'
                    }
                    return $v
                }
            }
        }
    } finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
    if ($hash) { Write-Log ("SLUS_009.22 sha256 $hash") }
    throw (L "Unknown disc image." "Неизвестный образ игры.")      # 08.10.2026 (игрок): без списка, он в README
}

# ──────────────────────────── шаги установки ─────────────────────────────
function Step-Framework {
    param([string] $SrcRoot)
    Start-Step (L "Recompiler framework (psxrecomp)" "Каркас рекомпилятора psxrecomp")

    # Архив держим рядом с исходниками: повторная установка не качает его
    # заново, а распаковка всё равно делается с нуля (см. ниже).
    $zip = Join-Path $SrcRoot "$PsxRecompDir.zip"

    if ($RecompZip) {
        # Пользователь скачал архив сам. Принимаем и файл под любым именем:
        # браузер мог сохранить его как «psxrecomp-<коммит>.zip» или как «<коммит>.zip».
        if (-not (Test-Path -LiteralPath $RecompZip -PathType Leaf)) {
            throw (L "Recompiler archive not found: $RecompZip" "Архив рекомпилятора не найден: $RecompZip")
        }
        Copy-Item -LiteralPath $RecompZip -Destination $zip -Force
        Write-Log ((L "using the archive you downloaded:" "беру архив, скачанный вручную:") + " $RecompZip") 'ok'
    }
    elseif (Test-Path $zip) {
        Write-Log (L "archive already downloaded, skipping" "архив уже скачан, пропускаю") 'ok'
    }
    else {
        Get-File $PsxRecompUrl $zip
    }

    # Распаковка ВСЕГДА с нуля. Патч ложится только на нетронутое дерево, а
    # проверять «не наложен ли он уже» нечем: раньше это делал `git checkout`,
    # теперь чистое состояние даёт сам архив.
    $dir = Join-Path $SrcRoot 'psxrecomp'
    Remove-Tree $dir
    if (Test-Path -LiteralPath $dir) {
        throw (L "Could not remove the old build folder $dir - delete it and run setup again." "Не удалось убрать старую папку сборки $dir - удали её и запусти установку заново.")
    }
    # 08.10.2026: распаковка - во временную папку с коротким путём. Самый длинный файл архива - 171 символ от его
    # корня, и под длинной папкой установки (от ~65 символов) Expand-Archive в Windows PowerShell упирался в предел
    # пути 260 («архив не распаковался»). В .build-src\psxrecomp те же файлы на 51 символ короче.
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('dcme_fw_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    New-Dir $tmp
    try { Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force }
    catch {
        # Битый архив не оставляем в кэше: иначе следующий запуск «пропустит
        # скачивание» и упрётся ровно в ту же ошибку.
        Remove-Item $zip -Force -ErrorAction SilentlyContinue
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        throw (L "The recompiler archive did not unpack - it is damaged or incomplete. Run setup again." "Архив рекомпилятора не распаковался - он повреждён или скачан не полностью. Запусти установку заново.")
    }

    $inner = Join-Path $tmp $PsxRecompDir
    if (-not (Test-Path $inner)) {
        # GitHub кладёт всё в одну папку с именем коммита. Если её нет, значит
        # архив не тот: например, скачана ветка main вместо закреплённого коммита.
        $got = (Get-ChildItem $tmp -Directory | ForEach-Object { $_.Name }) -join ', '
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        throw (L "Wrong archive: expected the folder $PsxRecompDir inside, found: $got. Download the pinned commit: $PsxRecompUrl" `
                 "Не тот архив: внутри ожидалась папка $PsxRecompDir, а лежит: $got. Нужен закреплённый коммит: $PsxRecompUrl")
    }
    # Move-Item не переносит папку между дисками (временная папка - на системном, игра - где угодно)
    if ([IO.Path]::GetPathRoot($inner) -eq [IO.Path]::GetPathRoot($dir)) { Move-Item $inner $dir }
    else { Copy-Item $inner $dir -Recurse -Force }
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    Write-Log ((L "psxrecomp at commit" "psxrecomp на коммите") + " $PsxRecompCommit") 'ok'
    return $dir
}

function Step-ApplyMod {
    param($Tools, [string] $FrameworkDir)
    Start-Step (L "Applying the mod to the framework" "Наложение мода на каркас")

    # Позиционный патч вместо unified diff: правки заданы номерами строк, а не
    # кусками чужого кода, поэтому в поставке мода нет ни одной строки каркаса.
    # Целостность исходника проверяется sha256 каждого файла — если коммит не
    # тот, патч честно откажется вместо тихой порчи дерева.
    $patch = Join-Path $ModDir 'patches\psxrecomp-dcme.dcpatch'
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\posdiff.py'), 'apply', $patch, $FrameworkDir)
    $pf = @(Get-Content $patch -Encoding UTF8 | Where-Object { $_ -like 'F *' }).Count
    Write-Log (L "framework patch applied ($pf files)" "патч правок каркаса наложен (файлов: $pf)") 'ok'

    Copy-Item (Join-Path $ModDir 'src\runtime\*')  (Join-Path $FrameworkDir 'runtime\src')     -Force
    Copy-Item (Join-Path $ModDir 'src\include\*')  (Join-Path $FrameworkDir 'runtime\include') -Force
    $n = @(Get-ChildItem (Join-Path $ModDir 'src\runtime') -File).Count +
         @(Get-ChildItem (Join-Path $ModDir 'src\include') -File).Count
    Write-Log ((L "mod runtime files copied" "наши файлы рантайма скопированы") + " ($n)") 'ok'

    # stb_truetype (Sean Barrett, public domain) — на нём растеризуется шрифт
    # мода. Раньше брали копию из состава Dear ImGui, из-за чего шрифт тянул за
    # собой отладочную библиотеку; теперь он от неё не зависит.
    New-Dir (Join-Path $FrameworkDir 'runtime\deps\stb')
    Copy-Item (Join-Path $ModDir 'deps\stb\stb_truetype.h') (Join-Path $FrameworkDir 'runtime\deps\stb') -Force
    (Get-Item (Join-Path $FrameworkDir 'runtime\deps\stb\stb_truetype.h')).LastWriteTime = Get-Date

    # 08.10.2026: другая версия игры - адреса рантайма по её карте и её правки (тот же позиционный формат: только наши
    # байты; sha256 каждого файла сверяется с деревом USA v1.1, которое получилось строками выше)
    if ($script:Version -and $script:Version['runtime_patch']) {
        $vp = Join-Path $script:Version['dir'] $script:Version['runtime_patch']
        Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\posdiff.py'), 'apply', $vp, $FrameworkDir)
        $vf = @(Get-Content $vp -Encoding UTF8 | Where-Object { $_ -like 'F *' }).Count
        Write-Log ((L "runtime adapted to" "рантайм под") + " " + (Get-VersionName $script:Version) + " (" + (L "files" "файлов") + ": $vf)") 'ok'
    }
}


function Step-Sdl2 {
    param([string] $SrcRoot)
    Start-Step 'SDL2'
    # runtime.cmake ищет пакет по маске <каркас>/../sdl2-msvc/SDL2-* — папка
    # обязана лежать рядом с psxrecomp и называться именно так.
    $sdlRoot = Join-Path $SrcRoot 'sdl2-msvc'
    $target  = Join-Path $sdlRoot "SDL2-$Sdl2Version"
    if (Test-Path (Join-Path $target 'lib\x64\SDL2.lib')) { Write-Log (L "already unpacked, skipping" "уже распакован, пропускаю") 'ok'; return $target }
    New-Dir $sdlRoot
    # 10.10.2026: SDL2 (лицензия zlib) едет в пакете мода (tools\make_sdl2_bundle.py пакета) - скачивать больше нечего
    $bundled = Join-Path $ModDir "sdl2\SDL2-$Sdl2Version"
    if (Test-Path -LiteralPath (Join-Path $bundled 'lib\x64\SDL2.lib')) {
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }   # недокопированная прошлым запуском
        Copy-Item -LiteralPath $bundled -Destination $sdlRoot -Recurse -Force
        if (-not (Test-Path (Join-Path $target 'lib\x64\SDL2.lib'))) { throw (L "SDL2 was not copied: no $target\lib\x64\SDL2.lib" "SDL2 не скопирован: нет $target\lib\x64\SDL2.lib") }
        Tool-State 'sdl2' 'bundled'
        Write-Log ("SDL2 $Sdl2Version " + (L "included with the mod" "из пакета мода"))         # только в журнал
        return $target
    }
    $zip = Join-Path $env:TEMP "SDL2-devel-$Sdl2Version-VC.zip"
    Get-File $Sdl2Url $zip
    Expand-Archive -Path $zip -DestinationPath $sdlRoot -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path (Join-Path $target 'lib\x64\SDL2.lib'))) { throw (L "SDL2 unpacked to the wrong place: no $target\lib\x64\SDL2.lib" "SDL2 распакован не туда: нет $target\lib\x64\SDL2.lib") }
    Write-Log ("SDL2 $Sdl2Version " + (L "ready" "готов")) 'ok'
    return $target
}

function Step-Project {
    param([string] $SrcRoot)
    Start-Step (L "Game project" "Проект игры")
    $proj = Join-Path $SrcRoot 'DinoCrisisME'
    New-Dir $proj; New-Dir (Join-Path $proj 'seeds'); New-Dir (Join-Path $proj 'generated')
    Copy-Item (Join-Path $ModDir 'project\CMakeLists.txt') $proj -Force
    $projToml = Set-VersionEntry ([IO.File]::ReadAllText((Join-Path $ModDir 'project\game.toml')))
    [IO.File]::WriteAllText((Join-Path $proj 'game.toml'), $projToml, (New-Object System.Text.UTF8Encoding $false))
    Copy-Item (Join-Path $ModDir 'project\dc_icon.ico')    $proj -Force
    # 11.10.2026: манифест UTF-8 программы (пути с кириллицей) - его берёт .rc сборки
    if (Test-Path -LiteralPath (Join-Path $ModDir 'project\dc_utf8.manifest')) {
        Copy-Item -LiteralPath (Join-Path $ModDir 'project\dc_utf8.manifest') $proj -Force
    }
    Copy-Item (Join-Path $ModDir 'project\seeds\*')        (Join-Path $proj 'seeds') -Force
    Write-Log ((L "project created at" "проект собран в") + " $proj") 'ok'
    return $proj
}

function Step-ExtractExe {
    param($Tools, [string] $ProjDir)
    Start-Step (L "Game executable from your disc image" "Исполняемый файл игры из твоего образа")
    $out = Join-Path $ProjDir 'SLUS_009.22'
    # 08.10.2026: у другой версии игры исполняемый файл на диске называется по-своему (Европа - SLES_022.07); в проекте он
    # всё равно SLUS_009.22 - так его ждут рекомпилятор, game.toml и игра (data\SLUS_009.22)
    $exeOnDisc = 'SLUS_009.22'
    if ($script:Version -and $script:Version['exe']) { $exeOnDisc = $script:Version['exe'] }
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\dc_extract\extract_exe.py'), $DiscPath, $out, $exeOnDisc)
    if (-not (Test-Path $out)) { throw (L "SLUS_009.22 was not extracted." "SLUS_009.22 не извлёкся.") }
    if ($script:Version -and (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash.ToLowerInvariant() -ne $script:Version['exe_sha256']) {
        throw (L "SLUS_009.22 changed since the image was recognised - is the disc image being replaced?" "SLUS_009.22 не тот, что при распознавании образа - образ подменили?")
    }
    Write-Log ("SLUS_009.22: " + ("{0:N0} " -f (Get-Item $out).Length) + (L "bytes" "байт")) 'ok'
}

function Step-Recompile {
    param($Tools, [string] $FrameworkDir, [string] $ProjDir)
    Start-Step (L "Recompiling the game to C" "Рекомпиляция игры в C")
    $recompSrc   = Join-Path $FrameworkDir 'recompiler'
    $recompBuild = Join-Path $recompSrc 'build'
    if ($Tools.Llvm) {
        # 10.10.2026: Clang из пакета, по записанному плану (rebuild.ps1, шаг 8c - там же, какие ключи и почему).
        # Вывод проверен: генкод байт в байт как у сборки MSVC.
        $exe = Join-Path $recompBuild 'psxrecomp-game.exe'
        # 11.10.2026: всегда заново (5 с) - рекомпилятор от прошлой, прерванной установки в ту же папку мог быть собран без
        # манифеста UTF-8 и снова упал бы на пути с кириллицей
        if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force }
        Invoke-Plan $Tools 'recompiler.plan.json' (Split-Path -Parent $FrameworkDir) 256 0
    } else {
        $exe = Join-Path $recompBuild 'Release\psxrecomp-game.exe'
        if (-not (Test-Path $exe)) {
            Invoke-Tool -File $Tools.CMake -Arguments @('-S',$recompSrc,'-B',$recompBuild,'-A','x64')
            Invoke-Tool -File $Tools.CMake -Arguments @('--build',$recompBuild,'--config','Release','--target','psxrecomp-game')
        } else { Write-Log (L "recompiler already built" "рекомпилятор уже собран") 'ok' }
    }
    if (-not (Test-Path $exe)) { throw (L "Recompiler did not build: no $exe" "Рекомпилятор не собрался: нет $exe") }

    Invoke-Tool -File $exe -Arguments @('--config','game.toml') -WorkDir $ProjDir -Quiet
    $full = Join-Path $ProjDir 'generated\SLUS_009.22_full.c'
    if (-not (Test-Path $full)) { throw (L "Recompiler produced no generated\SLUS_009.22_full.c" "Рекомпилятор не выдал generated\SLUS_009.22_full.c") }
    Write-Log ((L "generated" "сгенерировано") + (" {0:N0} " -f (Get-Item $full).Length) + (L "bytes of C code" "байт кода")) 'ok'
    return $full
}

function Step-PatchGenerated {
    param($Tools, [string] $ProjDir, [string] $FullC)
    Start-Step (L "Patching generated code (PGXP + mod hooks)" "Правка сгенерированного кода (PGXP + хуки мода)")
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\pgxp_transform.py'), $FullC)
    # Тоже позиционный патч (формат 2, 17.09.2026): наши строки хуков, а там, где
    # хук повторяет выражение инструкции игры, — наши байты и ссылки на место в
    # ТВОЁМ генкоде. Ни одного выражения рекомпилированной игры в патче нет.
    # 08.10.2026: у другой версии игры свой патч хуков - тот же набор правок, перенесённый на код из её exe
    $hooks = Join-Path $ModDir 'generated\dcme-hooks.dcpatch'
    if ($script:Version -and $script:Version['hooks_patch']) { $hooks = Join-Path $script:Version['dir'] $script:Version['hooks_patch'] }
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\posdiff.py'), 'apply', $hooks, $ProjDir)
    # Обёртки чтения вершин для Z-fight ставит правило, а не готовые строки
    # (17.09.2026): скрипт находит в генкоде загрузки вершин в GTE и оборачивает их.
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\zf_vxy_transform.py'), $FullC)
    Write-Log (L "mod hooks injected into the generated code" "хуки мода вживлены в генкод") 'ok'
    # Резервная копия рекомпилятора мешает повторной установке: патч уже наложен,
    # а pgxp_transform при следующем запуске возьмёт её за исходник.
    Remove-Item (Join-Path $ProjDir 'generated\SLUS_009.22_full.c.pgxp_orig') -Force -ErrorAction SilentlyContinue
}

function Step-BuildRuntime {
    param($Tools, [string] $FrameworkDir, [string] $ProjDir)
    Start-Step (L "Building the runtime (longest step, 5-20 minutes)" "Сборка рантайма (самый долгий шаг, 5-20 минут)")
    $build = Join-Path $ProjDir 'build'
    if ($Tools.Llvm) {
        # 10.10.2026: Clang из пакета, по записанному плану (rebuild.ps1, шаг 8c): SDL2 из <каркас>/../sdl2-msvc, -O2,
        # оконная подсистема, ресурс .rc (значок и пак данных) и вшитые libc++ - как настроены там и в
        # mod\project\CMakeLists.txt.
        Invoke-Plan $Tools 'game.plan.json' (Split-Path -Parent $FrameworkDir) 384 1536
        $exe = Join-Path $build 'DinoCrisisME.exe'
    } else {
        Invoke-Tool -File $Tools.CMake -Arguments @('-S',$ProjDir,'-B',$build,'-A','x64',"-DPSXRECOMP_ROOT=$FrameworkDir")
        Invoke-Tool -File $Tools.CMake -Arguments @('--build',$build,'--config','RelWithDebInfo','--target','DinoCrisisME')
        $exe = Join-Path $build 'RelWithDebInfo\DinoCrisisME.exe'
    }
    if (-not (Test-Path $exe)) { throw (L "Runtime did not build: no $exe" "Рантайм не собрался: нет $exe") }
    Write-Log ("DinoCrisisME.exe: " + ("{0:N0} " -f (Get-Item $exe).Length) + (L "bytes" "байт")) 'ok'
    return $exe
}

function Step-Rooms {
    param($Tools, [string] $ProjDir)
    Start-Step (L "Unpacking rooms from your disc image" "Распаковка комнат из твоего образа")
    # 17.09.2026: комнаты ложатся не в data\, а в дерево сборки — оттуда они
    # уходят внутрь программы (Step-PackData). Имена внутри пакета — те же пути,
    # что были от папки data: exported_rooms/library/room_NNN.dcroom.
    $rooms = Join-Path $ProjDir 'dcme_data\exported_rooms'
    if (Test-Path $rooms) { Remove-Item $rooms -Recurse -Force }
    # --library-only: без него экспортёр кладёт рядом с библиотекой ещё по две
    # копии каждой комнаты (ST*.obj для просмотра в редакторе и ST*.dcroom с
    # именем ST). Игра читает только library/room_*.dcroom и два списка.
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\dc_extract\export_rooms.py'), '--library-only', $DiscPath, $rooms) -Quiet
    $lib = Join-Path $rooms 'library'
    $n = @(Get-ChildItem $lib -Filter 'room_*.dcroom' -ErrorAction SilentlyContinue).Count
    if ($n -lt 100) { throw (L "Only $n rooms unpacked - is the disc image damaged?" "Из образа распаковалось только $n комнат - образ повреждён?") }
    Write-Log ((L "rooms unpacked:" "комнат распаковано:") + " $n") 'ok'

    # Камерная коллизия — ручная работа автора мода, она в поставке есть.
    # Только *.obj: README рядом с ними — для нас, в программу он не нужен.
    $coll = Join-Path $rooms 'collision'
    New-Dir $coll
    Copy-Item (Join-Path $ModDir 'collision\*.obj') $coll -Force
    # @() обязательна: Get-ChildItem на пустой папке отдаёт $null, а у него под
    # Set-StrictMode нет свойства Count — вместо понятного «0 файлов» установка
    # падала бы с «The property 'Count' cannot be found on this object».
    $nc = @(Get-ChildItem $coll -File -Filter '*.obj' -ErrorAction SilentlyContinue).Count
    if ($nc -eq 0) { throw (L "Camera collision is missing from the installer package." "В пакете установщика нет камерной коллизии.") }
    Write-Log ((L "camera collision:" "камерная коллизия:") + " $nc " + (L "files" "файлов")) 'ok'
}

function Step-PackData {
    param($Tools, [string] $ProjDir)
    Start-Step (L "Packing the game data into the program" "Данные игры - внутрь программы")
    # 17.09.2026 (игрок: «dc_user.cfg, input.ini, SLUS_009.22 оставляй, всё
    # остальное зашивай в exe»). Всё, что раньше лежало файлами в data\, кроме
    # этих трёх, собирается в dcme_data.pak рядом с CMakeLists.txt; сборка
    # кладёт его в DinoCrisisME.exe ресурсом DCME_DATA, игра читает его
    # (runtime/src/dc_pak.c) под прежними именами файлов.
    $stage = Join-Path $ProjDir 'dcme_data'
    New-Dir $stage

    # Стартовые настройки и подгонка комнат. dc_user.cfg и input.ini остаются
    # файлами: их меняет игрок (меню настроек, раскладка).
    # V3 (04.10.2026): и подгонка, появившаяся после V1 — отметки текстур (двусторонние, материалы, свечение,
    # подсветка), картинка (SSAO/MSAA, вспышки, свечение), тени по комнатам, кривые переходов камеры.
    foreach ($name in @('dc_env.cfg', 'dc_items_user.cfg', 'dc_decals_user.cfg', 'dc_zfsel_user.cfg', 'dc_hiprooms_user.cfg',
                        'dc_twosided_user.cfg', 'dc_material_user.cfg', 'dc_glow_user.cfg', 'dc_highlight_user.cfg',
                        'dc_post_user.cfg', 'dc_shadows_user.cfg', 'dc_cam_curves.cfg', 'dc_zfopt_user.cfg',
                        'dc_strafe_live.cfg')) {    # 13.10.2026: ходьба в прицеле (режимы F7)
        $from = Join-Path $ModDir "config\$name"
        if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { throw ((L "Missing from the installer package:" "В пакете установщика нет файла:") + " config\$name") }
        Copy-Item -LiteralPath $from $stage -Force
    }
    # V3: наши данные — шейдеры, стойки прицела, картинки экранов и меню (embed\, их отдаёт dc_embedded()) и лица
    # персонажей (assets\faces) — тоже в пакет; раньше первые три были массивами C в dc_embedded.c.
    $embed = Join-Path $ModDir 'assets\embed'
    $ne = @(Get-ChildItem -LiteralPath $embed -File -ErrorAction SilentlyContinue).Count
    if ($ne -lt 100) { throw ((L "Missing from the installer package:" "В пакете установщика нет:") + " assets\embed ($ne)") }
    New-Dir (Join-Path $stage 'embed')
    Copy-Item (Join-Path $embed '*') (Join-Path $stage 'embed') -Force
    $faces = Join-Path $ModDir 'assets\faces'
    if (Test-Path -LiteralPath $faces -PathType Container) {
        New-Dir (Join-Path $stage 'assets\faces')
        Copy-Item (Join-Path $faces '*.bin') (Join-Path $stage 'assets\faces') -Force
    }
    # 08.10.2026: рот под озвучку другой версии - её файлы лиц поверх (те же имена)
    if ($script:Version -and $script:Version['faces']) {
        $vf = Join-Path $script:Version['dir'] $script:Version['faces']
        if (Test-Path -LiteralPath $vf -PathType Container) {
            New-Dir (Join-Path $stage 'assets\faces')
            Copy-Item (Join-Path $vf '*.bin') (Join-Path $stage 'assets\faces') -Force
            Write-Log ((L "faces for the voice of this version:" "лица под озвучку этой версии:") + " " +
                       @(Get-ChildItem -LiteralPath $vf -File -Filter '*.bin').Count) 'ok'
        }
    }
    Write-Log ((L "our data:" "наши данные:") + " embed $ne, " + (L "faces" "лица") + " " +
               @(Get-ChildItem (Join-Path $stage 'assets\faces') -File -ErrorAction SilentlyContinue).Count) 'ok'
    # Лицензия мода и список стороннего — тоже внутри программы.
    foreach ($doc in @('LICENSE.txt', 'THIRD-PARTY.txt')) {
        $from = Join-Path $ModDir $doc
        if (Test-Path -LiteralPath $from -PathType Leaf) { Copy-Item -LiteralPath $from $stage -Force }
    }

    # game.toml указывает на образ пользователя там, где он лежит: копировать
    # 400+ МБ незачем. Путь пишем через «/»: в строке TOML обратный слэш —
    # символ экранирования. «$» в пути удваивается: в строке замены -replace он
    # означает ссылку на группу. Файл — UTF-8 без BOM.
    $discToml = $DiscPath -replace '\\', '/'
    $line = ('disc = "' + $discToml + '"').Replace('$', '$$')
    $toml = Set-VersionEntry ([IO.File]::ReadAllText((Join-Path $ModDir 'project\game.toml')))
    if ($toml -match '(?m)^disc\s*=') { $toml = $toml -replace '(?m)^disc\s*=[^\r\n]*', $line }
    else                              { $toml = $toml -replace '(?m)^(exe\s*=[^\r\n]*)', ('$1' + "`r`n" + $line) }
    [IO.File]::WriteAllText((Join-Path $stage 'game.toml'), $toml, (New-Object System.Text.UTF8Encoding $false))

    $pak = Join-Path $ProjDir 'dcme_data.pak'
    Invoke-Tool -File $Tools.Python -Arguments @((Join-Path $ModDir 'tools\pack_data.py'), $stage, $pak)
    if (-not (Test-Path -LiteralPath $pak -PathType Leaf)) { throw (L "The game data was not packed." "Данные игры не упаковались.") }
    Write-Log ("dcme_data.pak: " + ("{0:N0} " -f (Get-Item -LiteralPath $pak).Length) + (L "bytes" "байт")) 'ok'
}

function Step-Layout {
    param([string] $GameDir, [string] $RuntimeExe, [string] $Sdl2Dir, [string] $ProjDir)
    Start-Step (L "Laying out the game folder" "Раскладка готовой папки")
    # 17.09.2026 (игрок: «убери все лишние файлы, которые остаются после
    # установки»). Рядом с exe — только сам exe, SDL2.dll (Windows ищет его в
    # папке программы) и data\: игра работает из этой папки
    # (runtime/src/dc_boot.c).
    # 17.09.2026 (игрок: «dc_user.cfg, input.ini, SLUS_009.22 оставляй, всё
    # остальное зашивай в exe; это релиз мода, тут не должно быть ни дебага, ни
    # отладочных файлов»). В data\ — только эти три файла; карты памяти и
    # быстрые сохранения игра заводит там сама. Журнала у игры больше нет.
    $Data = Join-Path $GameDir 'data'

    # Папка прошлой версии. V0 (09.09) держал всё рядом с exe, V1 (17.09) — в
    # data\. Сейвы игрока переезжают в data\, наши старые файлы уходят — только
    # по точным именам и только если это действительно папка мода: рядом лежит
    # старый exe вместе с game.toml (V0) или data\game.toml (V1).
    $hasExe = (Test-Path -LiteralPath (Join-Path $GameDir 'DinoCrisisME.exe') -PathType Leaf) -or
              (Test-Path -LiteralPath (Join-Path $GameDir 'DinoCrisisMCM.exe') -PathType Leaf)
    $wasV0 = $hasExe -and (Test-Path -LiteralPath (Join-Path $GameDir 'game.toml') -PathType Leaf)
    $wasV1 = $hasExe -and (Test-Path -LiteralPath (Join-Path $Data 'game.toml') -PathType Leaf)
    New-Dir $GameDir; New-Dir $Data
    $moved = 0; $removed = 0
    if ($wasV0) {
        $saves = @('card1.mcd', 'card2.mcd') +
                 @(Get-ChildItem -LiteralPath $GameDir -File -Filter 'quicksave*.dcss' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        foreach ($name in $saves) {
            $from = Join-Path $GameDir $name
            if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { continue }
            $to = Join-Path $Data $name
            if (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $from -Force }   # в data\ уже новее
            else { Move-Item -LiteralPath $from -Destination $to; $moved++ }
        }
        foreach ($name in @('dc_user.cfg', 'input.ini', 'SLUS_009.22', 'DinoCrisisMCM.exe')) {
            $p = Join-Path $GameDir $name
            if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p -Force; $removed++ }
        }
    }
    if ($wasV0 -or $wasV1) {
        # То, что теперь внутри программы, и то, что писала отладка прошлых
        # версий (журнал, отчёты, клавиши отладки: 1/2 — dc_debug.cfg, F3 —
        # room_dump.obj, F6 — папка pt_dump). Метка экранов запуска живёт теперь
        # в dc_user.cfg (boot.seen), а он ставится заново — экраны покажутся.
        $gone = @('dc_env.cfg', 'dc_items_user.cfg', 'dc_decals_user.cfg', 'dc_zfsel_user.cfg', 'dc_hiprooms_user.cfg',
                  'dc_zfopt_user.cfg', 'dc_plates_user.cfg', 'dc_debug.cfg', 'dc_boot_seen.cfg', 'game.toml', 'disc.cfg',
                  'play.txt', 'psx_last_run_report.json', 'starvation_dump.jsonl',
                  'psx_crash.txt', 'room_dump.obj', 'camera_boxes.txt')
        foreach ($dir in @($GameDir, $Data)) {
            foreach ($name in $gone) {
                $p = Join-Path $dir $name
                if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p -Force; $removed++ }
            }
        }
        # 18.09.2026 (игрок: «файлы лицензии нужные верни в игру установленную»):
        # LICENSE.txt и THIRD-PARTY.txt снова лежат рядом с игрой, поэтому из
        # общей чистки они убраны; в data\ их быть не должно — оттуда удаляем.
        foreach ($name in @('LICENSE.txt', 'THIRD-PARTY.txt')) {
            $p = Join-Path $Data $name
            if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p -Force; $removed++ }
        }
        foreach ($sub in @('fonts', 'exported_rooms', 'pt_dump')) {
            $p = Join-Path $Data $sub
            if (Test-Path -LiteralPath $p -PathType Container) { Remove-Item -LiteralPath $p -Recurse -Force; $removed++ }
        }
        Write-Log ((L "previous version: saves moved into data\" "прошлая версия: сейвы перенесены в data\") +
                   " ($moved), " + (L "old files removed" "старые файлы удалены") + " ($removed)") 'ok'
    }

    Copy-Item $RuntimeExe $GameDir -Force
    Copy-Item (Join-Path $Sdl2Dir 'lib\x64\SDL2.dll') $GameDir -Force

    # V3 (04.10.2026, игрок: «нужна папка mods с подпапками в директории, куда игра установится»; «всё это — подпапки
    # mods»): игра сама смотрит mods при запуске — файл с именем файла игры подменяет его (runtime/src/dc_mods.c,
    # наши комнаты и коллизия — dc_pak.c). Опций в игре нет. Уже лежащее в mods при переустановке не трогаем.
    $mods = Join-Path $GameDir 'mods'
    # V4 (05.10.2026, игрок: «посмотри для примера на структуру папок в PC-версии… касается mods и source»): movie,
    # sound\bgm (вся музыка ME*, MF*, MR*, MS*, MT*, M__*), sound\se (STAGE00.XAS), sound\voice (речь STAGE01..06).
    # Игра читает и прежние папки V3 (dc_mods.c) — пустые из них убираем, с файлами игрока не трогаем.
    # V6 (06.10.2026): weapons — оружие каталога приложения (подмена моделей из glTF по index.txt, dc_meshrep.cpp).
    foreach ($sub in @('rooms', 'doors', 'characters', 'weapons', 'items', 'screens', 'movie', 'sound\bgm', 'sound\se',
                       'sound\voice', 'textures', 'collision', 'other')) {
        New-Dir (Join-Path $mods $sub)
    }
    foreach ($old in @('sounds\effects', 'sounds\stage', 'sounds\speech', 'sounds\music', 'sounds', 'videos')) {
        $p = Join-Path $mods $old
        if ((Test-Path -LiteralPath $p -PathType Container) -and -not (Get-ChildItem -LiteralPath $p -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $p -Force
        }
    }
    Write-Log ((L "mods folder:" "папка mods:") + " $mods") 'ok'

    # 06.10.2026: mods\names.txt - каждый меш игры по имени; модель под своим именем в mods игра берёт сама, без
    # конвертации (dc_meshrep.cpp). Игрок: «установщик игры и инструмент для моддинга идут раздельно» - приложения для
    # модов в установке игры больше нет (оно качается отдельно), список собран при сборке пакета (rebuild.ps1, шаг 2:
    # одинаков для любого образа USA v1.1, ключи - хеши). Приложение для модов перезаписывает его при каждом запуске.
    $names = Join-Path $ModDir 'mods\names.txt'
    if (Test-Path -LiteralPath $names -PathType Leaf) {
        Copy-Item -LiteralPath $names (Join-Path $mods 'names.txt') -Force
        Write-Log ((L "model names for mods:" "имена моделей для mods:") + " $mods\names.txt") 'ok'
    } else { Write-Log ((L "not in the package:" "нет в пакете:") + " mods\names.txt") 'warn' }

    # 08.10.2026: в этой папке стояла другая версия игры - её быстрые сохранения новой не годятся (снимок всей памяти,
    # а у версий она разложена по-разному). Уводим их в подпапку data\, ничего не удаляя; сейвы карт памяти не трогаем.
    $oldExe = Join-Path $Data 'SLUS_009.22'
    if ($script:Version -and (Test-Path -LiteralPath $oldExe -PathType Leaf)) {
        $oldHash = (Get-FileHash -LiteralPath $oldExe -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($oldHash -ne $script:Version['exe_sha256']) {
            $oldId = 'previous'
            foreach ($v in @(Get-Versions)) { if ($v['exe_sha256'] -eq $oldHash) { $oldId = $v['id'] } }
            # 13.10.2026: игра держит быстрые сохранения в data\quicksaves (прежние версии - прямо в data)
            $qs = @((@(Get-ChildItem -LiteralPath $Data -File -ErrorAction SilentlyContinue) +
                     @(Get-ChildItem -LiteralPath (Join-Path $Data 'quicksaves') -File -ErrorAction SilentlyContinue)) |
                    Where-Object { $_.Name -like 'quicksave*.dcss' -or $_.Name -like 'quicksave*.story' })
            if ($qs.Count -gt 0) {
                $keep = Join-Path $Data "quicksaves_$oldId"
                New-Dir $keep
                foreach ($q in $qs) { Move-Item -LiteralPath $q.FullName -Destination (Join-Path $keep $q.Name) -Force }
                Write-Log ((L "another game version was installed here: its quicksaves moved to" `
                             "здесь стояла другая версия игры: её быстрые сохранения перенесены в") + " $keep") 'warn'
            }
        }
    }

    # Шейдеры, стойки прицела, картинки меню, лица и данные игры (dcme_data.pak:
    # настройки, подгонка, game.toml, лицензии, коллизия, комнаты) — внутри
    # исполняемого файла. Файлами — только то, что меняет игрок, и исполняемый
    # файл игры из его образа (V3: плюс папка mods — её наполняет игрок).
    Copy-Item (Join-Path $ModDir 'config\dc_user.cfg')  $Data -Force
    Copy-Item (Join-Path $ModDir 'config\input.ini')    $Data -Force
    Copy-Item (Join-Path $ProjDir 'SLUS_009.22')        $Data -Force

    # 18.09.2026 (игрок: «файлы лицензии нужные верни в игру установленную
    # LICENSE.txt и THIRD-PARTY.txt»): лежат рядом с игрой, чтобы их было видно;
    # копии остаются и внутри программы (dcme_data.pak).
    foreach ($doc in @('LICENSE.txt', 'THIRD-PARTY.txt')) {
        $from = Join-Path $ModDir $doc
        if (Test-Path -LiteralPath $from -PathType Leaf) { Copy-Item -LiteralPath $from $GameDir -Force }
        else { Write-Log ((L "not in the package:" "нет в пакете:") + " $doc") 'warn' }
    }

    # 11.10.2026 (игрок: «после установки игры нужен exe деинсталляции в той же директории, который будет удалять игру»):
    # Uninstall.exe собирает build_setup.ps1 (installer\Uninstall.cs) и кладёт в пакет рядом с этим сценарием.
    $uninst = Join-Path $PSScriptRoot 'Uninstall.exe'
    if (Test-Path -LiteralPath $uninst -PathType Leaf) { Copy-Item -LiteralPath $uninst $GameDir -Force }
    else { Write-Log ((L "not in the package:" "нет в пакете:") + " Uninstall.exe") 'warn' }

    Write-Log ((L "game folder ready:" "папка игры готова:") + " $GameDir") 'ok'
}

function Step-Shortcut {
    param([string] $GameDir)
    Start-Step (L "Desktop shortcut" "Ярлык")
    if ($NoShortcut) { Write-Log (L "-NoShortcut: leaving the desktop alone" "ключ -NoShortcut: рабочий стол не трогаю") 'ok'; return }
    # ⛔ Имя цели должно быть ЛАТИНСКИМ: WScript.Shell не принимает кириллицу в
    # TargetPath («Value does not fall within the expected range»).
    # Ярлык ведёт прямо на игру: настройки запуска она теперь читает сама
    # (dc_env.cfg, runtime/src/dc_boot.c), батник-посредник больше не нужен.
    # ⛔ Имя цели обязано быть ЛАТИНСКИМ: WScript.Shell не принимает
    # кириллицу в TargetPath («Value does not fall within the expected range»).
    $bat = Join-Path $GameDir 'DinoCrisisME.exe'
    if (-not (Test-Path $bat)) { Write-Log (L "launcher not found, skipping the shortcut" "файл запуска не найден, ярлык не делаю") 'warn'; return }
    # Ярлык прошлой версии («Dino Crisis MCM»), если он вёл в эту же папку:
    # его exe удалён при раскладке, ярлык остался бы мёртвым.
    try {
        $oldLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Dino Crisis MCM.lnk'
        if (Test-Path -LiteralPath $oldLnk) {
            $sh0 = New-Object -ComObject WScript.Shell
            $t0 = $sh0.CreateShortcut($oldLnk).TargetPath
            if ($t0 -and ($t0 -ieq (Join-Path $GameDir 'DinoCrisisMCM.exe'))) { Remove-Item -LiteralPath $oldLnk -Force }
        }
    } catch { }
    try {
        $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Dino Crisis ME.lnk'
        $sh = New-Object -ComObject WScript.Shell
        $s = $sh.CreateShortcut($lnk)
        $s.TargetPath = $bat
        $s.WorkingDirectory = $GameDir
        $s.Description = 'Dino Crisis ME'
        # 18.09.2026 (игрок: «ярлык почему-то не отображается»). Значок в exe
        # целый (ресурс 1, десять кадров — проверено ExtractIconExW), пустой лист
        # на рабочем столе — залипший кэш значков проводника: ярлык создаётся
        # сразу после сборки, и оболочка успевает запомнить «значка нет». Поэтому
        # значок называем ЯВНО и просим оболочку перечитать запись.
        $s.IconLocation = "$bat,0"
        $s.Save()
        Write-Log ((L "desktop shortcut:" "ярлык на рабочем столе:") + " $lnk") 'ok'
        try {
            Add-Type -Namespace DcMe -Name Shell -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);
'@ -ErrorAction Stop
            $bytes = [System.Text.Encoding]::Unicode.GetBytes($lnk + [char]0)
            $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
            [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $mem, $bytes.Length)
            [DcMe.Shell]::SHChangeNotify(0x00002000, 0x1005, [System.IntPtr]::Zero, [System.IntPtr]::Zero)  # SHCNE_ASSOCCHANGED
            [DcMe.Shell]::SHChangeNotify(0x00002000, 0x0005, $mem, [System.IntPtr]::Zero)                   # + сам ярлык
            [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
            Write-Log (L "the shell was asked to refresh the icon" "оболочка попрошена перечитать значок") 'ok'
        } catch { Write-Log ((L "icon refresh skipped:" "обновление значка пропущено:") + " $($_.Exception.Message)") 'warn' }
    } catch { Write-Log ((L "shortcut not created:" "ярлык не создался:") + " $($_.Exception.Message)") 'warn' }
}

# ──────────────────────────────── ход дела ───────────────────────────────
try {
    Write-Log ("==== Dino Crisis ME - " + (L "mod setup" "установка мода") + " ====") 'step'
    Write-Log ((L "disc image :" "образ игры :") + " $DiscPath")
    Write-Log ((L "install to :" "куда ставим:") + " $InstallPath")

    if (-not (Test-Path -LiteralPath $DiscPath -PathType Leaf)) { throw (L "Disc image not found: $DiscPath" "Образ игры не найден: $DiscPath") }
    if ((Get-Item -LiteralPath $DiscPath).Length -lt 100MB) {
        throw (L "$DiscPath is too small for a disc image. Point at the Track 1 .bin, not the .cue." "Файл $DiscPath слишком мал для образа диска. Нужен .bin дорожки 1, а не .cue.")
    }
    if (-not (Test-Path (Join-Path $ModDir 'patches\psxrecomp-dcme.dcpatch'))) {
        throw (L "No mod folder next to the installer - unpack the whole archive." "Рядом с установщиком нет папки mod - распакуй архив целиком.")
    }

    $Tools = Test-Prerequisites
    $script:Version = Find-GameVersion $Tools
    if ($DryRun) { Write-Log (L "-DryRun: checks passed, nothing was installed." "Ключ -DryRun: проверки пройдены, установка не выполнялась.") 'ok'; return }

    # Игра ложится прямо в выбранную папку: подпапка «game» была нужна, пока
    # рядом оставалось дерево сборки. Теперь оно удаляется, и вложенность
    # только мешала бы искать исполняемый файл.
    $SrcRoot = Join-Path $InstallPath '.build-src'
    $GameDir = $InstallPath
    New-Dir $InstallPath; New-Dir $SrcRoot

    $fw    = Step-Framework $SrcRoot
    Step-ApplyMod $Tools $fw
    $sdl   = Step-Sdl2 $SrcRoot
    $proj  = Step-Project $SrcRoot
    Step-ExtractExe $Tools $proj
    $full  = Step-Recompile $Tools $fw $proj
    Step-PatchGenerated $Tools $proj $full
    Step-Rooms $Tools $proj                    # комнаты — в дерево сборки
    Step-PackData $Tools $proj                 # данные игры — в dcme_data.pak, до сборки
    $exe   = Step-BuildRuntime $Tools $fw $proj
    Step-Layout $GameDir $exe $sdl $proj
    Step-Shortcut $GameDir

    if (-not $KeepBuildTree) {
        # Убираем ВСЮ build-src, а не только объектники: там 160 МБ — каркас
        # рекомпилятора со своей документацией и 15 МБ сгенерированного кода
        # игры. Игра уже собрана, для игры это не нужно ничего, а у человека
        # рядом с модом лежала бы чужая рабочая папка.
        #
        # Ключ -KeepBuildTree оставляет её: повторная установка тогда не качает
        # и не собирает каркас заново.
        Write-Log (L "removing the build tree..." "убираю дерево сборки...")
        $before = 0
        try { $before = (Get-ChildItem $SrcRoot -Recurse -File -ErrorAction SilentlyContinue |
                         Measure-Object Length -Sum).Sum } catch { }
        Remove-Tree $SrcRoot          # 23.10.2026: и пути от 260 знаков (раньше рядом с игрой оставалась .build-src)
        Write-Log ((L "freed" "освобождено") + (" {0:N0} " -f ($before / 1MB)) + (L "MB" "МБ")) 'ok'
    }

    if ($RemoveTools) { Remove-InstalledTools }

    Write-Log ("==== " + (L "Done" "Готово") + " ====") 'step'
    Write-Log ((L "Run:" "Запуск:") + " $GameDir\DinoCrisisME.exe") 'ok'
    # Программы сборки нужны были только здесь: игра — обычный исполняемый файл.
    # 10.10: компилятор (Clang) с планами сборки, Python, Vulkan и SDL2 едут в пакете мода - ставить не нужно было
    # ничего, и сказать нечего; Build Tools - только на запасном пути (-Toolchain msvc)
    if (-not $Tools.Llvm) {
        Write-Log (L "The build tools (Visual Studio Build Tools) were only needed for setup - you can uninstall them." `
                     "Программы сборки (Visual Studio Build Tools) нужны были только для установки - их можно удалить.") 'ok'
    }
    exit 0
}
catch {
    Write-Log $_.Exception.Message 'err'
    if ($_.ScriptStackTrace) { Write-Log $_.ScriptStackTrace }
    exit 1
}
