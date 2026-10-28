# -*- coding: utf-8 -*-
"""Dino Crisis ME - the mod setup, in Python (27.10.2026).

The same work as installer\\dcme-install.ps1, step by step: the same command line, the same messages (English by
default, -Lang ru for Russian) and the same lines for the setup window (Setup.cs reads "== Step N of 14: ...", "[ok]",
"[!]", "[x]", "[tool] <key> <state>" and "[front]"). It runs on the Python that comes with the mod (mod\\tools\\python),
so no PowerShell is needed:

    mod\\tools\\python\\python.exe -I installer\\dcme_install.py -DiscPath "D:\\Dino Crisis (Track 1).bin" -InstallPath "C:\\Games\\DinoCrisisME"

27.10.2026 (the player: setup for Windows and for the Steam Deck): under Wine and Proton powershell.exe is a stub that
runs no script, while python.exe is an ordinary Windows program. The same day (the player: "switch setup to Python")
Setup.cs starts this file the same way, and the package no longer carries dcme-install.ps1 (it stays in _source).

What it does:
   1  checks the build tools: the compiler (Clang from llvm-mingw) with the recorded build plans, Python, the Vulkan
      headers and SDL2 come with the mod, nothing to install; without the compiler in the package (or with
      -Toolchain msvc) - Visual Studio Build Tools and CMake through winget
   2  recognises the disc image: the sha256 of its executable against mod\\versions\\<id>\\version.txt; an unknown
      image is refused before anything is built
   3  the psxrecomp framework at the pinned commit: downloads the archive or unpacks the one the user downloaded
      (-RecompZip)
   4  applies the mod patch and puts our runtime files in
   5  SDL2 from the mod package (downloads it when the package has none)
   6  the game project
   7  SLUS_009.22 from THE USER'S disc image
   8  builds the recompiler and recompiles the game
   9  PGXP and the mod hooks in the generated code
  10  the rooms from the disc image into the build tree
  11  the game data (settings, tuning, game.toml, licences, collision, rooms) into dcme_data.pak, inside the program
  12  builds DinoCrisisME.exe
  13  lays out the game folder
  14  the desktop shortcut

No file of the game comes with the mod: the executable and the room geometry come from the image the user provided.
"""
import fnmatch
import glob
import hashlib
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import traceback
import zipfile
import zlib

# The recompiler framework. The commit is pinned: the mod patch is made against exactly it and does not apply to
# another. A ZIP of the commit rather than a clone: no Git for the user, and the archive holds the same files as a clone
# (line endings as in the repository - the patch is made against that state).
PSXRECOMP_COMMIT = 'f69b2dde99813a54cdc9287dd83f7c7fd035b32d'
PSXRECOMP_DIR = 'psxrecomp-' + PSXRECOMP_COMMIT                     # the folder inside the archive
PSXRECOMP_URL = 'https://github.com/mstan/psxrecomp/archive/%s.zip' % PSXRECOMP_COMMIT

SDL2_VERSION = '2.30.3'
SDL2_URL = ('https://github.com/libsdl-org/SDL/releases/download/release-%s/SDL2-devel-%s-VC.zip'
            % (SDL2_VERSION, SDL2_VERSION))

TOTAL_STEPS = 14

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MOD_ROOT = os.path.dirname(SCRIPT_DIR)                               # the package root
MOD_DIR = os.path.join(MOD_ROOT, 'mod')

# CRC32 of the redump .sbi files of the five LibCrypt discs (as k_subq_known in cdrom.c)
SUBQ_CRC = {'SLES_022.07': 'C9C7AB72', 'SLES_022.08': 'AAC2D94D', 'SLES_022.09': 'C3F2CA64',
            'SLES_022.10': '2BEBA179', 'SLES_022.11': '7BE522A7'}


class O:
    """The command line (parse_args)."""
    disc_path = ''
    install_path = ''
    recomp_zip = ''              # the framework archive the user downloaded; empty - download it
    skip_prereq = False          # do not install missing tools through winget
    keep_build_tree = False      # keep the sources and object files after the build
    no_shortcut = False          # leave the desktop alone
    remove_tools = False         # uninstall the build tools this run installed
    lang = 'en'                  # the language of the log: English by default
    dry_run = False              # checks only, change nothing
    toolchain = 'auto'           # auto: Clang from the package when it is there, else MSVC


class S:
    """What the run has learned so far."""
    ru = False
    step_no = 0
    last_tool_exit = 0           # the exit code of the last run_tool
    last_install_reboot = False
    installed_by_us = []         # tools this run installed: only these may be removed (-RemoveTools)
    version = None               # the game version of the disc image (find_game_version)
    color = False


class InstallError(Exception):
    pass


class UsageError(Exception):
    pass


def L(en, ru):
    """The message in the language of the log; each line lives next to its translation."""
    return ru if S.ru else en


# ─────────────────────────────── the log ───────────────────────────────────────
_PREFIX = {'step': '== ', 'ok': '   [ok] ', 'warn': '   [!]  ', 'err': '   [x]  ', 'info': '   '}
_COLOR = {'step': '96', 'ok': '92', 'warn': '93', 'err': '91', 'info': '37'}


def _setup_output():
    """UTF-8 out, a line at a time: the setup window reads the pipe as UTF-8 and shows the steps as they come."""
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
        except (AttributeError, ValueError):
            pass
    # colours only in a console (Write-Host colours of dcme-install.ps1); the pipe of the window gets plain lines
    try:
        if sys.stdout.isatty():
            import ctypes
            k32 = ctypes.WinDLL('kernel32')
            k32.GetStdHandle.restype = ctypes.c_void_p
            h = k32.GetStdHandle(-11)
            mode = ctypes.c_ulong()
            if k32.GetConsoleMode(ctypes.c_void_p(h), ctypes.byref(mode)):
                S.color = bool(mode.value & 4) or bool(k32.SetConsoleMode(ctypes.c_void_p(h), mode.value | 4))
    except Exception:
        S.color = False


def write_log(message, level='info'):
    line = _PREFIX[level] + message
    if S.color:
        line = '\x1b[%sm%s\x1b[0m' % (_COLOR[level], line)
    print(line, flush=True)


def start_step(title):
    S.step_no += 1
    write_log('%s %d %s %d: %s' % (L('Step', 'Шаг'), S.step_no, L('of', 'из'), TOTAL_STEPS, title), 'step')


def tool_state(key, state):
    """10.10.2026: the state of each tool for the setup window (Setup.cs: the icon and caption in the list under the
    Install button; the window does not show the "[tool] ..." line itself): ok - already here, bundled - from the mod
    package, missing, installing, installed, reboot - Windows asks for a restart, failed."""
    print('[tool] %s %s' % (key, state), flush=True)


def n0(n):
    """{0:N0} of PowerShell: digit groups (a no-break space in Russian, as Windows writes them)."""
    s = '{:,}'.format(int(n))
    return s.replace(',', '\u00a0') if S.ru else s


def round_half_up(x):
    return int(math.floor(x + 0.5))


# ─────────────────────────────── running tools ─────────────────────────────────
def run_tool(file, args=(), workdir=None, quiet=False, allow_failure=False):
    """Runs a program, writes its output to the log and fails on a non-zero exit code (the exit code of the last run is
    in S.last_tool_exit). stderr is merged into stdout; the lines go to the log as they come."""
    args = [str(a) for a in args]
    write_log('> %s %s' % (file, ' '.join(args)))
    try:
        p = subprocess.Popen([file] + args, cwd=workdir, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
    except OSError as e:
        # 21.10.2026: Smart App Control (WinError 4551) does not let programs built on this PC start
        if getattr(e, 'winerror', None) == 4551:
            write_app_control_help('err')
        why = e.strerror or str(e)
        raise InstallError(L('"%s" did not start: %s' % (file, why), '«%s» не запустился: %s' % (file, why)))
    out = []
    import io
    for line in io.TextIOWrapper(p.stdout, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        out.append(line)
        if not quiet and line.strip():
            write_log('  ' + line)
    code = p.wait()
    if code >= 0x80000000:                                      # Windows exit codes are 32-bit signed, as in PowerShell
        code -= 0x100000000
    S.last_tool_exit = code
    if code != 0 and not allow_failure:
        if quiet:
            for line in out:
                if line.strip():
                    write_log('  ' + line)
        # 21.10.2026 (a player's friend: "python.exe exited with code 1", and the window did not say why - it shows only
        # the steps): the last lines of the output go to the window as well, cut short (whole in install-log.txt)
        for line in [x.strip() for x in out if x.strip()][-8:]:
            if len(line) > 240:
                line = line[:237] + '...'
            write_log(line, 'err')
        if any(re.search('WinError 4551|APP CONTROL:', x, re.I) for x in out):
            write_app_control_help('err')
        raise InstallError(L('"%s" exited with code %d' % (file, code), '«%s» завершился с кодом %d' % (file, code)))


def _which_all(name):
    """Every program of that name in PATH, in the order Windows looks (Get-Command -All of PowerShell; the current
    folder is not searched)."""
    exts = [e for e in os.environ.get('PATHEXT', '.COM;.EXE;.BAT;.CMD').split(';') if e]
    for d in os.environ.get('PATH', '').split(';'):
        d = d.strip().strip('"')
        if not d:
            continue
        for e in exts:
            p = os.path.join(d, name + e)
            if os.path.isfile(p):
                yield p


def which(name):
    return next(_which_all(name), None)


def test_python_exe(exe, pre=()):
    """Starts the candidate and returns the path of the real python.exe (sys.executable) when it is Python 3.8+, else
    None. The Microsoft Store stub (WindowsApps\\python.exe without an installed Python) prints "Python was not
    found..." and exits with 9009 - it is dropped."""
    try:
        r = subprocess.run([exe] + list(pre) + ['-c', "import sys;print('%d.%d'%sys.version_info[:2]);print(sys.executable)"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=120)
    except (OSError, ValueError, subprocess.SubprocessError):
        return None
    lines = r.stdout.decode('utf-8', 'replace').splitlines()
    if r.returncode != 0 or len(lines) < 2:
        return None
    m = re.match(r'^3\.(\d+)$', lines[0])
    if not m or int(m.group(1)) < 8:
        return None
    real = lines[1].strip()
    return real if real and os.path.exists(real) else None


def find_bundled_python():
    """10.10.2026: the Python of the mod package (tools\\make_portable_python.py of the package): setup needs only the
    standard library, the player installs no Python."""
    p = os.path.join(MOD_DIR, 'tools', 'python', 'python.exe')
    return test_python_exe(p) if os.path.exists(p) else None


def find_bundled_vulkan():
    """10.10.2026: the Vulkan headers and the import library of the loader from the mod package
    (tools\\make_vulkan_bundle.py of the package): the picture goes out through DirectX 11, but the renderer is Vulkan,
    and the build needs only these, not the whole Vulkan SDK; vulkan-1.dll comes with the graphics driver."""
    d = os.path.join(MOD_DIR, 'vulkan')
    if (os.path.exists(os.path.join(d, 'Include', 'vulkan', 'vulkan.h'))
            and os.path.exists(os.path.join(d, 'Lib', 'vulkan-1.lib'))):
        return d
    return None


def find_bundled_llvm():
    """10.10.2026: the C/C++ compiler of the mod package - llvm-mingw (Clang + LLD + the MinGW-w64 headers and
    libraries, all free to pass on; tools\\make_llvm_bundle.py of the package) and two recorded build plans
    (mod\\build\\recompiler.plan.json, game.plan.json; rebuild.ps1, step 8c records them). mod\\tools\\run_plan.py builds
    by them - the player needs neither CMake nor Ninja. The llvm folder or None."""
    ok = (os.path.exists(os.path.join(MOD_DIR, 'tools', 'llvm', 'bin', 'x86_64-w64-mingw32-clang.exe'))
          and os.path.exists(os.path.join(MOD_DIR, 'tools', 'run_plan.py'))
          and os.path.exists(os.path.join(MOD_DIR, 'build', 'recompiler.plan.json'))
          and os.path.exists(os.path.join(MOD_DIR, 'build', 'game.plan.json')))
    return os.path.join(MOD_DIR, 'tools', 'llvm') if ok else None


def _avail_phys_mb():
    import ctypes

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                    ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                    ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                    ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                    ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    if not ctypes.WinDLL('kernel32').GlobalMemoryStatusEx(ctypes.byref(m)):
        raise OSError('GlobalMemoryStatusEx')
    return round(m.ullAvailPhys / 1048576)


def get_build_jobs(per_job_mb, big_mb):
    """21.10.2026 (a player's friend: setup stood at "Recompiling the game to C", "python locks up the PC"): compilers at
    a time - no more than the processors and no more than the free memory holds: per_job_mb for each, big_mb for the
    biggest file, a gigabyte for the system. A trial install (32 at a time): all the compilers together peaked at 1.7
    GB, the biggest one (the generated code) at 1 GB. Free memory unknown - no more than 4."""
    cpu = max(1, os.cpu_count() or 1)
    try:
        free_mb = _avail_phys_mb()
        jobs = int(max(1, min(cpu, math.floor((free_mb - 1024 - big_mb) / per_job_mb))))
        write_log('%d ' % jobs + L('compilers at a time', 'компиляторов сразу') + ' (%d MB ' % free_mb
                  + L('free', 'свободно') + ', %d ' % cpu + L('processors', 'процессоров') + ')')
    except Exception:
        jobs = min(cpu, 4)
    return jobs


def invoke_plan(tools, plan, src_root, per_job_mb, big_mb):
    """The build by a recorded plan (Clang from the package): run_plan.py fills in the paths and runs the commands,
    the compilations in parallel."""
    jobs = get_build_jobs(per_job_mb, big_mb)
    run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'run_plan.py'), os.path.join(MOD_DIR, 'build', plan),
                               'SRC=' + src_root, 'LLVM=' + tools['llvm'], 'VULKAN=' + tools['vulkan'], 'JOBS=%d' % jobs])


def find_python():
    """A real Python 3: every python/python3 in PATH (the Microsoft Store stub often comes first), the usual install
    folders (winget installs there too), then py -3."""
    cands = []
    for n in ('python', 'python3'):
        for p in _which_all(n):
            if p.casefold() not in [c.casefold() for c in cands]:
                cands.append(p)
    local = os.environ.get('LOCALAPPDATA')
    for root in (os.path.join(local, 'Programs', 'Python') if local else '', os.environ.get('ProgramFiles', ''),
                 os.environ.get('ProgramFiles(x86)', '')):
        if not root or not os.path.isdir(root):
            continue
        try:
            names = [d for d in os.listdir(root) if fnmatch.fnmatch(d.lower(), 'python3*')
                     and os.path.isdir(os.path.join(root, d))]
        except OSError:
            continue
        for d in sorted(names, key=str.casefold, reverse=True):
            p = os.path.join(root, d, 'python.exe')
            if os.path.exists(p) and p.casefold() not in [c.casefold() for c in cands]:
                cands.append(p)
    for c in cands:
        r = test_python_exe(c)
        if r:
            return r
    py = which('py')
    if py:
        r = test_python_exe(py, ['-3'])
        if r:
            return r
    return None


# ─────────────────────────── step 1: the tools ─────────────────────────────────
def find_msvc_cmake():
    """The CMake of Visual Studio: it is there even when no separate one was installed."""
    tail = '\\commonextensions\\microsoft\\cmake\\cmake\\bin\\cmake.exe'
    for pf in (os.environ.get('ProgramFiles', ''), os.environ.get('ProgramFiles(x86)', '')):
        base = os.path.join(pf, 'Microsoft Visual Studio') if pf else ''
        if not base or not os.path.isdir(base):
            continue
        # the usual place first (<version>\<edition>\Common7\IDE\...), then the whole folder as dcme-install.ps1 does
        for hit in sorted(glob.glob(os.path.join(glob.escape(base), '*', '*', 'Common7', 'IDE', 'CommonExtensions',
                                                 'Microsoft', 'CMake', 'CMake', 'bin', 'cmake.exe'))):
            return hit
        for root, dirs, files in os.walk(base):
            dirs.sort()
            for f in files:
                p = os.path.join(root, f)
                if f.lower() == 'cmake.exe' and p.lower().endswith(tail):
                    return p
    return None


def find_vulkan_sdk():
    v = os.environ.get('VULKAN_SDK')
    if v and os.path.exists(v):
        return v
    root = 'C:\\VulkanSDK'
    if os.path.exists(root):
        try:
            names = sorted((d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d))), key=str.casefold,
                           reverse=True)
        except OSError:
            names = []
        if names:
            return os.path.join(root, names[0])
    return None


def find_cmake():
    c = which('cmake')
    if c:
        return c
    # the Kitware MSI puts it here and does not always edit PATH
    p = os.path.join(os.environ.get('ProgramFiles', ''), 'CMake', 'bin', 'cmake.exe')
    if os.path.exists(p):
        return p
    return find_msvc_cmake()


def find_msvc():
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', ''), 'Microsoft Visual Studio', 'Installer',
                           'vswhere.exe')
    if not os.path.exists(vswhere):
        return None
    try:
        r = subprocess.run([vswhere, '-latest', '-products', '*', '-requires',
                            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=300)
    except (OSError, subprocess.SubprocessError):
        return None
    for line in r.stdout.decode('utf-8', 'replace').splitlines():
        if line.strip():
            return line.strip()
    return None


def install_via_winget(id_, human, extra=(), note=''):
    winget = which('winget')
    if not winget:
        write_log(L('%s is missing and winget is unavailable - install it manually.' % human,
                    '%s не найден, и winget недоступен - поставь вручную.' % human), 'err')
        return False
    # 09.10.2026: the line shows in the setup window (level step), not only in the log - otherwise the window stood still
    # on "Missing: ..." for a long time and people thought setup had closed
    write_log(L('Installing %s via winget (may take a few minutes)...' % human,
                'Ставлю %s через winget (может занять несколько минут)...' % human) + note, 'step')
    run_tool(winget, ['install', '--id', id_, '-e', '--accept-package-agreements', '--accept-source-agreements',
                      '--disable-interactivity'] + list(extra), allow_failure=True)
    # 10.10.2026: the program is in, but Windows asks for a restart - codes 3010/1641 of the installers and the winget
    # codes 0x8A150109 (restart to finish), 0x8A15010A (restart required), 0x8A15010B (restart started)
    code = S.last_tool_exit
    S.last_install_reboot = code in (3010, 1641, -1978335991, -1978335990, -1978335989)
    ok = code == 0 or S.last_install_reboot
    if ok:
        S.installed_by_us.append({'id': id_, 'human': human})
    return ok


def remove_installed_tools():
    """Uninstalls the build tools setup installed itself."""
    start_step(L('Removing the build tools', 'Удаление программ сборки'))
    if not S.installed_by_us:
        write_log(L('nothing to remove: every tool was already on this machine',
                    'удалять нечего: все программы уже стояли на этой машине'), 'ok')
        return
    winget = which('winget')
    if not winget:
        write_log(L('winget is unavailable - remove them by hand', 'winget недоступен - удали их вручную'), 'warn')
        return
    for t in S.installed_by_us:
        write_log(L('removing', 'удаляю') + ' %s...' % t['human'])
        run_tool(winget, ['uninstall', '--id', t['id'], '-e', '--accept-source-agreements', '--disable-interactivity'],
                 allow_failure=True, quiet=True)
        if S.last_tool_exit == 0:
            write_log('%s ' % t['human'] + L('removed', 'удалён'), 'ok')
        else:
            write_log('%s ' % t['human'] + L('could not be removed - remove it by hand', 'удалить не удалось - убери вручную'),
                      'warn')


def _reg_env(name, machine):
    """An environment variable as Windows keeps it for new processes (Environment.GetEnvironmentVariable(name,
    Machine/User)): %...% expanded."""
    import winreg
    try:
        if machine:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment')
        else:
            k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, 'Environment')
        with k:
            v, t = winreg.QueryValueEx(k, name)
    except OSError:
        return None
    if t == winreg.REG_EXPAND_SZ:
        v = winreg.ExpandEnvironmentStrings(v)
    return v if isinstance(v, str) else None


def update_tool_env(llvm=None):
    """winget edits PATH and VULKAN_SDK only for new processes - take them from the registry. The Clang folder of the
    package goes in front again (windres finds clang for its preprocessor next to it)."""
    os.environ['PATH'] = (_reg_env('Path', True) or '') + ';' + (_reg_env('Path', False) or '')
    if llvm:
        os.environ['PATH'] = os.path.join(llvm, 'bin') + ';' + os.environ['PATH']
    vk = _reg_env('VULKAN_SDK', True) or _reg_env('VULKAN_SDK', False)
    if vk:
        os.environ['VULKAN_SDK'] = vk


def get_smart_app_control():
    """21.10.2026 (the log of a player's friend: "OSError: [WinError 4551] An Application Control policy has blocked this
    file" while building the recompiler): Windows 11 Smart App Control lets no unsigned program start, and the mod is
    built on the player's PC - neither the build nor the game itself can be signed. VerifiedAndReputablePolicyState:
    0 - off, 1 - on, 2 - evaluation; -1 - unknown (Windows 10, no key)."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\CI\Policy') as k:
            return int(winreg.QueryValueEx(k, 'VerifiedAndReputablePolicyState')[0])
    except Exception:
        return -1


def write_app_control_help(level):
    """21.10.2026 (the player: "it sounds scary for those who have it on"): calmly - why Windows does not know the game
    and what to do. Error 4551 with Smart App Control off is another policy of the PC (work, school), setup cannot get
    around it."""
    if get_smart_app_control() != 1:
        write_log(L('Windows did not let programs built on this PC run. Smart App Control is off here, so another app '
                    'control policy of this PC stops them (often on work or school PCs).',
                    'Windows не дала запустить программы, собранные на этом ПК. Интеллектуальное управление приложениями '
                    'здесь выключено, значит, их не пускает другая политика этого ПК (так бывает на рабочих и учебных '
                    'ПК).'), level)
        return
    write_log(L('Windows 11 Smart App Control is on. It only lets through apps it already knows, and the game is built '
                'from your disc right on this PC, so to Windows it is a brand-new app.',
                'Включено интеллектуальное управление приложениями Windows 11. Оно пропускает только знакомые ему '
                'программы, а игра собирается из твоего диска прямо на этом ПК, и для Windows это новая программа.'),
              level)
    write_log(L('To install and play, turn it off: Windows Security > App & browser control > Smart App Control settings '
                '> Off, then run setup again.',
                'Чтобы установить и играть, выключи его: Безопасность Windows > Управление приложениями и браузером > '
                'Интеллектуальное управление приложениями > Выключить, и запусти установку снова.'), level)


def _under(path, folder):
    """Is path inside folder (any case, long or short names)."""
    def norm(p):
        try:
            p = os.path.realpath(p)
        except (OSError, ValueError):
            p = os.path.abspath(p)
        return os.path.normcase(p).rstrip('\\') + '\\'
    return norm(path).startswith(norm(folder))


def test_prerequisites():
    start_step(L('Checking build tools', 'Проверка инструментов сборки'))
    if get_smart_app_control() == 1:
        write_app_control_help('warn')            # 21.10.2026: up front, before minutes of building

    # No Git: the framework comes as a ZIP of the pinned commit. 09.10.2026, the first run on a clean PC: the check took
    # the Microsoft Store stub for Python and stopped setup - every tool is looked for for real, and looked for again
    # right after it is installed. The compiler before CMake: Build Tools with the recommended parts bring CMake.
    # winget installs Build Tools WITHOUT the C++ compiler (the manifest has only --quiet --wait), so the C++ workload
    # is named; version 2026 is the same Visual Studio 18 the mod is built and checked with.
    # 10.10.2026: Python, the Vulkan headers and the compiler (llvm-mingw with the recorded build plans) come with the
    # mod; then neither the Microsoft compiler nor CMake is needed. -Toolchain msvc - the old way through Visual Studio
    # Build Tools and CMake (winget installs them). The window gets "[tool] <key> <state>" lines.
    llvm = None
    if O.toolchain != 'msvc':
        llvm = find_bundled_llvm()
    if O.toolchain == 'llvm' and not llvm:
        raise InstallError(L('-Toolchain llvm: the package carries no compiler or build plans (mod\\tools\\llvm, mod\\build).',
                             '-Toolchain llvm: в пакете нет компилятора или планов сборки (mod\\tools\\llvm, mod\\build).'))
    if llvm:
        os.environ['PATH'] = os.path.join(llvm, 'bin') + ';' + os.environ.get('PATH', '')   # windres finds clang next to it
        tool_state('msvc', 'bundled')
        write_log(L('C/C++ compiler: Clang (llvm-mingw) included with the mod', 'компилятор C/C++: Clang (llvm-mingw) из пакета мода'))

    def find_tool_python():
        # the Python of the package; then the one running setup; then any Python 3 of this PC
        return find_bundled_python() or test_python_exe(sys.executable) or find_python()

    tools = [
        {'key': 'msvc', 'label': 'MSVC', 'human': 'Visual Studio Build Tools (C++)', 'id': 'Microsoft.VisualStudio.BuildTools',
         'find': find_msvc,
         'extra': ['--override', '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'],
         'note': L(' Several GB, 10-30 minutes; the progress is in the Visual Studio Installer window.',
                   ' Это несколько ГБ, 10-30 минут; ход установки - в окне Visual Studio Installer.'),
         'hint': L('Open Visual Studio Installer, press Modify and add the C++ build tools (MSVC), then run setup again.',
                   'Открой Visual Studio Installer, нажми «Изменить» и добавь средства сборки C++ (MSVC), затем запусти '
                   'установку заново.')},
        {'key': 'cmake', 'label': 'cmake', 'human': 'CMake', 'id': 'Kitware.CMake', 'find': find_cmake},
        {'key': 'python', 'label': 'python', 'human': 'Python 3', 'id': 'Python.Python.3.12', 'find': find_tool_python},
        {'key': 'vulkan', 'label': 'Vulkan', 'human': 'Vulkan SDK', 'id': 'KhronosGroup.VulkanSDK',
         'find': lambda: find_bundled_vulkan() or find_vulkan_sdk()},
    ]
    if llvm:
        tools = [t for t in tools if t['key'] not in ('msvc', 'cmake')]   # the plans need no CMake
    found = {}
    missing = []
    for t in tools:
        p = t['find']()
        if p:
            found[t['key']] = p
            if _under(p, MOD_DIR):
                tool_state(t['key'], 'bundled')
                write_log('%s: ' % t['label'] + L('included with the mod', 'из пакета мода'))   # the log only
            else:
                tool_state(t['key'], 'ok')
                write_log('%s: %s' % (t['label'], p), 'ok')
        else:
            missing.append(t)
            tool_state(t['key'], 'missing')

    if missing:
        write_log(L('Missing: ', 'Не хватает: ') + ', '.join(m['human'] for m in missing), 'warn')
        if O.skip_prereq:
            raise InstallError(L('Required tools are missing and -SkipPrereq disabled installing them.',
                                 'Нет обязательных инструментов, а доустановка отключена ключом -SkipPrereq.'))
        write_log(L('Setup installs them now and then continues by itself - keep this window open.',
                    'Установщик сейчас поставит их и сам продолжит - не закрывай это окно.'), 'warn')
        for m in missing:
            update_tool_env(llvm)
            p = m['find']()                                   # CMake may have come with Build Tools
            if not p:
                tool_state(m['key'], 'installing')
                ok = install_via_winget(m['id'], m['human'], m.get('extra', []), m.get('note', ''))
                update_tool_env(llvm)
                p = m['find']()
                if not p:
                    tool_state(m['key'], 'failed')
                    if 'hint' in m:
                        msg = L('%s: the C++ compiler is still not found.' % m['human'],
                                '%s: компилятор C++ так и не найден.' % m['human']) + ' ' + m['hint']
                    elif ok:
                        msg = L('%s is still not found after installing. Reboot and run setup again.' % m['human'],
                                '%s не найден и после установки. Перезагрузи компьютер и запусти установку заново.' % m['human'])
                    else:
                        msg = L('Could not install %s. Install it manually and run setup again.' % m['human'],
                                'Не удалось поставить %s. Поставь вручную и запусти установку заново.' % m['human'])
                    raise InstallError(msg)
                tool_state(m['key'], 'installed')
                if S.last_install_reboot:
                    tool_state(m['key'], 'reboot')
                    write_log(L('Windows asks to restart the PC to finish installing %s. Setup goes on - the game will be '
                                'ready; restart the PC when it is done.' % m['human'],
                                'Windows просит перезагрузить компьютер, чтобы закончить установку %s. Установка мода '
                                'продолжается - игра будет готова; перезагрузи компьютер после неё.' % m['human']), 'warn')
                # the windows of the tool installers cover ours: ask the setup window back on top (Setup.cs; the window
                # does not show the "[front]" line itself)
                print('[front]', flush=True)
            else:
                tool_state(m['key'], 'ok')                    # came with another program
            found[m['key']] = p
            write_log('%s: %s' % (m['label'], p), 'ok')

    os.environ['VULKAN_SDK'] = found['vulkan']               # 10.10: the headers and vulkan-1.lib, usually from the package
    return {'python': found['python'], 'cmake': found.get('cmake'), 'vulkan': found['vulkan'], 'llvm': llvm}


# ─────────────────────────── files ─────────────────────────────────────────────
def _long(path):
    """The \\\\?\\ form of a path: Windows then takes paths of 260 characters and more (the object files of the
    recompiler build under a long install path)."""
    full = os.path.abspath(path)
    if full.startswith('\\\\?\\'):
        return full
    if full.startswith('\\\\'):
        return '\\\\?\\UNC\\' + full[2:]
    return '\\\\?\\' + full


def remove_file(path):
    """Remove-Item -Force: a read-only file too."""
    try:
        os.remove(path)
    except PermissionError:
        os.chmod(path, stat.S_IWRITE)
        os.remove(path)


def _rmtree(path, strict):
    """shutil.rmtree that also removes read-only files (Remove-Item -Recurse -Force); strict - a file that still will
    not go stops setup, otherwise it is left."""
    def again(func, p, _exc=None):
        try:
            os.chmod(p, stat.S_IWRITE)
            func(p)
        except OSError:
            if strict:
                raise
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=again)
    else:
        shutil.rmtree(path, onerror=again)


def remove_tree(path):
    """23.10.2026: Remove-Item of Windows PowerShell did not take paths of 260 characters and more, so .build-src of an
    earlier install stayed behind and the next install into the same folder failed at the framework step. Never fails:
    what will not go is left to the check of the caller."""
    if not os.path.lexists(path):
        return
    lp = _long(path)
    try:
        if os.path.isdir(lp) and not os.path.islink(path):
            _rmtree(lp, False)
        else:
            remove_file(lp)
    except OSError:
        pass
    if os.path.lexists(path):
        try:
            subprocess.run(['cmd', '/c', 'rd', '/s', '/q', lp], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        except OSError:
            pass


def tree_size(path):
    total = 0
    for root, dirs, files in os.walk(_long(path)):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def copy_file(src, dst):
    """Copy-Item -Force: also over a read-only file; the date of the file stays the source's (MSBuild sees dates)."""
    try:
        shutil.copy2(src, dst)
    except PermissionError:
        target = os.path.join(dst, os.path.basename(src)) if os.path.isdir(dst) else dst
        if not os.path.isfile(target):
            raise
        os.chmod(target, stat.S_IWRITE)
        shutil.copy2(src, target)


def copy_glob(src_dir, dst_dir, pattern='*'):
    """Copy-Item <src_dir>\\<pattern> <dst_dir> -Force: the matching files (PowerShell wildcards ignore case); a folder
    that matches is made empty, as Copy-Item without -Recurse does."""
    for name in sorted(os.listdir(src_dir), key=str.casefold):
        if not fnmatch.fnmatch(name.lower(), pattern.lower()):
            continue
        s = os.path.join(src_dir, name)
        if os.path.isdir(s):
            os.makedirs(os.path.join(dst_dir, name), exist_ok=True)
        else:
            copy_file(s, os.path.join(dst_dir, name))


def count_files(folder, pattern='*'):
    try:
        return sum(1 for n in os.listdir(folder)
                   if fnmatch.fnmatch(n.lower(), pattern.lower()) and os.path.isfile(os.path.join(folder, n)))
    except OSError:
        return 0


def read_text(path):
    """[IO.File]::ReadAllText: UTF-8, a byte order mark dropped, the line endings as they are."""
    with open(path, 'r', encoding='utf-8-sig', errors='replace', newline='') as f:
        return f.read()


def write_text(path, text):
    """UTF-8 without a byte order mark, the line endings as they are."""
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def temp_root():
    """26.10.2026 (YouTube: "An object at the specified path c:\\users\\saeed~1 does not exist"): the temp folder under
    its long name - TEMP may hold the short name of the profile folder."""
    t = tempfile.gettempdir()
    try:
        return os.path.realpath(t)
    except (OSError, ValueError):
        return t


def get_file(url, dest, expected_size=0):
    write_log(L('downloading', 'скачиваю') + ' ' + url)
    import ssl
    import urllib.request
    part = dest + '.part'
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'DinoCrisisME-setup'})
        with urllib.request.urlopen(req, timeout=120, context=ssl.create_default_context()) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(part, dest)
    except Exception as e:
        try:
            os.remove(part)
        except OSError:
            pass
        raise InstallError(L('Could not download %s: %s' % (url, e), 'Не удалось скачать %s: %s' % (url, e)))
    size = os.path.getsize(dest)
    if expected_size > 0 and size != expected_size:
        raise InstallError(L('Size of %s (%d) does not match the expected %d - the file on the server changed.' % (dest, size, expected_size),
                             'Размер %s (%d) не совпал с ожидаемым (%d) - файл на сервере изменился.' % (dest, size, expected_size)))
    write_log(L('received', 'получено') + ' ' + n0(size) + ' ' + L('bytes', 'байт'), 'ok')


def extract_zip(zip_path, dest):
    """Expand-Archive: every file of the archive under dest, the folders it lists, the date of each file from the
    archive (read as local time, as .NET does). Through \\\\?\\ - no 260 character limit; a name that would leave dest is
    refused."""
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            parts = [p for p in info.filename.replace('\\', '/').split('/') if p not in ('', '.')]
            if not parts:
                continue
            if any(p == '..' or ':' in p for p in parts):
                raise zipfile.BadZipFile('a name outside the folder: ' + info.filename)
            target = _long(os.path.join(dest, *parts))
            if info.is_dir():
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if os.path.isfile(target):
                os.chmod(target, stat.S_IWRITE)
            with z.open(info) as src, open(target, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            try:
                t = time.mktime(info.date_time + (0, 0, -1))
                os.utime(target, (t, t))
            except (OverflowError, ValueError, OSError):
                pass


def count_patch_files(patch):
    """The 'F *' lines of a positional patch - one for each file it changes (PowerShell -like ignores case)."""
    with open(patch, 'rb') as f:
        text = f.read().decode('utf-8-sig', 'replace')
    return sum(1 for line in re.split(r'\r\n|\r|\n', text) if line[:2] in ('F ', 'f '))


# ─────────────────────────── game versions ─────────────────────────────────────
# 08.10.2026: mod\versions\<id>\version.txt - "key = value" lines (tools\make_version.py of the package): id, order, name,
# name_ru, exe, exe_sha256, exe_size; a version other than the base one also has runtime_patch, hooks_patch, faces (paths
# from its folder); subchannel = sbi - the disc needs the subchannel file next to the image (LibCrypt, Europe).
def get_versions():
    vroot = os.path.join(MOD_DIR, 'versions')
    try:
        names = sorted(os.listdir(vroot), key=str.casefold)
    except OSError:
        names = []
    found = []
    for n in names:
        d = os.path.join(vroot, n)
        f = os.path.join(d, 'version.txt')
        if not os.path.isdir(d) or not os.path.isfile(f):
            continue
        v = {'dir': d, 'order': '99'}
        for line in re.split(r'\r\n|\r|\n', read_text(f)):
            m = re.match(r'^\s*([a-z_0-9]+)\s*=\s*(.*?)\s*$', line, re.I)
            if m:
                v[m.group(1).lower()] = m.group(2)
        if v.get('id') and v.get('exe_sha256'):
            found.append(v)

    def order(v):
        try:
            return int(v['order'])
        except ValueError:
            return 99
    return sorted(found, key=order)


def get_version_name(v):
    return v['name_ru'] if S.ru and v.get('name_ru') else v.get('name', '')


def set_version_entry(text):
    """08.10.2026: the entry point of the exe of this version in the text of game.toml (Europe - 0x8001A1E8, USA v1.1 -
    0x8001A1C8). The recompiler starts from it, and the game learns from it that the game code has started (4:3 BIOS ->
    16:9 world)."""
    v = S.version
    if not v:
        return text
    e = v.get('entry_pc')
    if e:
        if not re.match(r'^0x[0-9A-Fa-f]{8}$', e, re.I):
            raise InstallError('entry_pc: ' + e)
        text = re.sub(r'^(entry_pc\s*=\s*)"0x[0-9A-Fa-f]+"', lambda m: m.group(1) + '"' + e + '"', text, flags=re.M | re.I)
    # 08.10.2026: where the code of the exe of the version lies and how big it is - the Italian and Spanish ones load at
    # 0x80010000, the Spanish code is 0x9A000 (USA v1.1 and English Europe - 0x80018000 and 0x99800)
    for key in ('load_address', 'text_size'):
        kv = v.get(key)
        if not kv:
            continue
        if not re.match(r'^0x[0-9A-Fa-f]{8}$', kv, re.I):
            raise InstallError('%s: %s' % (key, kv))
        text = re.sub(r'^(' + key + r'\s*=\s*)"0x[0-9A-Fa-f]+"', lambda m: m.group(1) + '"' + kv + '"', text,
                      flags=re.M | re.I)
    # the disc ID: the game checks the image by it at start (otherwise "the disc has no expected game ID"); for a version
    # it is the name of its exe on the disc (Europe - SLES_022.07). The exe file in the project and in data\ stays
    # SLUS_009.22.
    i = v.get('exe')
    if i and re.match(r'^[A-Za-z0-9_.\-]+$', i):
        text = re.sub(r'^(id\s*=\s*)"[^"\r\n]*"', lambda m: m.group(1) + '"' + i + '"', text, flags=re.M | re.I)
    return text


def get_subchannel_bases(disc):
    stem = os.path.splitext(os.path.basename(disc))[0]
    bases = []
    m = re.match(r'^(.*\S) \(Track [^)]+\)$', stem, re.I)       # redump: "<game> (Track 1).bin" and "<game>.sbi"
    if m:
        bases.append(m.group(1))
    bases.append(stem)
    return bases


def find_subchannel_file(disc, exe=''):
    """08.10.2026: the subchannel file of the disc (.sbi / .lsd) next to the image - where and how the game looks for it
    (cdrom.c, subq_load). 24.10.2026 (the player: a German disc with "SLES_022.09.sbi" next to it was not found; "by the
    name and by the contents"): first an .sbi of this folder with the CRC32 of the redump file of this disc (any name),
    then by the name of the image, then the only .sbi/.lsd of the folder (the game checks it and says so if it belongs to
    another disc). The path or None."""
    d = os.path.dirname(disc)
    try:
        allf = [os.path.join(d, n) for n in sorted(os.listdir(d), key=str.casefold)
                if os.path.splitext(n)[1].lower() in ('.sbi', '.lsd') and os.path.isfile(os.path.join(d, n))]
    except OSError:
        allf = []
    crc = {k.upper(): c for k, c in SUBQ_CRC.items()}.get(exe.upper()) if exe else None
    if crc:
        for f in allf:
            if os.path.splitext(f)[1].lower() == '.sbi' and os.path.getsize(f) <= 65536:
                with open(f, 'rb') as fh:
                    if '%08X' % (zlib.crc32(fh.read()) & 0xFFFFFFFF) == crc:
                        return f
    for b in get_subchannel_bases(disc):
        for e in ('.sbi', '.lsd'):
            p = os.path.join(d, b + e)
            if os.path.isfile(p):
                return p
    if len(allf) == 1:
        return allf[0]
    return None


def disc_voice_print(disc, name):
    """28.10.2026: the print of the speech file PSX/DATA_XA/<name> of the image as the game's faces check it
    (dc_faces2.inc): CRC32 of the first 8 sectors of channel 1 - sector lba + 1 + 32*k, the 2048 bytes after the 24-byte
    header. Raw images only (a 2048-byte image holds no speech); None - no such file."""
    try:
        with open(disc, 'rb') as f:
            if f.read(12) != b'\x00' + b'\xff' * 10 + b'\x00':
                return None

            def sec(lba):
                f.seek(lba * 2352 + 24)
                b = f.read(2048)
                if len(b) != 2048:
                    raise EOFError
                return b
            pvd = sec(16)
            if pvd[1:6] != b'CD001':
                return None
            lba, size = int.from_bytes(pvd[158:162], 'little'), int.from_bytes(pvd[166:170], 'little')
            for part in ('PSX', 'DATA_XA', name):
                if size <= 0 or size > (1 << 20):
                    return None
                d = b''.join(sec(lba + i) for i in range((size + 2047) // 2048))
                o, hit = 0, None
                while o + 33 < len(d):
                    if d[o] == 0:
                        o = (o // 2048 + 1) * 2048
                        continue
                    nm = d[o + 33:o + 33 + d[o + 32]].split(b';')[0].decode('latin-1')
                    if nm.upper() == part.upper():
                        hit = (int.from_bytes(d[o + 2:o + 6], 'little'), int.from_bytes(d[o + 10:o + 14], 'little'))
                        break
                    o += d[o]
                if not hit:
                    return None
                lba, size = hit
                if part == name:
                    break
            crc = 0
            for i in range(8):
                crc = zlib.crc32(sec(lba + 1 + 32 * i), crc)
            return crc & 0xFFFFFFFF
    except (OSError, EOFError):
        return None


def pick_by_voice(hits):
    """28.10.2026 (SRU: the Japanese disc with a Russian voice-over, the same exe): of the versions with the disc's exe the
    one whose voice_print ("STAGE01.XAS A4B95C0D") is the print of that speech file on the disc, else the one without a
    print; None - none of them."""
    prints = {}
    for v in hits:
        vp = v.get('voice_print', '').split()
        if len(vp) != 2:
            continue
        if vp[0] not in prints:
            prints[vp[0]] = disc_voice_print(O.disc_path, vp[0])
            write_log('voice print %s: %s' % (vp[0], 'none' if prints[vp[0]] is None else '%08X' % prints[vp[0]]))
        if prints[vp[0]] is not None and '%08X' % prints[vp[0]] == vp[1].upper():
            return v
    for v in hits:
        if not v.get('voice_print'):
            return v
    return None


def find_game_version(tools):
    """Which image this is: the sha256 of the game executable from it against the list of versions. An unknown one is
    refused at once, before downloading and building."""
    start_step(L('Recognising the disc image', 'Распознаю образ игры'))
    allv = get_versions()
    if not allv:
        raise InstallError(L('The installer package has no list of game versions (mod\\versions).',
                             'В пакете установщика нет списка версий игры (mod\\versions).'))
    tmp = os.path.join(temp_root(), 'dcme_exe_' + os.urandom(4).hex())
    os.makedirs(tmp, exist_ok=True)
    hsh = ''
    try:
        names = []
        for v in allv:
            e = v.get('exe')
            if e and e.casefold() not in [n.casefold() for n in names]:
                names.append(e)
        for exe_name in sorted(names, key=str.casefold):
            out = os.path.join(tmp, exe_name)
            run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'dc_extract', 'extract_exe.py'), O.disc_path, out,
                                       exe_name], allow_failure=True, quiet=True)
            if not os.path.isfile(out):
                continue
            hsh = sha256(out)
            v = pick_by_voice([v for v in allv if v.get('exe', '').casefold() == exe_name.casefold() and
                               v['exe_sha256'].lower() == hsh])
            if not v:
                continue
            write_log(L('disc image:', 'образ:') + ' ' + get_version_name(v), 'ok')
            if v.get('subchannel', '').lower() == 'sbi':
                # LibCrypt: without the subchannel the game stops at the first screen (the key does not match)
                sbi = find_subchannel_file(O.disc_path, v['exe'])
                if not sbi:
                    want = os.path.join(os.path.dirname(O.disc_path), get_subchannel_bases(O.disc_path)[0] + '.sbi')
                    raise InstallError(L(
                        "This disc is protected by LibCrypt: the game checks data kept in the disc's subchannel, "
                        "and a .bin image does not hold it. Put the subchannel file of this disc next to the image: "
                        "%s (the .sbi file of the disc dump, the same one the DuckStation emulator uses)." % want,
                        'Этот диск защищён LibCrypt: игра сверяет данные служебного канала диска (subchannel), а в '
                        'образе .bin их нет. Положи файл служебного канала этого диска рядом с образом: %s (файл '
                        '.sbi от дампа диска, тот же, что берёт эмулятор DuckStation).' % want))
                write_log(L('subchannel file:', 'служебный канал:') + ' ' + sbi, 'ok')
            return v
    finally:
        remove_tree(tmp)
    if hsh:
        write_log('SLUS_009.22 sha256 ' + hsh)
    raise InstallError(L('Unknown disc image.', 'Неизвестный образ игры.'))      # 08.10.2026 (the player): no list, it is in README


# ─────────────────────────── the install steps ─────────────────────────────────
def step_framework(src_root):
    start_step(L('Recompiler framework (psxrecomp)', 'Каркас рекомпилятора psxrecomp'))
    # the archive stays next to the sources: a repeated install does not download it again, and the unpacking is done
    # from scratch anyway (below)
    zip_path = os.path.join(src_root, PSXRECOMP_DIR + '.zip')
    if O.recomp_zip:
        # the user downloaded the archive: any name will do ("psxrecomp-<commit>.zip" or "<commit>.zip")
        if not os.path.isfile(O.recomp_zip):
            raise InstallError(L('Recompiler archive not found: ' + O.recomp_zip, 'Архив рекомпилятора не найден: ' + O.recomp_zip))
        try:
            same = os.path.exists(zip_path) and os.path.samefile(O.recomp_zip, zip_path)
        except OSError:
            same = False
        if not same:
            copy_file(O.recomp_zip, zip_path)
        write_log(L('using the archive you downloaded:', 'беру архив, скачанный вручную:') + ' ' + O.recomp_zip, 'ok')
    elif os.path.exists(zip_path):
        write_log(L('archive already downloaded, skipping', 'архив уже скачан, пропускаю'), 'ok')
    else:
        get_file(PSXRECOMP_URL, zip_path)

    # Unpacking ALWAYS from scratch: the patch applies only to an untouched tree, and the archive gives that state.
    fw = os.path.join(src_root, 'psxrecomp')
    remove_tree(fw)
    if os.path.lexists(fw):
        raise InstallError(L('Could not remove the old build folder %s - delete it and run setup again.' % fw,
                             'Не удалось убрать старую папку сборки %s - удали её и запусти установку заново.' % fw))
    # 08.10.2026: unpacking into a temp folder with a short path - the longest file of the archive is 171 characters from
    # its root; in .build-src\psxrecomp the same files are 51 characters shorter
    tmp = os.path.join(temp_root(), 'dcme_fw_' + os.urandom(4).hex())
    if os.path.exists(tmp):
        _rmtree(_long(tmp), True)
    os.makedirs(tmp)
    try:
        extract_zip(zip_path, tmp)
    except Exception:
        # a broken archive does not stay in the cache: otherwise the next run would "skip the download" and hit the same
        # error
        try:
            remove_file(zip_path)
        except OSError:
            pass
        remove_tree(tmp)
        raise InstallError(L('The recompiler archive did not unpack - it is damaged or incomplete. Run setup again.',
                             'Архив рекомпилятора не распаковался - он повреждён или скачан не полностью. Запусти установку заново.'))
    inner = os.path.join(tmp, PSXRECOMP_DIR)
    if not os.path.isdir(inner):
        # GitHub puts everything into one folder named after the commit; without it the archive is not the one (for
        # example the main branch instead of the pinned commit)
        got = ', '.join(sorted((n for n in os.listdir(tmp) if os.path.isdir(os.path.join(tmp, n))), key=str.casefold))
        remove_tree(tmp)
        raise InstallError(L('Wrong archive: expected the folder %s inside, found: %s. Download the pinned commit: %s'
                             % (PSXRECOMP_DIR, got, PSXRECOMP_URL),
                             'Не тот архив: внутри ожидалась папка %s, а лежит: %s. Нужен закреплённый коммит: %s'
                             % (PSXRECOMP_DIR, got, PSXRECOMP_URL)))
    shutil.move(_long(inner), _long(fw))              # a rename on one disk, a copy across disks
    remove_tree(tmp)
    write_log(L('psxrecomp at commit', 'psxrecomp на коммите') + ' ' + PSXRECOMP_COMMIT, 'ok')
    return fw


def step_apply_mod(tools, fw):
    start_step(L('Applying the mod to the framework', 'Наложение мода на каркас'))
    # A positional patch instead of a unified diff: the edits are given by line numbers, not by pieces of someone
    # else's code, so the mod carries no line of the framework. The sha256 of each source file is checked - on another
    # commit the patch refuses honestly instead of quietly spoiling the tree.
    patch = os.path.join(MOD_DIR, 'patches', 'psxrecomp-dcme.dcpatch')
    posdiff = os.path.join(MOD_DIR, 'tools', 'posdiff.py')
    run_tool(tools['python'], [posdiff, 'apply', patch, fw])
    pf = count_patch_files(patch)
    write_log(L('framework patch applied (%d files)' % pf, 'патч правок каркаса наложен (файлов: %d)' % pf), 'ok')

    copy_glob(os.path.join(MOD_DIR, 'src', 'runtime'), os.path.join(fw, 'runtime', 'src'))
    copy_glob(os.path.join(MOD_DIR, 'src', 'include'), os.path.join(fw, 'runtime', 'include'))
    n = count_files(os.path.join(MOD_DIR, 'src', 'runtime')) + count_files(os.path.join(MOD_DIR, 'src', 'include'))
    write_log(L('mod runtime files copied', 'наши файлы рантайма скопированы') + ' (%d)' % n, 'ok')

    # stb_truetype (Sean Barrett, public domain) rasterises the font of the mod
    stb_dir = os.path.join(fw, 'runtime', 'deps', 'stb')
    os.makedirs(stb_dir, exist_ok=True)
    stb = os.path.join(stb_dir, 'stb_truetype.h')
    copy_file(os.path.join(MOD_DIR, 'deps', 'stb', 'stb_truetype.h'), stb)
    os.utime(stb, None)

    # 08.10.2026: another game version - the runtime addresses by its map and its edits (the same positional format,
    # only our bytes; the sha256 of each file is checked against the USA v1.1 tree made above)
    v = S.version
    if v and v.get('runtime_patch'):
        vp = os.path.join(v['dir'], v['runtime_patch'])
        run_tool(tools['python'], [posdiff, 'apply', vp, fw])
        vf = count_patch_files(vp)
        write_log(L('runtime adapted to', 'рантайм под') + ' ' + get_version_name(v) + ' (' + L('files', 'файлов')
                  + ': %d)' % vf, 'ok')


def step_sdl2(src_root):
    start_step('SDL2')
    # runtime.cmake looks for the pack by the mask <framework>/../sdl2-msvc/SDL2-* - the folder has to lie next to
    # psxrecomp and have exactly this name
    sdl_root = os.path.join(src_root, 'sdl2-msvc')
    target = os.path.join(sdl_root, 'SDL2-' + SDL2_VERSION)
    lib = os.path.join(target, 'lib', 'x64', 'SDL2.lib')
    if os.path.exists(lib):
        write_log(L('already unpacked, skipping', 'уже распакован, пропускаю'), 'ok')
        return target
    os.makedirs(sdl_root, exist_ok=True)
    # 10.10.2026: SDL2 (zlib licence) comes with the mod package (tools\make_sdl2_bundle.py of the package)
    bundled = os.path.join(MOD_DIR, 'sdl2', 'SDL2-' + SDL2_VERSION)
    if os.path.isfile(os.path.join(bundled, 'lib', 'x64', 'SDL2.lib')):
        if os.path.lexists(target):
            _rmtree(_long(target), True)               # left half-copied by an earlier run
        shutil.copytree(_long(bundled), _long(target), dirs_exist_ok=True)
        if not os.path.exists(lib):
            raise InstallError(L('SDL2 was not copied: no %s\\lib\\x64\\SDL2.lib' % target,
                                 'SDL2 не скопирован: нет %s\\lib\\x64\\SDL2.lib' % target))
        tool_state('sdl2', 'bundled')
        write_log('SDL2 %s ' % SDL2_VERSION + L('included with the mod', 'из пакета мода'))         # the log only
        return target
    zip_path = os.path.join(temp_root(), 'SDL2-devel-%s-VC.zip' % SDL2_VERSION)
    get_file(SDL2_URL, zip_path)
    extract_zip(zip_path, sdl_root)
    try:
        remove_file(zip_path)
    except OSError:
        pass
    if not os.path.exists(lib):
        raise InstallError(L('SDL2 unpacked to the wrong place: no %s\\lib\\x64\\SDL2.lib' % target,
                             'SDL2 распакован не туда: нет %s\\lib\\x64\\SDL2.lib' % target))
    write_log('SDL2 %s ' % SDL2_VERSION + L('ready', 'готов'), 'ok')
    return target


def step_project(src_root):
    start_step(L('Game project', 'Проект игры'))
    proj = os.path.join(src_root, 'DinoCrisisME')
    for d in (proj, os.path.join(proj, 'seeds'), os.path.join(proj, 'generated')):
        os.makedirs(d, exist_ok=True)
    pdir = os.path.join(MOD_DIR, 'project')
    copy_file(os.path.join(pdir, 'CMakeLists.txt'), os.path.join(proj, 'CMakeLists.txt'))
    write_text(os.path.join(proj, 'game.toml'), set_version_entry(read_text(os.path.join(pdir, 'game.toml'))))
    copy_file(os.path.join(pdir, 'dc_icon.ico'), os.path.join(proj, 'dc_icon.ico'))
    # 11.10.2026: the UTF-8 manifest of the program (paths with Cyrillic) - the .rc of the build takes it
    if os.path.isfile(os.path.join(pdir, 'dc_utf8.manifest')):
        copy_file(os.path.join(pdir, 'dc_utf8.manifest'), os.path.join(proj, 'dc_utf8.manifest'))
    copy_glob(os.path.join(pdir, 'seeds'), os.path.join(proj, 'seeds'))
    # 27.10.2026 (the Arabic translation): the entries of the own code of the version the recompiler does not find by
    # itself (seeds.txt of the version, tools\make_version.py of the package) - at the end of the seeds file of the
    # project, as in the package build
    v = S.version
    if v and v.get('seeds'):
        sf = os.path.join(proj, 'seeds', 'dc1_seeds.txt')
        if not os.path.isfile(sf):
            raise InstallError('no ' + sf)
        with open(os.path.join(v['dir'], v['seeds']), 'rb') as f:
            add = f.read()
        with open(sf, 'ab') as f:
            f.write(add)
    write_log(L('project created at', 'проект собран в') + ' ' + proj, 'ok')
    return proj


def step_extract_exe(tools, proj):
    start_step(L('Game executable from your disc image', 'Исполняемый файл игры из твоего образа'))
    out = os.path.join(proj, 'SLUS_009.22')
    # 08.10.2026: another game version names its executable on the disc its own way (Europe - SLES_022.07); in the
    # project it is still SLUS_009.22 - so the recompiler, game.toml and the game (data\SLUS_009.22) expect it
    v = S.version
    exe_on_disc = v['exe'] if v and v.get('exe') else 'SLUS_009.22'
    run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'dc_extract', 'extract_exe.py'), O.disc_path, out, exe_on_disc])
    if not os.path.exists(out):
        raise InstallError(L('SLUS_009.22 was not extracted.', 'SLUS_009.22 не извлёкся.'))
    if v and sha256(out) != v['exe_sha256'].lower():
        raise InstallError(L('SLUS_009.22 changed since the image was recognised - is the disc image being replaced?',
                             'SLUS_009.22 не тот, что при распознавании образа - образ подменили?'))
    # 27.10.2026 (the Arabic translation): the exe on the disc is packed - its stub unpacks the program into memory. The
    # recompiler and the game (data\SLUS_009.22) need the program itself: unpacked by the tool of the package build and
    # checked by sha256.
    if v and v.get('exe_unpack'):
        if v['exe_unpack'].lower() != 'lzss':
            raise InstallError('exe_unpack: ' + v['exe_unpack'])
        unp = out + '.unpacked'
        run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'dc_extract', 'unpack_exe.py'), out, unp])
        if not os.path.isfile(unp) or sha256(unp) != v.get('exe_unpacked_sha256', '').lower():
            raise InstallError(L('The game executable did not unpack as expected.',
                                 'Исполняемый файл игры распаковался не так, как ожидалось.'))
        copy_file(unp, out)
        remove_file(unp)
    write_log('SLUS_009.22: ' + n0(os.path.getsize(out)) + ' ' + L('bytes', 'байт'), 'ok')


def step_recompile(tools, fw, proj):
    start_step(L('Recompiling the game to C', 'Рекомпиляция игры в C'))
    recomp_src = os.path.join(fw, 'recompiler')
    recomp_build = os.path.join(recomp_src, 'build')
    if tools['llvm']:
        # 10.10.2026: Clang from the package, by the recorded plan (rebuild.ps1, step 8c - which switches and why). The
        # output is checked: the generated code is byte for byte that of the MSVC build.
        exe = os.path.join(recomp_build, 'psxrecomp-game.exe')
        # 11.10.2026: always anew (5 s) - a recompiler of an earlier, broken off install into the same folder may have
        # been built without the UTF-8 manifest and would fail again on a path with Cyrillic
        if os.path.exists(exe):
            remove_file(exe)
        invoke_plan(tools, 'recompiler.plan.json', os.path.dirname(fw), 256, 0)
    else:
        exe = os.path.join(recomp_build, 'Release', 'psxrecomp-game.exe')
        if not os.path.exists(exe):
            run_tool(tools['cmake'], ['-S', recomp_src, '-B', recomp_build, '-A', 'x64'])
            run_tool(tools['cmake'], ['--build', recomp_build, '--config', 'Release', '--target', 'psxrecomp-game'])
        else:
            write_log(L('recompiler already built', 'рекомпилятор уже собран'), 'ok')
    if not os.path.exists(exe):
        raise InstallError(L('Recompiler did not build: no ' + exe, 'Рекомпилятор не собрался: нет ' + exe))

    run_tool(exe, ['--config', 'game.toml'], workdir=proj, quiet=True)
    full = os.path.join(proj, 'generated', 'SLUS_009.22_full.c')
    if not os.path.exists(full):
        raise InstallError(L('Recompiler produced no generated\\SLUS_009.22_full.c',
                             'Рекомпилятор не выдал generated\\SLUS_009.22_full.c'))
    write_log(L('generated', 'сгенерировано') + ' ' + n0(os.path.getsize(full)) + ' ' + L('bytes of C code', 'байт кода'), 'ok')
    return full


def step_patch_generated(tools, proj, full_c):
    start_step(L('Patching generated code (PGXP + mod hooks)', 'Правка сгенерированного кода (PGXP + хуки мода)'))
    run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'pgxp_transform.py'), full_c])
    # A positional patch too (format 2, 17.09.2026): our hook lines, and where a hook repeats an expression of a game
    # instruction - our bytes and references to the place in YOUR generated code. No expression of the recompiled game
    # is in the patch. 08.10.2026: another game version has its own hooks patch - the same edits moved onto the code
    # from its exe.
    hooks = os.path.join(MOD_DIR, 'generated', 'dcme-hooks.dcpatch')
    v = S.version
    if v and v.get('hooks_patch'):
        hooks = os.path.join(v['dir'], v['hooks_patch'])
    run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'posdiff.py'), 'apply', hooks, proj])
    # the wrappers of the vertex reads for Z-fight are put by a rule, not by ready lines (17.09.2026): the script finds
    # the vertex loads into the GTE in the generated code and wraps them
    run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'zf_vxy_transform.py'), full_c])
    write_log(L('mod hooks injected into the generated code', 'хуки мода вживлены в генкод'), 'ok')
    # the backup copy of the recompiler output gets in the way of a repeated install: the patch is already applied, and
    # pgxp_transform would take it for the source next time
    try:
        remove_file(os.path.join(proj, 'generated', 'SLUS_009.22_full.c.pgxp_orig'))
    except OSError:
        pass


def step_build_runtime(tools, fw, proj):
    start_step(L('Building the runtime (longest step, 5-20 minutes)', 'Сборка рантайма (самый долгий шаг, 5-20 минут)'))
    build = os.path.join(proj, 'build')
    if tools['llvm']:
        # 10.10.2026: Clang from the package, by the recorded plan (rebuild.ps1, step 8c): SDL2 from
        # <framework>/../sdl2-msvc, -O2, the windows subsystem, the .rc resource (the icon and the data pack) and libc++
        # linked in - as set up there and in mod\project\CMakeLists.txt
        invoke_plan(tools, 'game.plan.json', os.path.dirname(fw), 384, 1536)
        exe = os.path.join(build, 'DinoCrisisME.exe')
    else:
        run_tool(tools['cmake'], ['-S', proj, '-B', build, '-A', 'x64', '-DPSXRECOMP_ROOT=' + fw])
        run_tool(tools['cmake'], ['--build', build, '--config', 'RelWithDebInfo', '--target', 'DinoCrisisME'])
        exe = os.path.join(build, 'RelWithDebInfo', 'DinoCrisisME.exe')
    if not os.path.exists(exe):
        raise InstallError(L('Runtime did not build: no ' + exe, 'Рантайм не собрался: нет ' + exe))
    write_log('DinoCrisisME.exe: ' + n0(os.path.getsize(exe)) + ' ' + L('bytes', 'байт'), 'ok')
    return exe


def step_rooms(tools, proj):
    start_step(L('Unpacking rooms from your disc image', 'Распаковка комнат из твоего образа'))
    # 17.09.2026: the rooms go into the build tree, not into data\ - from there they go inside the program
    # (step_pack_data) under the paths they had from the data folder: exported_rooms/library/room_NNN.dcroom
    rooms = os.path.join(proj, 'dcme_data', 'exported_rooms')
    if os.path.exists(rooms):
        _rmtree(_long(rooms), True)
    # --library-only: without it the exporter puts two more copies of every room next to the library (ST*.obj to look
    # at in an editor and ST*.dcroom under the ST name); the game reads only library/room_*.dcroom and two lists
    run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'dc_extract', 'export_rooms.py'), '--library-only',
                               O.disc_path, rooms], quiet=True)
    lib = os.path.join(rooms, 'library')
    try:
        n = sum(1 for x in os.listdir(lib) if fnmatch.fnmatch(x.lower(), 'room_*.dcroom'))
    except OSError:
        n = 0
    if n < 100:
        raise InstallError(L('Only %d rooms unpacked - is the disc image damaged?' % n,
                             'Из образа распаковалось только %d комнат - образ повреждён?' % n))
    write_log(L('rooms unpacked:', 'комнат распаковано:') + ' %d' % n, 'ok')

    # the camera collision is the hand work of the mod author, it comes with the mod; only *.obj - the README next to
    # them is for us, the program needs none
    coll = os.path.join(rooms, 'collision')
    os.makedirs(coll, exist_ok=True)
    copy_glob(os.path.join(MOD_DIR, 'collision'), coll, '*.obj')
    nc = count_files(coll, '*.obj')
    if nc == 0:
        raise InstallError(L('Camera collision is missing from the installer package.', 'В пакете установщика нет камерной коллизии.'))
    write_log(L('camera collision:', 'камерная коллизия:') + ' %d ' % nc + L('files', 'файлов'), 'ok')


def step_pack_data(tools, proj):
    start_step(L('Packing the game data into the program', 'Данные игры - внутрь программы'))
    # 17.09.2026 (the player: "keep dc_user.cfg, input.ini, SLUS_009.22, sew everything else into the exe"): everything
    # that lay in data\ as files, but these three, goes into dcme_data.pak next to CMakeLists.txt; the build puts it into
    # DinoCrisisME.exe as the DCME_DATA resource, and the game reads it (runtime/src/dc_pak.c) under the old file names.
    stage = os.path.join(proj, 'dcme_data')
    os.makedirs(stage, exist_ok=True)

    # The start settings and the room tuning (dc_user.cfg and input.ini stay files: the player changes them). V3
    # (04.10.2026): the tuning that came after V1 too - texture marks, the picture, shadows by room, camera curves.
    for name in ('dc_env.cfg', 'dc_items_user.cfg', 'dc_decals_user.cfg', 'dc_zfsel_user.cfg', 'dc_hiprooms_user.cfg',
                 'dc_twosided_user.cfg', 'dc_material_user.cfg', 'dc_glow_user.cfg', 'dc_highlight_user.cfg',
                 'dc_post_user.cfg', 'dc_shadows_user.cfg', 'dc_cam_curves.cfg', 'dc_zfopt_user.cfg',
                 'dc_strafe_live.cfg'):     # 13.10.2026: walking while aiming (the F7 modes)
        src = os.path.join(MOD_DIR, 'config', name)
        if not os.path.isfile(src):
            raise InstallError(L('Missing from the installer package:', 'В пакете установщика нет файла:') + ' config\\' + name)
        copy_file(src, os.path.join(stage, name))
    # V3: our data - shaders, aim poses, the pictures of the screens and the menu (embed\, dc_embedded() gives them) and
    # the faces of the characters (assets\faces) - into the pack as well
    embed = os.path.join(MOD_DIR, 'assets', 'embed')
    ne = count_files(embed)
    if ne < 100:
        raise InstallError(L('Missing from the installer package:', 'В пакете установщика нет:') + ' assets\\embed (%d)' % ne)
    os.makedirs(os.path.join(stage, 'embed'), exist_ok=True)
    copy_glob(embed, os.path.join(stage, 'embed'))
    faces_dst = os.path.join(stage, 'assets', 'faces')
    faces = os.path.join(MOD_DIR, 'assets', 'faces')
    if os.path.isdir(faces):
        os.makedirs(faces_dst, exist_ok=True)
        copy_glob(faces, faces_dst, '*.bin')
    # 08.10.2026: the mouth for the voice of another version - its face files over ours (the same names)
    v = S.version
    if v and v.get('faces'):
        vf = os.path.join(v['dir'], v['faces'])
        if os.path.isdir(vf):
            os.makedirs(faces_dst, exist_ok=True)
            copy_glob(vf, faces_dst, '*.bin')
            write_log(L('faces for the voice of this version:', 'лица под озвучку этой версии:') + ' %d' % count_files(vf, '*.bin'), 'ok')
    write_log(L('our data:', 'наши данные:') + ' embed %d, ' % ne + L('faces', 'лица') + ' %d' % count_files(faces_dst), 'ok')
    # the licence of the mod and the list of third-party parts - inside the program too
    for doc in ('LICENSE.txt', 'THIRD-PARTY.txt'):
        src = os.path.join(MOD_DIR, doc)
        if os.path.isfile(src):
            copy_file(src, os.path.join(stage, doc))

    # game.toml points at the image of the user where it lies: no need to copy 400+ MB. The path with "/": in a TOML
    # string the backslash escapes. UTF-8 without a byte order mark.
    line = 'disc = "' + O.disc_path.replace('\\', '/') + '"'
    toml = set_version_entry(read_text(os.path.join(MOD_DIR, 'project', 'game.toml')))
    if re.search(r'^disc\s*=', toml, re.M | re.I):
        toml = re.sub(r'^disc\s*=[^\r\n]*', lambda m: line, toml, flags=re.M | re.I)
    else:
        toml = re.sub(r'^(exe\s*=[^\r\n]*)', lambda m: m.group(1) + '\r\n' + line, toml, flags=re.M | re.I)
    write_text(os.path.join(stage, 'game.toml'), toml)

    pak = os.path.join(proj, 'dcme_data.pak')
    run_tool(tools['python'], [os.path.join(MOD_DIR, 'tools', 'pack_data.py'), stage, pak])
    if not os.path.isfile(pak):
        raise InstallError(L('The game data was not packed.', 'Данные игры не упаковались.'))
    write_log('dcme_data.pak: ' + n0(os.path.getsize(pak)) + ' ' + L('bytes', 'байт'), 'ok')


def step_layout(game_dir, runtime_exe, sdl2_dir, proj):
    start_step(L('Laying out the game folder', 'Раскладка готовой папки'))
    # 17.09.2026 (the player: "remove all the extra files left after the install"; "keep dc_user.cfg, input.ini,
    # SLUS_009.22, sew everything else into the exe; this is the mod release, no debug and no debug files"): next to the
    # exe only the exe itself, SDL2.dll (Windows looks for it in the program folder) and data\ with these three files;
    # the memory cards and quicksaves the game makes there itself. The game has no log any more.
    data = os.path.join(game_dir, 'data')

    # The folder of an earlier version. V0 (09.09) kept everything next to the exe, V1 (17.09) in data\. The saves of the
    # player move into data\, our old files go - only by exact names and only if this really is the mod folder: the old
    # exe lies here together with game.toml (V0) or data\game.toml (V1).
    has_exe = (os.path.isfile(os.path.join(game_dir, 'DinoCrisisME.exe'))
               or os.path.isfile(os.path.join(game_dir, 'DinoCrisisMCM.exe')))
    was_v0 = has_exe and os.path.isfile(os.path.join(game_dir, 'game.toml'))
    was_v1 = has_exe and os.path.isfile(os.path.join(data, 'game.toml'))
    os.makedirs(game_dir, exist_ok=True)
    os.makedirs(data, exist_ok=True)
    moved = removed = 0
    if was_v0:
        saves = ['card1.mcd', 'card2.mcd'] + [n for n in sorted(os.listdir(game_dir), key=str.casefold)
                                              if fnmatch.fnmatch(n.lower(), 'quicksave*.dcss')
                                              and os.path.isfile(os.path.join(game_dir, n))]
        for name in saves:
            src = os.path.join(game_dir, name)
            if not os.path.isfile(src):
                continue
            dst = os.path.join(data, name)
            if os.path.exists(dst):
                remove_file(src)                       # the one in data\ is newer
            else:
                shutil.move(src, dst)
                moved += 1
        for name in ('dc_user.cfg', 'input.ini', 'SLUS_009.22', 'DinoCrisisMCM.exe'):
            p = os.path.join(game_dir, name)
            if os.path.isfile(p):
                remove_file(p)
                removed += 1
    if was_v0 or was_v1:
        # what is inside the program now and what the debugging of earlier versions wrote (the log, reports, the debug
        # keys: 1/2 - dc_debug.cfg, F3 - room_dump.obj, F6 - the pt_dump folder). The mark of the start screens lives in
        # dc_user.cfg now (boot.seen), and that is put anew - the screens show again.
        gone = ('dc_env.cfg', 'dc_items_user.cfg', 'dc_decals_user.cfg', 'dc_zfsel_user.cfg', 'dc_hiprooms_user.cfg',
                'dc_zfopt_user.cfg', 'dc_plates_user.cfg', 'dc_debug.cfg', 'dc_boot_seen.cfg', 'game.toml', 'disc.cfg',
                'play.txt', 'psx_last_run_report.json', 'starvation_dump.jsonl',
                'psx_crash.txt', 'room_dump.obj', 'camera_boxes.txt')
        for d in (game_dir, data):
            for name in gone:
                p = os.path.join(d, name)
                if os.path.isfile(p):
                    remove_file(p)
                    removed += 1
        # 18.09.2026 (the player: "bring the needed licence files back into the installed game"): LICENSE.txt and
        # THIRD-PARTY.txt lie next to the game again, so they are out of the general cleanup; in data\ they must not be
        for name in ('LICENSE.txt', 'THIRD-PARTY.txt'):
            p = os.path.join(data, name)
            if os.path.isfile(p):
                remove_file(p)
                removed += 1
        for sub in ('fonts', 'exported_rooms', 'pt_dump'):
            p = os.path.join(data, sub)
            if os.path.isdir(p):
                _rmtree(_long(p), True)
                removed += 1
        write_log(L('previous version: saves moved into data\\', 'прошлая версия: сейвы перенесены в data\\')
                  + ' (%d), ' % moved + L('old files removed', 'старые файлы удалены') + ' (%d)' % removed, 'ok')

    copy_file(runtime_exe, os.path.join(game_dir, os.path.basename(runtime_exe)))
    copy_file(os.path.join(sdl2_dir, 'lib', 'x64', 'SDL2.dll'), os.path.join(game_dir, 'SDL2.dll'))

    # V3 (04.10.2026, the player: "a mods folder with subfolders in the folder the game is installed to"): the game looks
    # into mods at start - a file named like a game file replaces it (runtime/src/dc_mods.c; our rooms and collision -
    # dc_pak.c). No options in the game. What already lies in mods is left alone on a reinstall. V4 (05.10.2026): the
    # folders as in the PC version of the game - movie, sound\bgm, sound\se, sound\voice; the game still reads the old V3
    # folders (dc_mods.c) - the empty ones are removed, those with files of the player stay. V6 (06.10.2026): weapons.
    mods = os.path.join(game_dir, 'mods')
    for sub in ('rooms', 'doors', 'characters', 'weapons', 'items', 'screens', 'movie', 'sound\\bgm', 'sound\\se',
                'sound\\voice', 'textures', 'collision', 'other'):
        os.makedirs(os.path.join(mods, sub), exist_ok=True)
    for old in ('sounds\\effects', 'sounds\\stage', 'sounds\\speech', 'sounds\\music', 'sounds', 'videos'):
        p = os.path.join(mods, old)
        if os.path.isdir(p) and not os.listdir(p):
            try:
                os.rmdir(p)
            except PermissionError:
                os.chmod(p, stat.S_IWRITE)
                os.rmdir(p)
    write_log(L('mods folder:', 'папка mods:') + ' ' + mods, 'ok')

    # 06.10.2026: mods\names.txt - every mesh of the game by name; a model under its name in mods the game takes by itself
    # (dc_meshrep.cpp). The modding app is a separate download; the list is made when the package is built (rebuild.ps1,
    # step 2) and the modding app rewrites it at every start.
    names = os.path.join(MOD_DIR, 'mods', 'names.txt')
    if os.path.isfile(names):
        copy_file(names, os.path.join(mods, 'names.txt'))
        write_log(L('model names for mods:', 'имена моделей для mods:') + ' ' + mods + '\\names.txt', 'ok')
    else:
        write_log(L('not in the package:', 'нет в пакете:') + ' mods\\names.txt', 'warn')

    # 08.10.2026: another version of the game stood in this folder - its quicksaves do not suit the new one (a snapshot of
    # the whole memory, laid out differently by each version). They move into a subfolder of data\, nothing is deleted;
    # the memory card saves stay.
    old_exe = os.path.join(data, 'SLUS_009.22')
    v = S.version
    if v and os.path.isfile(old_exe):
        old_hash = sha256(old_exe)
        # 27.10.2026: for a version with a packed exe, data\ holds the unpacked one (exe_unpacked_sha256)
        if old_hash != v['exe_sha256'].lower() and old_hash != v.get('exe_unpacked_sha256', '').lower():
            old_id = 'previous'
            for o in get_versions():
                if o['exe_sha256'].lower() == old_hash or o.get('exe_unpacked_sha256', '').lower() == old_hash:
                    old_id = o['id']
            # 13.10.2026: the game keeps the quicksaves in data\quicksaves (earlier versions - right in data)
            qs = []
            for folder in (data, os.path.join(data, 'quicksaves')):
                try:
                    listing = sorted(os.listdir(folder), key=str.casefold)
                except OSError:
                    continue
                for n in listing:
                    if ((fnmatch.fnmatch(n.lower(), 'quicksave*.dcss') or fnmatch.fnmatch(n.lower(), 'quicksave*.story'))
                            and os.path.isfile(os.path.join(folder, n))):
                        qs.append(os.path.join(folder, n))
            if qs:
                keep = os.path.join(data, 'quicksaves_' + old_id)
                os.makedirs(keep, exist_ok=True)
                for q in qs:
                    dst = os.path.join(keep, os.path.basename(q))
                    if os.path.isfile(dst):
                        remove_file(dst)
                    shutil.move(q, dst)
                write_log(L('another game version was installed here: its quicksaves moved to',
                            'здесь стояла другая версия игры: её быстрые сохранения перенесены в') + ' ' + keep, 'warn')

    # Shaders, aim poses, menu pictures, faces and the game data (dcme_data.pak) are inside the executable. As files -
    # only what the player changes and the game executable from the player's image (V3: plus the mods folder the player fills).
    copy_file(os.path.join(MOD_DIR, 'config', 'dc_user.cfg'), os.path.join(data, 'dc_user.cfg'))
    copy_file(os.path.join(MOD_DIR, 'config', 'input.ini'), os.path.join(data, 'input.ini'))
    copy_file(os.path.join(proj, 'SLUS_009.22'), os.path.join(data, 'SLUS_009.22'))

    # 18.09.2026: LICENSE.txt and THIRD-PARTY.txt lie next to the game, to be seen; copies stay inside the program too
    for doc in ('LICENSE.txt', 'THIRD-PARTY.txt'):
        src = os.path.join(MOD_DIR, doc)
        if os.path.isfile(src):
            copy_file(src, os.path.join(game_dir, doc))
        else:
            write_log(L('not in the package:', 'нет в пакете:') + ' ' + doc, 'warn')

    # 11.10.2026 (the player: "after the install an uninstall exe is needed in the same folder"): build_setup.ps1 builds
    # Uninstall.exe (installer\Uninstall.cs) and puts it into the package next to this script
    uninst = os.path.join(SCRIPT_DIR, 'Uninstall.exe')
    if os.path.isfile(uninst):
        copy_file(uninst, os.path.join(game_dir, 'Uninstall.exe'))
    else:
        write_log(L('not in the package:', 'нет в пакете:') + ' Uninstall.exe', 'warn')

    write_log(L('game folder ready:', 'папка игры готова:') + ' ' + game_dir, 'ok')


# ─────────────────────────── the shortcut ──────────────────────────────────────
_CLSID_SHELLLINK = '{00021401-0000-0000-C000-000000000046}'
_IID_ISHELLLINKW = '{000214F9-0000-0000-C000-000000000046}'
_IID_IPERSISTFILE = '{0000010B-0000-0000-C000-000000000046}'


def _desktop_dir():
    import ctypes
    shell32 = ctypes.WinDLL('shell32')
    shell32.SHGetFolderPathW.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p)
    shell32.SHGetFolderPathW.restype = ctypes.c_long
    buf = ctypes.create_unicode_buffer(1024)
    hr = shell32.SHGetFolderPathW(None, 0x0010, None, 0, buf)        # CSIDL_DESKTOPDIRECTORY
    if hr != 0 or not buf.value:
        raise OSError('SHGetFolderPathW: 0x%08X' % (hr & 0xFFFFFFFF))
    return buf.value


def _shell_link(lnk, target=None, workdir='', description='', icon=''):
    """Reads (target None: returns the target path) or writes a .lnk file through IShellLinkW and IPersistFile - the
    object WScript.Shell of dcme-install.ps1 works through. Wide strings all the way, so a non-Latin letter in the game
    folder does not break the shortcut (WScript.Shell answered "Value does not fall within the expected range")."""
    import ctypes
    ole32 = ctypes.WinDLL('ole32')
    ole32.CoInitializeEx.argtypes = (ctypes.c_void_p, ctypes.c_ulong)
    ole32.CoInitializeEx.restype = ctypes.c_long
    ole32.CoUninitialize.argtypes = ()
    ole32.CoUninitialize.restype = None
    ole32.CLSIDFromString.argtypes = (ctypes.c_wchar_p, ctypes.c_void_p)
    ole32.CLSIDFromString.restype = ctypes.HRESULT
    ole32.CoCreateInstance.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p)
    ole32.CoCreateInstance.restype = ctypes.HRESULT

    def guid(s):
        g = (ctypes.c_ubyte * 16)()
        ole32.CLSIDFromString(s, g)
        return g

    def vtable(obj):
        return ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

    def method(obj, index, *argtypes):
        f = ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p, *argtypes)(vtable(obj)[index])
        return lambda *a: f(obj, *a)

    hr0 = ole32.CoInitializeEx(None, 2)                          # COINIT_APARTMENTTHREADED
    link, persist = ctypes.c_void_p(), ctypes.c_void_p()
    try:
        ole32.CoCreateInstance(guid(_CLSID_SHELLLINK), None, 1, guid(_IID_ISHELLLINKW), ctypes.byref(link))
        method(link, 0, ctypes.c_void_p, ctypes.c_void_p)(guid(_IID_IPERSISTFILE), ctypes.byref(persist))   # QueryInterface
        load = method(persist, 5, ctypes.c_wchar_p, ctypes.c_ulong)                                          # IPersistFile::Load
        if target is None:
            load(lnk, 0)                                                                                     # STGM_READ
            buf = ctypes.create_unicode_buffer(32768)
            method(link, 3, ctypes.c_wchar_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong)(buf, len(buf), None, 0)  # GetPath
            return buf.value
        if os.path.exists(lnk):
            try:
                load(lnk, 0)                 # as WScript.Shell: an existing shortcut keeps what we do not set
            except OSError:
                pass
        method(link, 20, ctypes.c_wchar_p)(target)                                 # SetPath
        method(link, 9, ctypes.c_wchar_p)(workdir)                                 # SetWorkingDirectory
        method(link, 7, ctypes.c_wchar_p)(description)                             # SetDescription
        method(link, 17, ctypes.c_wchar_p, ctypes.c_int)(icon, 0)                  # SetIconLocation
        method(persist, 6, ctypes.c_wchar_p, ctypes.c_int)(lnk, 1)                 # IPersistFile::Save
        return target
    finally:
        for obj in (persist, link):
            if obj.value:
                ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtable(obj)[2])(obj)   # Release
        if hr0 >= 0:
            ole32.CoUninitialize()


def _shell_notify(lnk):
    """18.09.2026: the shell is asked to read the entry again (the same two calls as dcme-install.ps1)."""
    import ctypes
    shell32 = ctypes.WinDLL('shell32')
    shell32.SHChangeNotify.argtypes = (ctypes.c_long, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p)
    shell32.SHChangeNotify.restype = None
    shell32.SHChangeNotify(0x00002000, 0x1005, None, None)
    item = ctypes.create_unicode_buffer(lnk)
    shell32.SHChangeNotify(0x00002000, 0x0005, ctypes.addressof(item), None)          # + the shortcut itself


def step_shortcut(game_dir):
    start_step(L('Desktop shortcut', 'Ярлык'))
    if O.no_shortcut:
        write_log(L('-NoShortcut: leaving the desktop alone', 'ключ -NoShortcut: рабочий стол не трогаю'), 'ok')
        return
    # the shortcut leads right to the game: it reads its start settings itself (dc_env.cfg, runtime/src/dc_boot.c)
    exe = os.path.join(game_dir, 'DinoCrisisME.exe')
    if not os.path.exists(exe):
        write_log(L('launcher not found, skipping the shortcut', 'файл запуска не найден, ярлык не делаю'), 'warn')
        return
    # the shortcut of the earlier version ("Dino Crisis MCM"), if it led into this folder: its exe was removed by the
    # layout, the shortcut would stay dead
    try:
        old = os.path.join(_desktop_dir(), 'Dino Crisis MCM.lnk')
        if os.path.exists(old):
            t0 = _shell_link(old)
            if t0 and t0.casefold() == os.path.join(game_dir, 'DinoCrisisMCM.exe').casefold():
                remove_file(old)
    except Exception:
        pass
    try:
        lnk = os.path.join(_desktop_dir(), 'Dino Crisis ME.lnk')
        # 18.09.2026 (the player: "the shortcut does not show for some reason"): the icon in the exe is whole (resource 1,
        # ten frames), the blank sheet on the desktop is a stuck icon cache of the shell - the shortcut is made right
        # after the build. So the icon is named EXPLICITLY and the shell is asked to read the entry again.
        _shell_link(lnk, exe, game_dir, 'Dino Crisis ME', exe)
        write_log(L('desktop shortcut:', 'ярлык на рабочем столе:') + ' ' + lnk, 'ok')
        try:
            _shell_notify(lnk)
            write_log(L('the shell was asked to refresh the icon', 'оболочка попрошена перечитать значок'), 'ok')
        except Exception as e:
            write_log(L('icon refresh skipped:', 'обновление значка пропущено:') + ' %s' % e, 'warn')
    except Exception as e:
        write_log(L('shortcut not created:', 'ярлык не создался:') + ' %s' % e, 'warn')


# ─────────────────────────── the command line ──────────────────────────────────
_PARAMS = (('DiscPath', str), ('InstallPath', str), ('RecompZip', str), ('SkipPrereq', bool), ('KeepBuildTree', bool),
           ('NoShortcut', bool), ('RemoveTools', bool), ('Lang', str), ('DryRun', bool), ('Toolchain', str))
_POSITIONAL = ('DiscPath', 'InstallPath', 'RecompZip', 'Lang', 'Toolchain')


def parse_args(argv):
    """The parameters of dcme-install.ps1: -Name value, -Name:value (also --name and =), switches; names in any case and
    shortened while they stay unambiguous; DiscPath, InstallPath, RecompZip, Lang and Toolchain also by position."""
    kinds = dict(_PARAMS)
    got = {}
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        i += 1
        if len(a) < 2 or a[0] != '-':
            pos.append(a)
            continue
        body = a.lstrip('-')
        val = None
        cut = [k for k in (body.find(':'), body.find('=')) if k > 0]
        if cut:
            body, val = body[:min(cut)], body[min(cut) + 1:]
        hits = ([n for n in kinds if n.lower() == body.lower()]
                or [n for n in kinds if body and n.lower().startswith(body.lower())])
        if len(hits) != 1:
            raise UsageError('A parameter cannot be found that matches parameter name \'%s\'.' % body)
        name = hits[0]
        if kinds[name] is bool:
            got[name] = val is None or val.strip().lower() not in ('$false', 'false', '0')
            continue
        if val is None:
            if i >= len(argv):
                raise UsageError('Missing an argument for parameter \'%s\'.' % name)
            val = argv[i]
            i += 1
        got[name] = val
    for name in _POSITIONAL:
        if pos and name not in got:
            got[name] = pos.pop(0)
    if pos:
        raise UsageError('A positional parameter cannot be found that accepts argument \'%s\'.' % pos[0])
    for name in ('DiscPath', 'InstallPath'):
        if not got.get(name):
            raise UsageError('Missing the mandatory parameter -%s.' % name)
    for name, allowed in (('Lang', ('en', 'ru')), ('Toolchain', ('auto', 'llvm', 'msvc'))):
        if name in got and got[name].lower() not in allowed:
            raise UsageError('Cannot validate argument on parameter \'%s\'. The argument "%s" does not belong to the set "%s".'
                             % (name, got[name], ','.join(allowed)))
    O.disc_path = os.path.abspath(got['DiscPath'])
    O.install_path = os.path.abspath(got['InstallPath'])
    O.recomp_zip = os.path.abspath(got['RecompZip']) if got.get('RecompZip') else ''
    O.skip_prereq = got.get('SkipPrereq', False)
    O.keep_build_tree = got.get('KeepBuildTree', False)
    O.no_shortcut = got.get('NoShortcut', False)
    O.remove_tools = got.get('RemoveTools', False)
    O.lang = got.get('Lang', 'en').lower()
    O.dry_run = got.get('DryRun', False)
    O.toolchain = got.get('Toolchain', 'auto').lower()


# ──────────────────────────────── the run ──────────────────────────────────────
def main(argv=None):
    _setup_output()
    try:
        parse_args(sys.argv[1:] if argv is None else argv)
    except UsageError as e:
        write_log(str(e), 'err')
        return 1
    S.ru = O.lang == 'ru'
    # 09.10.2026: a Python writes into a pipe in the ANSI code page, and the pipe is read as UTF-8 - ask for UTF-8 (the
    # path of python.exe under a Cyrillic user name came out broken, and the Python was "not found")
    os.environ['PYTHONIOENCODING'] = 'utf-8'
    try:
        write_log('==== Dino Crisis ME - ' + L('mod setup', 'установка мода') + ' ====', 'step')
        write_log(L('disc image :', 'образ игры :') + ' ' + O.disc_path)
        write_log(L('install to :', 'куда ставим:') + ' ' + O.install_path)

        if not os.path.isfile(O.disc_path):
            raise InstallError(L('Disc image not found: ' + O.disc_path, 'Образ игры не найден: ' + O.disc_path))
        if os.path.getsize(O.disc_path) < 100 * 1024 * 1024:
            raise InstallError(L('%s is too small for a disc image. Point at the Track 1 .bin, not the .cue.' % O.disc_path,
                                 'Файл %s слишком мал для образа диска. Нужен .bin дорожки 1, а не .cue.' % O.disc_path))
        if not os.path.exists(os.path.join(MOD_DIR, 'patches', 'psxrecomp-dcme.dcpatch')):
            raise InstallError(L('No mod folder next to the installer - unpack the whole archive.',
                                 'Рядом с установщиком нет папки mod - распакуй архив целиком.'))

        tools = test_prerequisites()
        S.version = find_game_version(tools)
        if O.dry_run:
            write_log(L('-DryRun: checks passed, nothing was installed.', 'Ключ -DryRun: проверки пройдены, установка не выполнялась.'), 'ok')
            return 0

        # the game goes right into the chosen folder; the build tree next to it is removed at the end
        src_root = os.path.join(O.install_path, '.build-src')
        game_dir = O.install_path
        os.makedirs(src_root, exist_ok=True)

        fw = step_framework(src_root)
        step_apply_mod(tools, fw)
        sdl = step_sdl2(src_root)
        proj = step_project(src_root)
        step_extract_exe(tools, proj)
        full = step_recompile(tools, fw, proj)
        step_patch_generated(tools, proj, full)
        step_rooms(tools, proj)                          # the rooms - into the build tree
        step_pack_data(tools, proj)                      # the game data - into dcme_data.pak, before the build
        exe = step_build_runtime(tools, fw, proj)
        step_layout(game_dir, exe, sdl, proj)
        step_shortcut(game_dir)

        if not O.keep_build_tree:
            # ALL of build-src goes, not only the object files: 160 MB of the recompiler framework with its documents and
            # 15 MB of generated code of the game - the game needs none of it. -KeepBuildTree keeps it: a repeated install
            # then neither downloads nor builds the framework again.
            write_log(L('removing the build tree...', 'убираю дерево сборки...'))
            before = tree_size(src_root)
            remove_tree(src_root)       # 23.10.2026: paths of 260 characters and more too
            write_log(L('freed', 'освобождено') + ' ' + n0(round_half_up(before / 1048576)) + ' ' + L('MB', 'МБ'), 'ok')

        if O.remove_tools:
            remove_installed_tools()

        write_log('==== ' + L('Done', 'Готово') + ' ====', 'step')
        write_log(L('Run:', 'Запуск:') + ' ' + os.path.join(game_dir, 'DinoCrisisME.exe'), 'ok')
        # the build tools were needed only here: the game is an ordinary executable. 10.10: the compiler, Python, Vulkan and
        # SDL2 come with the mod - nothing was installed and there is nothing to say; Build Tools only on the old way
        if not tools['llvm']:
            write_log(L('The build tools (Visual Studio Build Tools) were only needed for setup - you can uninstall them.',
                        'Программы сборки (Visual Studio Build Tools) нужны были только для установки - их можно удалить.'), 'ok')
        return 0
    except Exception as e:
        write_log(str(e) if isinstance(e, InstallError) else '%s: %s' % (type(e).__name__, e), 'err')
        # where it happened, as the script stack of dcme-install.ps1 (the innermost call first). 27.10.2026: not
        # traceback.format_exc - with a non-ASCII source line it wants unicodedata, and an error inside the error output
        # left the window with a raw Python traceback
        try:
            for f, n in reversed(list(traceback.walk_tb(e.__traceback__))):
                write_log('at %s, %s: line %d' % (f.f_code.co_name, f.f_code.co_filename, n))
        except Exception:
            pass
        return 1


if __name__ == '__main__':
    sys.exit(main())
