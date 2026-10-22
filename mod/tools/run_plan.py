# -*- coding: utf-8 -*-
"""Builds the recompiler or the game from a build plan (10.10.2026): the commands a clean CMake + Ninja build of the mod
runs, recorded once when the package is made (tools\\make_build_plan.py of the package). So the player needs neither
CMake nor Ninja - only the compiler that comes with the mod (Clang, mod\\tools\\llvm).

    python run_plan.py <plan.json> SRC=<.build-src> LLVM=<mod\\tools\\llvm> VULKAN=<mod\\vulkan> [JOBS=<n>]

Compile steps run in parallel (the biggest source first); every other step (archive, resources, link) waits for all
before it, in the recorded order. Stops at the first failure with the command and its output.
Every step is a list of arguments: the folders go into single arguments, and Python quotes them for Windows itself, so a
space or a non-Latin letter in a path (the user's name in it) does not break the command.

21.10.2026 (a player's friend: setup stopped at "Recompiling the game to C" with "python.exe exited with code 1", "python
locks up the PC"): JOBS - how many compilers at a time, setup counts it from the free memory (one per processor
otherwise); the compilers run at below normal priority, so the PC stays usable; compile steps that failed in the
parallel batch are tried once more one at a time (a compiler that ran out of memory next to the others); a compiler
Windows or an antivirus will not start gives a plain line instead of a Python traceback.
"""
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor


def main():
    plan = json.load(open(sys.argv[1], encoding='utf-8'))
    roots = dict(a.split('=', 1) for a in sys.argv[2:])
    jobs = int(roots.pop('JOBS', '0') or 0)
    sub = {('@%s@' % k): os.path.normpath(v).replace('\\', '/') for k, v in roots.items()}

    def fill(s):
        for k, v in sub.items():
            s = s.replace(k, v)
        return s

    build = os.path.join(roots['SRC'], plan['build'])
    os.makedirs(build, exist_ok=True)
    for d in plan['dirs']:
        os.makedirs(os.path.join(build, fill(d)), exist_ok=True)
    for f in plan['files']:
        with open(os.path.join(build, f['name']), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(fill(f['text']))

    steps = plan['steps']
    total, done = len(steps), [0]
    t0 = time.time()
    low = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)
    blocked = [False]                                      # Windows would not start a program - no retry, it would not either

    def run(step):
        if 'rm' in step:                                   # CMake's "-E rm -f" before an archive is made
            for f in step['rm']:
                p = os.path.join(build, fill(f))
                if os.path.exists(p):
                    os.remove(p)
            done[0] += 1
            return True
        argv = [fill(a) for a in step['argv']]
        what = os.path.basename(fill(step['src'])) if step.get('src') else os.path.basename(argv[0].replace('\\', '/'))
        try:
            r = subprocess.run(argv, cwd=build, capture_output=True, text=True, encoding='utf-8', errors='replace',
                               creationflags=low)
        except OSError as e:                               # blocked or removed by Windows security or an antivirus
            done[0] += 1
            blocked[0] = True
            print('FAILED: %s - Windows did not start %s: %s' % (what, os.path.basename(argv[0]), e), flush=True)
            if getattr(e, 'winerror', 0) == 4551:          # the friend's log: Smart App Control / an app control policy
                print('APP CONTROL: Smart App Control or an app control policy of this PC blocks programs built on it',
                      flush=True)
            else:
                print('An antivirus or Windows security may have blocked or removed the compiler that comes with the mod.',
                      flush=True)
            return False
        done[0] += 1
        print('[%d/%d] %s' % (done[0], total, what), flush=True)
        out = (r.stdout + r.stderr).strip()
        if r.returncode != 0:
            print('FAILED (%d): %s' % (r.returncode, subprocess.list2cmdline(argv)), flush=True)
            if out:
                print(out, flush=True)
            return False
        return True

    def size(step):
        try:
            return os.path.getsize(fill(step['src']).replace('/', os.sep)) if step.get('src') else 0
        except OSError:
            return 0

    workers = max(1, jobs if jobs > 0 else (os.cpu_count() or 1))
    print('%d compilers at a time%s' % (workers, '' if jobs > 0 else ' (one per processor)'), flush=True)
    i = 0
    while i < total:
        if steps[i]['compile']:
            j = i
            while j < total and steps[j]['compile']:
                j += 1
            batch = sorted(steps[i:j], key=size, reverse=True)
            with ThreadPoolExecutor(max_workers=workers) as ex:
                res = list(ex.map(run, batch))
            failed = [s for s, ok in zip(batch, res) if not ok]
            if failed and blocked[0]:
                sys.exit(1)
            if failed:
                print('retrying %d failed step(s) one at a time' % len(failed), flush=True)
                done[0] -= len(failed)
                for s in failed:
                    if not run(s):
                        sys.exit(1)
            i = j
        else:
            if not run(steps[i]):
                sys.exit(1)
            i += 1
    exe = os.path.join(build, plan['output'])
    if not os.path.exists(exe):
        sys.exit('no %s after the plan' % exe)
    print('built %s in %.0f s (%d steps, %d at a time)' % (plan['output'], time.time() - t0, total, workers), flush=True)


if __name__ == '__main__':
    main()
