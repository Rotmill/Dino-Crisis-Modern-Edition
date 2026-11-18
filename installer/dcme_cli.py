# -*- coding: utf-8 -*-
"""Terminal output of the Linux setup (18.11.2026): a CLI, nothing graphical.

On a terminal the steps are numbered and aligned, the running step has a spinner, a progress bar (the build's own
[n/m] when the tool prints one, else the steps done) and the last line of the tool it runs, all redrawn in place; a
finished step leaves one line with its time and a short summary. Warnings and errors stay on screen, an error with the
last lines of the failing tool and the path of the full log.

Without a terminal (a pipe, a file, a CI log), with TERM=dumb or with --verbose the output is plain lines, the same as
dcme_install.py writes. Every message and every line of every tool goes to the technical log in all modes.

Only the standard library: ANSI escapes, no curses, no GUI toolkit. NO_COLOR turns colours off.
"""
import os
import re
import shutil
import sys
import threading
import time

_ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
_NINJA = re.compile(r'^\[(\d+)/(\d+)\]')          # ninja: [12/50] Building C object ...
_MAKE = re.compile(r'^\[\s*(\d+)%\]')              # make:  [ 42%] Building C object ...


def visible_len(s):
    return len(_ANSI.sub('', s))


def clip(s, width):
    """s cut to width visible characters (escapes kept), with an ellipsis when cut."""
    if width <= 0:
        return ''
    if visible_len(s) <= width:
        return s
    out, n, i = [], 0, 0
    while i < len(s) and n < width - 1:
        m = _ANSI.match(s, i)
        if m:
            out.append(m.group(0)); i = m.end(); continue
        out.append(s[i]); n += 1; i += 1
    return ''.join(out) + '…' + '\x1b[0m'


def fmt_time(sec):
    sec = int(sec + 0.5)
    return '%d:%02d' % (sec // 60, sec % 60) if sec >= 60 else '%ds' % sec


class Terminal:
    """The output of one setup run. mode: 'live' (redrawn terminal lines) or 'plain' (one line per message)."""

    SPIN_UTF = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
    SPIN_ASCII = '|/-\\'

    def __init__(self, stream=None, total_steps=0, verbose=False, log_path=None, force_mode=None, width=None):
        self.out = stream or sys.stdout
        self.total = total_steps
        self.verbose = verbose
        enc = (getattr(self.out, 'encoding', '') or '').lower()
        self.utf = 'utf' in enc
        tty = hasattr(self.out, 'isatty') and self.out.isatty()
        dumb = os.environ.get('TERM', '') in ('', 'dumb')
        self.mode = force_mode or ('live' if tty and not dumb and not verbose else 'plain')
        self.color = self.mode == 'live' or (tty and not dumb)
        if 'NO_COLOR' in os.environ:
            self.color = False
        self._width = width
        self.lock = threading.RLock()
        self.log = None
        self.log_path = log_path
        if log_path:
            os.makedirs(os.path.dirname(log_path), exist_ok=True)
            self.log = open(log_path, 'a', encoding='utf-8', buffering=1)
        self.t0 = time.monotonic()
        self.step_no = 0
        self.step_title = ''
        self.step_t0 = 0.0
        self.step_warn = 0
        self.step_warns = []
        self.step_summary = ''
        self.steps = []                       # (no, title, status, seconds, summary)
        self.sub = None                       # (done, total) of the running tool, None - unknown
        self.activity = ''                    # last line of the running tool
        self.live_lines = 0
        self.spin_i = 0
        self._stop = threading.Event()
        self._thread = None
        self.errors = []
        self.tool_tail = []

    # ------------------------------------------------------------------ basics
    def width(self):
        return self._width or max(40, shutil.get_terminal_size((100, 24)).columns)

    def c(self, code, s):
        return '\x1b[%sm%s\x1b[0m' % (code, s) if self.color else s

    def logline(self, text):
        if self.log:
            self.log.write('%s %s\n' % (time.strftime('%H:%M:%S'), _ANSI.sub('', text)))

    def _write(self, s):
        try:
            self.out.write(s)
            self.out.flush()
        except (OSError, ValueError):
            pass

    def bar(self, frac, cells=12):
        frac = 0.0 if frac < 0 else 1.0 if frac > 1 else frac
        full = int(round(frac * cells))
        a, b = ('█', '░') if self.utf else ('#', '-')
        return '[' + self.c('36', a * full) + self.c('2', b * (cells - full)) + ']'

    # ------------------------------------------------------------------ live region
    def _clear_live(self):
        if self.mode != 'live' or not self.live_lines:
            return
        s = '\r\x1b[2K'
        for _ in range(self.live_lines - 1):
            s += '\x1b[1A\x1b[2K'
        self._write(s)
        self.live_lines = 0

    def _draw_live(self):
        if self.mode != 'live' or not self.step_no:
            return
        w = self.width() - 1
        spin = (self.SPIN_UTF if self.utf else self.SPIN_ASCII)
        frame = spin[self.spin_i % len(spin)]
        num = '%2d/%d' % (self.step_no, self.total)
        el = fmt_time(time.monotonic() - self.step_t0)
        if self.sub and self.sub[1] > 0:
            frac = self.sub[0] / self.sub[1]
            prog = '%s %3d%%' % (self.bar(frac), int(frac * 100))
        else:
            prog = ''
        line1 = ' %s       %s  %s  %s %s' % (self.c('36', frame), self.c('1', num), self.step_title,
                                               prog, self.c('2', el))        # the column of [OK] / [WARN]
        done = sum(1 for s in self.steps if s[2] != 'fail')
        frac_all = done / self.total if self.total else 0
        tail = (self.c('2', '· ' + self.activity) if self.activity else '')
        line2 = '               %s %s %3d%%  %s  %s' % (self.c('2', 'total'), self.bar(frac_all, 20), int(frac_all * 100),
                                             self.c('2', fmt_time(time.monotonic() - self.t0)), tail)
        self._write('\x1b[?25l' + clip(line1, w) + '\n' + clip(line2, w))
        self.live_lines = 2

    def _redraw(self):
        with self.lock:
            self._clear_live()
            self._draw_live()

    def _spinner(self):
        while not self._stop.wait(0.1):
            with self.lock:
                self.spin_i += 1
                self._clear_live()
                self._draw_live()

    def print_line(self, s):
        """A permanent line above the live region."""
        with self.lock:
            self._clear_live()
            self._write(s + '\n')
            self._draw_live()

    # ------------------------------------------------------------------ the setup's calls
    def header(self, title, rows):
        self.logline('==== ' + title + ' ====')
        for k, v in rows:
            self.logline('%s: %s' % (k, v))
        if self.mode == 'plain':
            self._write('== %s\n' % title)
            for k, v in rows:
                self._write('   %-11s %s\n' % (k, v))
            return
        self._write('\n ' + self.c('1', title) + '\n')
        for k, v in rows:
            self._write('   %s %s\n' % (self.c('2', '%-9s' % k), v))
        self._write('\n')
        if not self._thread:
            self._thread = threading.Thread(target=self._spinner, daemon=True)
            self._thread.start()

    def _finish_step(self, status):
        if not self.step_no:
            return
        sec = time.monotonic() - self.step_t0
        if status == 'ok' and self.step_warn:
            status = 'warn'
        self.steps.append((self.step_no, self.step_title, status, sec, self.step_summary))
        self.logline('-- step %d %s (%.1f s)' % (self.step_no, status, sec))
        if self.mode == 'live':
            tag = {'ok': self.c('32', '[OK]   '), 'warn': self.c('33', '[WARN] '), 'fail': self.c('31;1', '[ERROR]')}[status]
            num = '%2d/%d' % (self.step_no, self.total)
            w = self.width() - 1
            left = ' %s %s  %s' % (tag, self.c('1', num), self.step_title)
            right = self.c('2', '%6s' % fmt_time(sec))
            pad = max(1, min(w, 72) - visible_len(left) - visible_len(right))
            self.print_line(clip(left + ' ' * pad + right, w))
            if self.step_summary:
                self.print_line(clip('               ' + self.c('2', self.step_summary), w))
            for t in self.step_warns:
                self._print_warn(t)
        self.step_warns = []
        self.step_no = 0
        self.sub = None
        self.activity = ''

    def step(self, no, total, title):
        with self.lock:
            self._finish_step('ok')
            self.total = total
            self.step_no, self.step_title = no, title
            self.step_t0 = time.monotonic()
            self.step_warn = 0
            self.step_warns = []
            self.step_summary = ''
            self.logline('== Step %d of %d: %s' % (no, total, title))
            if self.mode == 'plain':
                self._write('== Step %d of %d: %s\n' % (no, total, title))
            else:
                self._redraw()

    def message(self, text, level='info'):
        self.logline('[%s] %s' % (level, text))
        if self.mode == 'plain':
            prefix = {'step': '== ', 'ok': '   [OK] ', 'warn': '   [WARN] ', 'err': '   [ERROR] ', 'info': '   '}[level]
            colour = {'ok': '32', 'warn': '33', 'err': '31;1', 'step': '1'}.get(level)
            line = prefix + text
            self._write((self.c(colour, line) if colour else line) + '\n')
            return
        if level == 'ok':
            self.step_summary = text                       # the last result of a step is its summary
            self._redraw()
        elif level == 'warn':
            self.step_warn += 1
            if self.step_no:
                self.step_warns.append(text)               # printed under the step's line when it ends
                self._redraw()
            else:
                self._print_warn(text)
        elif level == 'err':
            self.errors.append(text)
        elif level == 'step':
            pass                                           # header/footer lines: drawn by header() / done()
        else:
            if text.startswith('> '):
                self.activity = os.path.basename(text[2:].split(' ', 1)[0]) + ' ' + \
                    (text[2:].split(' ', 1)[1] if ' ' in text[2:] else '')
            else:
                self.activity = text
            self._redraw()

    def _print_warn(self, text):
        w = self.width() - 1
        for i, part in enumerate(text.split('\n')):
            self.print_line(clip(('               ' + self.c('33', '[WARN]') + ' ' if i == 0 else '                      ')
                                 + part.strip(), w))

    def summary(self, text):
        """The line shown under the finished step (instead of its last result)."""
        self.step_summary = text
        self.logline('[summary] ' + text)

    def tool_line(self, line, show=True):
        """One line of a running tool: always into the log; on a terminal the activity line, in plain mode the line
        itself when the caller shows the tool's output (or with --verbose)."""
        self.logline('  | ' + line)
        m = _NINJA.match(line)
        if m:
            self.sub = (int(m.group(1)), int(m.group(2)))
        else:
            m = _MAKE.match(line)
            if m:
                self.sub = (int(m.group(1)), 100)
        if self.mode == 'plain':
            if self.verbose or show:
                self._write('     ' + self.c('2', line) + '\n')
            return
        self.activity = line.strip()
        self._redraw()

    def progress(self, done, total, label=''):
        self.sub = (done, total) if total else None
        if label:
            self.activity = label
        if self.mode == 'live':
            self._redraw()

    def fail(self, text, tool_tail=()):
        with self.lock:
            self.logline('[ERROR] ' + text)
            if self.mode == 'plain':
                self._write(self.c('31;1', '   [ERROR] ' + text) + '\n')
                for t in tool_tail:
                    self._write('     ' + t + '\n')
                if self.log_path:
                    self._write('   log: %s\n' % self.log_path)
                return
            self._finish_step('fail')
            self._stop_live()
            w = self.width() - 1
            bar = self.c('31', '─' * min(w, 72) if self.utf else '-' * min(w, 72))
            self._write('\n' + bar + '\n')
            self._write(' ' + self.c('31;1', '[ERROR]') + ' ' + self.c('1', text) + '\n')
            if tool_tail:
                self._write('\n ' + self.c('2', 'last lines of the tool:') + '\n')
                for t in tool_tail:
                    self._write('   ' + self.c('2', clip(t, w - 4)) + '\n')
            if self.log_path:
                self._write('\n ' + self.c('2', 'full log:') + ' ' + self.log_path + '\n')
            self._write(bar + '\n')

    def done(self, title, rows):
        with self.lock:
            self._finish_step('ok')
            total = time.monotonic() - self.t0
            self.logline('==== %s (%.1f s) ====' % (title, total))
            for k, v in rows:
                self.logline('%s: %s' % (k, v))
            if self.mode == 'plain':
                self._write('== %s\n' % title)
                for k, v in rows:
                    self._write('   [OK] %-9s %s\n' % (k, v))
                return
            self._stop_live()
            warns = sum(1 for s in self.steps if s[2] == 'warn')
            note = (self.c('33', '  %d warning(s)' % warns) if warns else '')
            self._write('\n ' + self.c('32;1', '✔ ' if self.utf else '') + self.c('1', title) +
                        self.c('2', '  in %s' % fmt_time(total)) + note + '\n')
            for k, v in rows:
                self._write('   %s %s\n' % (self.c('2', '%-9s' % k), v))
            self._write('\n')

    def _stop_live(self):
        self._stop.set()
        self._clear_live()
        self._write('\x1b[?25h' if self.mode == 'live' else '')

    def close(self):
        with self.lock:
            if self.mode == 'live':
                self._stop_live()
        if self.log:
            self.log.close()
            self.log = None
