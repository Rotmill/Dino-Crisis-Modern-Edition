# -*- coding: utf-8 -*-
"""Dino Crisis ME - native Linux setup (17.11.2026).

The same install as dcme_install.py (its steps are reused as they are), built into a Linux program instead of
DinoCrisisME.exe: the system's compiler, CMake, SDL2, Vulkan, libpng and FFmpeg instead of the tools the Windows
package carries. Nothing of the game comes with the mod - the program is recompiled from the player's own disc image.

    python3 installer/dcme_install_linux.py -DiscPath "/path/Dino Crisis (USA) (v1.1) (Track 1).bin" \\
        -InstallPath ~/Games/DinoCrisisME

The same parameters as dcme_install.py (-RecompZip, -KeepBuildTree, -NoShortcut, -Lang ru, -DryRun); -Toolchain picks
the compiler: auto (Clang when there is one - the compiler of the Windows release - else GCC), clang or gcc.

Steps:
   1  checks the build tools (compiler, CMake, Ninja or make, pkg-config: sdl2, vulkan, libpng, libav*)
   2  recognises the disc image (as on Windows)
   3  the psxrecomp framework at the pinned commit
   4  applies the mod patch and puts our runtime files in
   5  the game project
   6  SLUS_009.22 from the player's disc image
   7  builds the recompiler and recompiles the game
   8  PGXP and the mod hooks in the generated code
   9  the rooms from the disc image
  10  the game data into dcme_data.pak (a file next to the program on Linux)
  11  builds DinoCrisisME
  12  lays out the game folder (+ the launcher DinoCrisisME.sh)
  13  the menu entry (~/.local/share/applications)
"""
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcme_install as W  # noqa: E402

L = W.L
write_log = W.write_log
InstallError = W.InstallError

W.TOTAL_STEPS = 13

LAUNCHER = r'''#!/usr/bin/env bash
# Dino Crisis ME (native Linux build). The recompiled game calls deeply on the native stack: the Windows build links a
# 64 MB main stack, here the stack limit is raised to the same before the game starts. The output goes to
# DinoCrisisME_log.txt next to the game (and to the terminal when there is one).
here=$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)
want=65536
cur=$(ulimit -s)
if [ "$cur" != "unlimited" ] && [ "$cur" -lt "$want" ]; then
  ulimit -s "$want" 2>/dev/null || ulimit -s "$(ulimit -H -s)" 2>/dev/null || true
fi
log="$here/DinoCrisisME_log.txt"
if [ -t 2 ]; then
  "$here/DinoCrisisME" "$@" 2>&1 | tee "$log"
  exit "${PIPESTATUS[0]}"
fi
exec "$here/DinoCrisisME" "$@" > "$log" 2>&1
'''

PKG_MODULES = (('sdl2', 'SDL2'), ('vulkan', 'Vulkan loader'), ('libpng', 'libpng'), ('libavformat', 'FFmpeg'),
               ('libavcodec', 'FFmpeg'), ('libavutil', 'FFmpeg'), ('libswscale', 'FFmpeg'),
               ('libswresample', 'FFmpeg'))

INSTALL_HINT = {
    'apt': 'sudo apt install build-essential clang cmake ninja-build pkg-config libsdl2-dev libvulkan-dev libpng-dev '
           'libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev',
    'dnf': 'sudo dnf install clang gcc-c++ cmake ninja-build pkgconf SDL2-devel vulkan-loader-devel libpng-devel '
           'ffmpeg-free-devel',
    'pacman': 'sudo pacman -S --needed base-devel clang cmake ninja pkgconf sdl2 vulkan-icd-loader vulkan-headers '
              'libpng ffmpeg',
    'zypper': 'sudo zypper install clang gcc-c++ cmake ninja pkg-config libSDL2-devel vulkan-devel libpng16-devel '
              'ffmpeg-devel',
}


def _hint():
    for pm, cmd in INSTALL_HINT.items():
        if shutil.which(pm):
            return cmd
    return INSTALL_HINT['apt']


def _mem_available_mb():
    try:
        with open('/proc/meminfo') as f:
            for line in f:
                if line.startswith('MemAvailable:'):
                    return int(line.split()[1]) // 1024
    except (OSError, ValueError, IndexError):
        pass
    return 4096


def build_jobs():
    """Parallel jobs: the cores, but not more than the free memory takes (the generated game is one big C file -
    about 1.5 GB for Clang -O2 - and every other file about 400 MB)."""
    cores = os.cpu_count() or 2
    mb = _mem_available_mb() - 1536
    return max(1, min(cores, mb // 400 if mb > 0 else 1))


def test_prerequisites():
    W.start_step(L('Checking build tools', 'Проверка инструментов сборки'))
    missing = []
    want = W.O.toolchain
    cc = cxx = None
    if want in ('auto', 'clang', 'llvm') and shutil.which('clang') and shutil.which('clang++'):
        cc, cxx = shutil.which('clang'), shutil.which('clang++')
    elif want in ('auto', 'gcc', 'msvc') and shutil.which('gcc') and shutil.which('g++'):
        cc, cxx = shutil.which('gcc'), shutil.which('g++')
    if not cc:
        missing.append(L('a C/C++ compiler (clang or gcc)', 'компилятор C/C++ (clang или gcc)'))
    else:
        write_log(L('compiler:', 'компилятор:') + ' %s / %s' % (cc, cxx), 'ok')
    cmake = shutil.which('cmake')
    if not cmake:
        missing.append('cmake')
    else:
        write_log('CMake: ' + cmake, 'ok')
    ninja = shutil.which('ninja') or shutil.which('ninja-build')
    if not ninja and not shutil.which('make'):
        missing.append('ninja (or make)')
    pkg = shutil.which('pkg-config') or shutil.which('pkgconf')
    if not pkg:
        missing.append('pkg-config')
    else:
        for mod, human in PKG_MODULES:
            r = subprocess.run([pkg, '--modversion', mod], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            if r.returncode != 0:
                if human not in missing:
                    missing.append(human)
            else:
                write_log('%s %s' % (mod, r.stdout.strip()), 'ok')
    if missing:
        raise InstallError(L('Missing build tools: %s. Install them, for example:\n      %s'
                             % (', '.join(missing), _hint()),
                             'Нет инструментов сборки: %s. Поставь их, например:\n      %s'
                             % (', '.join(missing), _hint())))
    if not shutil.which('vulkaninfo'):
        write_log(L('vulkaninfo not found - make sure the Vulkan driver of your GPU is installed (mesa-vulkan-drivers, '
                    'nvidia-utils, ...)', 'нет vulkaninfo - проверь, что стоит драйвер Vulkan видеокарты'), 'warn')
    return {'python': sys.executable, 'cmake': cmake, 'ninja': ninja, 'cc': cc, 'cxx': cxx, 'vulkan': None,
            'llvm': None}


def _generator_args(tools):
    return ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + tools['ninja']] if tools['ninja'] else ['-G', 'Unix Makefiles']


def _cmake_common(tools):
    # -O2 like the Windows release (Clang from llvm-mingw, recorded build plan), not CMake's -O3
    return ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=' + tools['cc'], '-DCMAKE_CXX_COMPILER=' + tools['cxx'],
            '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG']


def step_recompile(tools, fw, proj):
    W.start_step(L('Recompiling the game to C', 'Рекомпиляция игры в C'))
    src = os.path.join(fw, 'recompiler')
    build = os.path.join(src, 'build-linux')
    exe = os.path.join(build, 'psxrecomp-game')
    if os.path.exists(exe):
        W.remove_file(exe)
    W.run_tool(tools['cmake'], ['-S', src, '-B', build] + _generator_args(tools) + _cmake_common(tools), quiet=True)
    W.run_tool(tools['cmake'], ['--build', build, '--target', 'psxrecomp-game', '--parallel', str(build_jobs())],
               quiet=True)
    if not os.path.exists(exe):
        raise InstallError(L('Recompiler did not build: no ' + exe, 'Рекомпилятор не собрался: нет ' + exe))
    W.run_tool(exe, ['--config', 'game.toml'], workdir=proj, quiet=True)
    full = os.path.join(proj, 'generated', 'SLUS_009.22_full.c')
    if not os.path.exists(full):
        raise InstallError(L('Recompiler produced no generated/SLUS_009.22_full.c',
                             'Рекомпилятор не выдал generated/SLUS_009.22_full.c'))
    write_log(L('generated', 'сгенерировано') + ' ' + W.n0(os.path.getsize(full)) + ' ' + L('bytes of C code', 'байт кода'), 'ok')
    return full


def step_build_runtime(tools, fw, proj):
    W.start_step(L('Building the game (longest step, 5-20 minutes)', 'Сборка игры (самый долгий шаг, 5-20 минут)'))
    build = os.path.join(proj, 'build-linux')
    W.run_tool(tools['cmake'], ['-S', proj, '-B', build, '-DPSXRECOMP_ROOT=' + fw] + _generator_args(tools) +
               _cmake_common(tools), quiet=True)
    jobs = build_jobs()
    write_log(L('parallel jobs:', 'параллельных заданий:') + ' %d' % jobs)
    W.run_tool(tools['cmake'], ['--build', build, '--target', 'DinoCrisisME', '--parallel', str(jobs)], quiet=True)
    exe = os.path.join(build, 'DinoCrisisME')
    if not os.path.exists(exe):
        raise InstallError(L('The game did not build: no ' + exe, 'Игра не собралась: нет ' + exe))
    write_log('DinoCrisisME: ' + W.n0(os.path.getsize(exe)) + ' ' + L('bytes', 'байт'), 'ok')
    return exe


def distrobox_name():
    """The distrobox this setup runs in (SteamOS: the root filesystem is read-only, so the build tools live in a
    container), '' outside one. The game needs the container's libraries (FFmpeg, SDL2), so it is started through it."""
    name = os.environ.get('CONTAINER_ID', '')
    if name and (os.path.exists('/run/.containerenv') or os.path.exists('/.dockerenv') or os.environ.get('DISTROBOX_ENTER_PATH')):
        return name
    return ''


HOST_LAUNCHER = r'''#!/usr/bin/env bash
# Dino Crisis ME from the host (SteamOS Game Mode, the desktop menu): the game was built in the distrobox "%s" and
# runs inside it, with its libraries. Nothing is installed on the read-only system partition.
here=$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)
if [ -n "${CONTAINER_ID:-}" ]; then exec "$here/DinoCrisisME.sh" "$@"; fi
exec distrobox enter %s -- "$here/DinoCrisisME.sh" "$@"
'''


def step_layout(game_dir, exe, proj):
    W.step_layout(game_dir, exe, None, proj)          # the folder as on Windows: data/, mods/, licences, old versions
    os.chmod(os.path.join(game_dir, 'DinoCrisisME'), 0o755)
    # PE resources do not exist here: the data pack lies next to the program (dc_pak.c, DCME_DATA_PAK_PATH)
    W.copy_file(os.path.join(proj, 'dcme_data.pak'), os.path.join(game_dir, 'dcme_data.pak'))
    launcher = os.path.join(game_dir, 'DinoCrisisME.sh')
    with open(launcher, 'w', encoding='utf-8', newline='\n') as f:
        f.write(LAUNCHER)
    os.chmod(launcher, 0o755)
    icon_src = os.path.join(W.SCRIPT_DIR, 'dc_icon_source_512.png')
    if os.path.isfile(icon_src):
        W.copy_file(icon_src, os.path.join(game_dir, 'DinoCrisisME.png'))
    write_log(L('launcher:', 'запуск:') + ' ' + launcher, 'ok')
    box = distrobox_name()
    if box:
        host = os.path.join(game_dir, 'DinoCrisisME-host.sh')
        with open(host, 'w', encoding='utf-8', newline='\n') as f:
            f.write(HOST_LAUNCHER % (box, box))
        os.chmod(host, 0o755)
        write_log(L('distrobox "%s": to start from SteamOS (Game Mode, menu): %s' % (box, host),
                    'distrobox "%s": запуск из SteamOS (игровой режим, меню): %s' % (box, host)), 'ok')
    problems = verify_layout(game_dir)
    if problems:
        raise InstallError(L('The game folder is incomplete: ', 'Папка игры неполная: ') + '; '.join(problems))
    write_log(L('checked: program, data pack with game.toml, game executable, disc image',
                'проверено: программа, пакет данных с game.toml, исполняемый файл игры, образ'), 'ok')


def read_pak(path):
    """{name: bytes} of a dcme_data.pak (mod/tools/pack_data.py format); InstallError when it is not one."""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 12 or data[:8] != b'DCMEPAK1':
        raise InstallError(L('%s is not a data pack' % path, '%s - не пакет данных' % path))
    count = struct.unpack_from('<I', data, 8)[0]
    at, out = 12, {}
    for _ in range(count):
        nl = struct.unpack_from('<H', data, at)[0]
        name = data[at + 2:at + 2 + nl].decode('utf-8')
        off, size = struct.unpack_from('<II', data, at + 2 + nl + 1)
        out[name] = data[off:off + size]
        at += 2 + nl + 1 + 8
    return out


def verify_layout(game_dir):
    """18.11.2026 (Steam Deck: "the own kernel needs exe in game.toml"): what the game reads at start is checked here,
    where setup can still say what is wrong. The game works in data/ and finds the pack next to its executable
    (dc_pak.c); game.toml inside the pack names the game executable (relative to data/) and the disc image."""
    problems = []
    exe = os.path.join(game_dir, 'DinoCrisisME')
    pak = os.path.join(game_dir, 'dcme_data.pak')
    data = os.path.join(game_dir, 'data')
    if not os.access(exe, os.X_OK):
        problems.append(L('no executable %s' % exe, 'нет программы %s' % exe))
    if not os.path.isfile(pak):
        problems.append(L('no data pack next to the program: %s' % pak, 'нет пакета данных рядом с программой: %s' % pak))
    else:
        entries = read_pak(pak)
        for need in ('game.toml', 'dc_env.cfg'):
            if need not in entries:
                problems.append(L('%s is not in %s' % (need, pak), '%s нет в %s' % (need, pak)))
        toml = entries.get('game.toml', b'').decode('utf-8', 'replace')

        def field(key):
            m = re.search(r'^\s*%s\s*=\s*"([^"]*)"' % key, toml, re.M)
            return m.group(1) if m else ''
        exe_field, disc_field = field('exe'), field('disc')
        if 'game.toml' in entries and not exe_field:
            problems.append(L('game.toml in the pack has no exe', 'в game.toml пакета нет exe'))
        if exe_field and not os.path.isfile(os.path.join(data, exe_field)):
            problems.append(L('the game executable %s is not in %s' % (exe_field, data),
                              'исполняемого файла игры %s нет в %s' % (exe_field, data)))
        if 'game.toml' in entries and not disc_field:
            problems.append(L('game.toml in the pack has no disc', 'в game.toml пакета нет disc'))
        elif disc_field and not os.path.isfile(os.path.join(data, disc_field)):
            problems.append(L('the disc image %s is not where game.toml points' % disc_field,
                              'образа %s нет там, куда указывает game.toml' % disc_field))
    for name in ('dc_user.cfg', 'input.ini'):
        if not os.path.isfile(os.path.join(data, name)):
            problems.append(L('no %s in %s' % (name, data), 'нет %s в %s' % (name, data)))
    return problems


def step_shortcut(game_dir):
    W.start_step(L('Menu entry', 'Пункт меню'))
    if W.O.no_shortcut:
        write_log(L('-NoShortcut: skipped', '-NoShortcut: пропущено'), 'ok')
        return
    apps = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'applications')
    try:
        os.makedirs(apps, exist_ok=True)
        desk = os.path.join(apps, 'dinocrisis-me.desktop')

        def q(p):                                     # the Exec quoting of the desktop entry spec
            return '"' + p.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
        icon = os.path.join(game_dir, 'DinoCrisisME.png')
        # inside a distrobox the menu of the host starts it: through the container (DinoCrisisME-host.sh)
        start = 'DinoCrisisME-host.sh' if distrobox_name() else 'DinoCrisisME.sh'
        text = ('[Desktop Entry]\nType=Application\nName=Dino Crisis ME\nComment=Dino Crisis - Modern Edition\n'
                'Exec=%s\nPath=%s\nTerminal=false\nCategories=Game;\n' % (q(os.path.join(game_dir, start)), game_dir))
        if os.path.isfile(icon):
            text += 'Icon=%s\n' % icon
        with open(desk, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        os.chmod(desk, 0o755)
        write_log(L('menu entry:', 'пункт меню:') + ' ' + desk, 'ok')
    except OSError as e:
        write_log(L('menu entry not created:', 'пункт меню не создан:') + ' %s' % e, 'warn')


def main(argv=None):
    W._setup_output()
    try:
        W.parse_args(sys.argv[1:] if argv is None else argv)
    except W.UsageError as e:
        write_log(str(e), 'err')
        return 1
    W.S.ru = W.O.lang == 'ru'
    os.environ['PYTHONIOENCODING'] = 'utf-8'
    try:
        write_log('==== Dino Crisis ME - ' + L('native Linux setup', 'установка для Linux') + ' ====', 'step')
        write_log(L('disc image :', 'образ игры :') + ' ' + W.O.disc_path)
        write_log(L('install to :', 'куда ставим:') + ' ' + W.O.install_path)
        if not os.path.isfile(W.O.disc_path):
            raise InstallError(L('Disc image not found: ' + W.O.disc_path, 'Образ игры не найден: ' + W.O.disc_path))
        if os.path.getsize(W.O.disc_path) < 100 * 1024 * 1024:
            raise InstallError(L('%s is too small for a disc image. Point at the Track 1 .bin, not the .cue.' % W.O.disc_path,
                                 'Файл %s слишком мал для образа диска. Нужен .bin дорожки 1, а не .cue.' % W.O.disc_path))
        if not os.path.exists(os.path.join(W.MOD_DIR, 'patches', 'psxrecomp-dcme.dcpatch')):
            raise InstallError(L('No mod folder next to the installer - unpack the whole archive.',
                                 'Рядом с установщиком нет папки mod - распакуй архив целиком.'))
        tools = test_prerequisites()
        W.S.version = W.find_game_version(tools)
        if W.O.dry_run:
            write_log(L('-DryRun: checks passed, nothing was installed.',
                        'Ключ -DryRun: проверки пройдены, установка не выполнялась.'), 'ok')
            return 0
        src_root = os.path.join(W.O.install_path, '.build-src')
        game_dir = W.O.install_path
        os.makedirs(src_root, exist_ok=True)

        fw = W.step_framework(src_root)
        W.step_apply_mod(tools, fw)
        proj = W.step_project(src_root)
        W.step_extract_exe(tools, proj)
        full = step_recompile(tools, fw, proj)
        W.step_patch_generated(tools, proj, full)
        W.step_rooms(tools, proj)
        W.step_pack_data(tools, proj)
        exe = step_build_runtime(tools, fw, proj)
        step_layout(game_dir, exe, proj)
        step_shortcut(game_dir)

        if not W.O.keep_build_tree:
            write_log(L('removing the build tree...', 'убираю дерево сборки...'))
            before = W.tree_size(src_root)
            W.remove_tree(src_root)
            write_log(L('freed', 'освобождено') + ' ' + W.n0(W.round_half_up(before / 1048576)) + ' ' + L('MB', 'МБ'), 'ok')
        write_log('==== ' + L('Done', 'Готово') + ' ====', 'step')
        write_log(L('Run:', 'Запуск:') + ' ' + os.path.join(game_dir, 'DinoCrisisME.sh'), 'ok')
        if distrobox_name():
            write_log(L('From SteamOS / Steam (Add a Non-Steam Game):', 'Из SteamOS / Steam (добавить стороннюю игру):')
                      + ' ' + os.path.join(game_dir, 'DinoCrisisME-host.sh'), 'ok')
        return 0
    except InstallError as e:
        write_log(str(e), 'err')
        return 1
    except KeyboardInterrupt:
        write_log(L('Interrupted.', 'Прервано.'), 'err')
        return 1


if __name__ == '__main__':
    sys.exit(main())
