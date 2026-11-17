"""The terminal output of the Linux setup (installer/dcme_cli.py): live on a TTY, plain lines elsewhere, full log always."""
import io
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "installer"))
import dcme_cli  # noqa: E402

ESC = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")


class TtyStream(io.StringIO):
    encoding = "utf-8"

    def isatty(self):
        return True


class PipeStream(io.StringIO):
    encoding = "utf-8"

    def isatty(self):
        return False


def screen(raw):
    """The final screen of a byte stream with \\r, \\n, ESC[K and ESC[nA - what a terminal would show."""
    lines, row, col, i = [""], 0, 0, 0
    while i < len(raw):
        m = ESC.match(raw, i)
        if m:
            if m.group(2) == "K":
                lines[row] = lines[row][:col]
            elif m.group(2) == "A":
                row = max(0, row - int(m.group(1) or 1))
            i = m.end()
            continue
        ch = raw[i]
        i += 1
        if ch == "\r":
            col = 0
        elif ch == "\n":
            row, col = row + 1, 0
            if row >= len(lines):
                lines.append("")
        else:
            line = lines[row].ljust(col)
            lines[row] = line[:col] + ch + line[col + 1:]
            col += 1
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def run_session(stream, log, mode=None, fail=False):
    os.environ.pop("NO_COLOR", None)
    os.environ["TERM"] = "xterm-256color"
    ui = dcme_cli.Terminal(stream=stream, total_steps=3, log_path=log, force_mode=mode, width=90)
    ui.header("Dino Crisis ME - native Linux setup", [("disc", "/d/disc.bin"), ("target", "/g")])
    ui.step(1, 3, "Checking build tools")
    ui.message("compiler: clang", "ok")
    ui.message("vulkaninfo not found", "warn")
    ui.summary("clang · cmake · ninja")
    ui.step(2, 3, "Building the game")
    for k in range(1, 501):
        ui.tool_line("[%d/500] Building C object CMakeFiles/x.dir/file_%03d.c.o" % (k, k))
        if k == 335:
            sub = ui.sub
    ui.message("DinoCrisisME: 5 MB", "ok")
    if fail:
        ui.step(3, 3, "Laying out the game folder")
        ui.tool_tail = ["ld: cannot find -lvulkan"]
        ui.fail('"cmake" exited with code 1', ui.tool_tail)
    else:
        ui.step(3, 3, "Laying out the game folder")
        ui.message("game folder ready", "ok")
        ui.done("Dino Crisis ME is installed", [("play", "/g/DinoCrisisME.sh")])
    ui.close()
    return sub


def main():
    with tempfile.TemporaryDirectory() as t:
        log = os.path.join(t, "logs", "install.log")

        # live: few lines on screen however much the tools print; aligned step lines; progress parsed
        tty = TtyStream()
        sub = run_session(tty, log)
        raw = tty.getvalue()
        scr = screen(raw)
        assert sub == (335, 500), sub
        assert len(scr) < 20, "live output must stay short, got %d lines:\n%s" % (len(scr), "\n".join(scr))
        text = "\n".join(scr)
        assert "[WARN]   1/3  Checking build tools" in text, text
        assert "[OK]     2/3  Building the game" in text, text
        assert "[OK]     3/3  Laying out the game folder" in text, text
        assert "clang · cmake · ninja" in text and "vulkaninfo not found" in text
        assert text.index("1/3") < text.index("vulkaninfo not found") < text.index("2/3"), "warning under its step"
        assert "Dino Crisis ME is installed" in text and "/g/DinoCrisisME.sh" in text
        assert "Building C object" not in text, "tool lines must not stay on screen"
        assert "█" in raw and "%" in raw, "a progress bar is drawn while the build runs"
        assert "\x1b[?25h" in raw, "the cursor is shown again"
        assert all(len(ESC.sub("", l)) <= 90 for l in scr), "no line wider than the terminal"
        logged = pathlib.Path(log).read_text()
        assert logged.count("Building C object") == 500, "every tool line goes to the log"
        assert "[warn] vulkaninfo not found" in logged

        # live failure: highlighted block with the tool's last lines and the log path
        tty = TtyStream()
        run_session(tty, log, fail=True)
        text = "\n".join(screen(tty.getvalue()))
        assert "[ERROR]  3/3  Laying out the game folder" in text, text
        assert '[ERROR] "cmake" exited with code 1' in text
        assert "ld: cannot find -lvulkan" in text and "full log: " + log in text

        # no TTY: plain lines, no escape codes, every message, the same markers
        pipe = PipeStream()
        run_session(pipe, log)
        out = pipe.getvalue()
        assert "\x1b[" not in out, "no ANSI escapes in a pipe"
        assert "== Step 2 of 3: Building the game" in out
        assert "[OK] compiler: clang" in out and "[WARN] vulkaninfo not found" in out
        assert "\r" not in out

        # NO_COLOR on a TTY: still live, no colours
        os.environ["NO_COLOR"] = "1"
        ui = dcme_cli.Terminal(stream=TtyStream(), total_steps=1, width=80)
        assert ui.mode == "live" and not ui.color
        os.environ.pop("NO_COLOR")
        # TERM=dumb and --verbose: plain
        os.environ["TERM"] = "dumb"
        assert dcme_cli.Terminal(stream=TtyStream(), total_steps=1).mode == "plain"
        os.environ["TERM"] = "xterm"
        assert dcme_cli.Terminal(stream=TtyStream(), total_steps=1, verbose=True).mode == "plain"

        # clip keeps the visible width and closes colours
        c = dcme_cli.clip("\x1b[32m" + "x" * 50 + "\x1b[0m", 10)
        assert dcme_cli.visible_len(c) == 10 and c.endswith("\x1b[0m")

        # the real setup in a pipe: plain output, a clear error, the log written, exit code 1
        fake = os.path.join(t, "fake.bin")
        with open(fake, "wb") as f:
            f.truncate(1024)
        env = dict(os.environ, XDG_STATE_HOME=os.path.join(t, "state"))
        r = subprocess.run([sys.executable, "-I", str(ROOT / "installer" / "dcme_install_linux.py"), "-DiscPath", fake,
                            "-InstallPath", os.path.join(t, "game"), "-DryRun", "--verbose"],
                           capture_output=True, text=True, env=env)
        assert r.returncode == 1, r
        assert "[ERROR]" in r.stdout and "too small for a disc image" in r.stdout and "\x1b[" not in r.stdout, r.stdout
        logs = list(pathlib.Path(t, "state", "dcme").glob("install-*.log"))
        assert logs and "too small" in logs[0].read_text()
    print("linux setup terminal output: ok")


if __name__ == "__main__":
    main()
