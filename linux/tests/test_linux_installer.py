"""The Linux setup refuses a game folder the game cannot start from (installer/dcme_install_linux.py, verify_layout).

18.11.2026 regression: on the Steam Deck the game stopped with "the own kernel needs exe in game.toml" - the pack with
game.toml was not found from data/. A complete layout must pass; each missing piece must be named.
"""
import os
import pathlib
import stat
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "installer"))
import dcme_install_linux as I  # noqa: E402


def write_pak(path, files):
    names = sorted(files)
    head = 12 + sum(2 + len(n.encode()) + 1 + 8 for n in names)
    idx, body, off = b"", b"", head
    for n in names:
        b = files[n]
        idx += struct.pack("<H", len(n.encode())) + n.encode() + b"\0" + struct.pack("<II", off, len(b))
        body += b + b"\0"
        off += len(b) + 1
    path.write_bytes(b"DCMEPAK1" + struct.pack("<I", len(names)) + idx + body)


def layout(root, toml, with_exe=True, with_pak=True, with_slus=True):
    (root / "data").mkdir(parents=True)
    disc = root / "disc.bin"
    disc.write_bytes(b"\0" * 16)
    if with_exe:
        exe = root / "DinoCrisisME"
        exe.write_bytes(b"\x7fELF")
        exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
    if with_pak:
        write_pak(root / "dcme_data.pak", {"game.toml": toml.replace("@DISC@", str(disc)).encode(),
                                           "dc_env.cfg": b"x=1\n"})
    if with_slus:
        (root / "data" / "SLUS_009.22").write_bytes(b"PS-X EXE")
    for n in ("dc_user.cfg", "input.ini"):
        (root / "data" / n).write_text("")


GOOD = '[game]\nname = "Dino Crisis"\nexe  = "SLUS_009.22"\ndisc = "@DISC@"\n'


def check(name, toml, expect, **kw):
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t) / "game"
        layout(root, toml, **kw)
        problems = I.verify_layout(str(root))
        text = "; ".join(problems)
        if expect is None:
            assert not problems, "%s: a complete layout was refused: %s" % (name, text)
        else:
            assert any(expect in p for p in problems), "%s: expected %r among %r" % (name, expect, problems)


def main():
    check("complete", GOOD, None)
    check("no pack", GOOD, "no data pack", with_pak=False)
    check("no exe in game.toml", '[game]\nname = "x"\ndisc = "@DISC@"\n', "has no exe")
    check("no game executable", GOOD, "SLUS_009.22 is not in", with_slus=False)
    check("no program", GOOD, "no executable", with_exe=False)
    check("disc moved", GOOD.replace("@DISC@", "/nowhere/disc.bin"), "disc image")
    # inside a distrobox (SteamOS) the menu entry starts the game through the container
    with tempfile.TemporaryDirectory() as t:
        env = dict(os.environ)
        os.environ.update({"CONTAINER_ID": "dcme", "DISTROBOX_ENTER_PATH": "/usr/bin/distrobox-enter",
                           "XDG_DATA_HOME": t})
        try:
            assert I.distrobox_name() == "dcme"
            I.W.O.no_shortcut = False
            I.step_shortcut("/home/deck/Games/DinoCrisisME")
            desk = pathlib.Path(t, "applications", "dinocrisis-me.desktop").read_text()
            assert 'Exec="/home/deck/Games/DinoCrisisME/DinoCrisisME-host.sh"' in desk, desk
            host = I.HOST_LAUNCHER % ("dcme", "dcme")
            assert 'exec distrobox enter dcme -- "$here/DinoCrisisME.sh"' in host
        finally:
            os.environ.clear()
            os.environ.update(env)
    os.environ.pop("CONTAINER_ID", None)
    assert I.distrobox_name() == "" or os.path.exists("/run/.containerenv") or os.path.exists("/.dockerenv")
    print("linux installer layout check: ok")


if __name__ == "__main__":
    main()
