#!/usr/bin/env python3
"""Keep the positional patches valid after editing the mod runtime or the framework.

The installer builds the game tree in three layers, each checked by sha256 (mod/tools/posdiff.py):

  1. psxrecomp at the pinned commit + mod/patches/psxrecomp-dcme.dcpatch      (framework files)
  2. mod/src/runtime + mod/src/include copied over it                          (the base tree, USA v1.1)
  3. mod/versions/<v>/runtime.dcpatch on top of that, for every other disc    (expects layer 2 byte for byte)

So any edit of a runtime file breaks layer 3 for each version that patches the same file, and an edit of a framework
file is not shipped at all unless layer 1 is regenerated. This tool does both:

  repatch.py <pristine psxrecomp dir> [--framework-tree DIR] [--git-ref HEAD] [--check]

  * old base  = pristine + layer 1 at <git-ref> + mod/src at <git-ref>
  * new base  = framework files from --framework-tree (a tree prepared like the installer does, then edited; default:
                the old base) + mod/src of the working tree
  * layer 1   = make(pristine -> new base) over every framework file that differs from pristine (only when one changed)
  * layer 3   = for each version: old version tree = old base + its patch; every file it touches is merged three-way
                (git merge-file: new base <- old base -> old version); the patch is made again from the new base.

A conflict stops the tool and leaves the merged file with markers in the work folder, nothing is written.
--check only reports which patches would change.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
MOD = ROOT / "mod"
POSDIFF = MOD / "tools" / "posdiff.py"
FW_PATCH = MOD / "patches" / "psxrecomp-dcme.dcpatch"

sys.path.insert(0, str(POSDIFF.parent))
import posdiff  # noqa: E402


def patch_files(patch: pathlib.Path) -> list[str]:
    return [f["rel"] for f in posdiff.parse(str(patch))]


def apply(patch: pathlib.Path, root: pathlib.Path) -> None:
    files = posdiff.parse(str(patch))
    for f in files:
        p = root / f["rel"]
        data = p.read_bytes()
        if posdiff.sha(data) != f["sha"]:
            raise SystemExit("repatch: %s: %s does not match the original the patch expects" % (patch, f["rel"]))
        p.write_bytes(posdiff.apply_file(f, data))


def git_tree(ref: str, sub: str, dest: pathlib.Path) -> None:
    """Export <ref>:<sub> into dest (byte-exact, no CRLF conversion)."""
    out = subprocess.run(["git", "-C", str(ROOT), "archive", "--format=tar", ref, sub],
                         check=True, capture_output=True).stdout
    tmp = dest.parent / ("_tar_" + dest.name)
    tmp.mkdir(parents=True, exist_ok=True)
    subprocess.run(["tar", "-x", "-C", str(tmp)], input=out, check=True)
    src = tmp / sub
    for p in src.rglob("*"):
        if p.is_file():
            t = dest / p.relative_to(src)
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, t)
    shutil.rmtree(tmp)


def git_file(ref: str, rel: str) -> bytes:
    return subprocess.run(["git", "-C", str(ROOT), "show", "%s:%s" % (ref, rel)], check=True,
                          capture_output=True).stdout


def copy_dir(src: pathlib.Path, dest: pathlib.Path) -> None:
    for p in src.rglob("*"):
        if p.is_file():
            t = dest / p.relative_to(src)
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, t)


def merge3(ours: bytes, base: bytes, theirs: bytes, work: pathlib.Path, name: str) -> bytes:
    if ours == base:
        return theirs
    if theirs == base:
        return ours
    a, o, b = work / (name + ".ours"), work / (name + ".base"), work / (name + ".theirs")
    a.write_bytes(ours); o.write_bytes(base); b.write_bytes(theirs)
    r = subprocess.run(["git", "merge-file", "-p", str(a), str(o), str(b)], capture_output=True)
    if r.returncode != 0:
        bad = work / (name + ".conflict")
        bad.write_bytes(r.stdout)
        raise SystemExit("repatch: merge conflict in %s - see %s" % (name, bad))
    return r.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pristine", type=pathlib.Path, help="psxrecomp at the pinned commit, untouched")
    ap.add_argument("--framework-tree", type=pathlib.Path, help="prepared framework tree with framework edits")
    ap.add_argument("--git-ref", default="HEAD", help="revision the current patches were made for")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep the work folder")
    args = ap.parse_args()

    work = pathlib.Path(tempfile.mkdtemp(prefix="dcme_repatch_"))
    old, new = work / "old", work / "new"
    shutil.copytree(args.pristine, old, symlinks=True)
    shutil.copytree(args.pristine, new, symlinks=True)

    # layer 1 + 2, old
    old_fw_patch = work / "old_fw.dcpatch"
    old_fw_patch.write_bytes(git_file(args.git_ref, "mod/patches/psxrecomp-dcme.dcpatch"))
    apply(old_fw_patch, old)
    git_tree(args.git_ref, "mod/src/runtime", old / "runtime" / "src")
    git_tree(args.git_ref, "mod/src/include", old / "runtime" / "include")

    # layer 1 + 2, new: framework files from the edited tree (the project never stores framework text, only patches)
    mod_rels = {"runtime/src/" + p.name for p in (MOD / "src" / "runtime").iterdir() if p.is_file()}
    mod_rels |= {"runtime/include/" + p.name for p in (MOD / "src" / "include").iterdir() if p.is_file()}
    for sub, dest in (("mod/src/runtime", "runtime/src/"), ("mod/src/include", "runtime/include/")):
        names = subprocess.run(["git", "-C", str(ROOT), "ls-tree", "--name-only", args.git_ref, sub + "/"],
                               check=True, capture_output=True, text=True).stdout.split()
        mod_rels |= {dest + n.rsplit("/", 1)[-1] for n in names}
    old_fw_rels = patch_files(old_fw_patch)
    fw_rels = list(old_fw_rels)
    apply(old_fw_patch, new)
    fw_changed = False
    if args.framework_tree:
        for sub in ("runtime", "recompiler/src"):
            for p in sorted((args.framework_tree / sub).rglob("*")):
                if not p.is_file():
                    continue
                rel = p.relative_to(args.framework_tree).as_posix()
                if rel in mod_rels or rel.startswith("runtime/deps/") or not (args.pristine / rel).is_file():
                    continue
                data = p.read_bytes()
                if data != (new / rel).read_bytes():
                    (new / rel).write_bytes(data)
                    fw_changed = True
                    if rel not in fw_rels:
                        fw_rels.append(rel)
    copy_dir(MOD / "src" / "runtime", new / "runtime" / "src")
    copy_dir(MOD / "src" / "include", new / "runtime" / "include")

    changed = []
    if fw_changed:
        new_fw_patch = work / "fw.dcpatch"
        posdiff.make(str(new_fw_patch), str(args.pristine), str(new), fw_rels)
        if new_fw_patch.read_bytes() != FW_PATCH.read_bytes():
            changed.append((new_fw_patch, FW_PATCH))

    for vdir in sorted((MOD / "versions").iterdir()):
        vp = vdir / "runtime.dcpatch"
        if not vp.is_file():
            continue
        rels = patch_files(vp)
        if all(posdiff.sha((new / f["rel"]).read_bytes()) == f["sha"] for f in posdiff.parse(str(vp))):
            continue                                   # already made for the new base (an earlier run of this tool)
        if all((new / r).read_bytes() == (old / r).read_bytes() for r in rels):
            continue                                   # nothing under this version changed: its patch stays as is
        old_v = work / ("v_" + vdir.name)
        merged = work / ("m_" + vdir.name)
        for rel in rels:
            (old_v / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(old / rel, old_v / rel)
        apply(vp, old_v)
        for rel in rels:
            (merged / rel).parent.mkdir(parents=True, exist_ok=True)
            (merged / rel).write_bytes(merge3((new / rel).read_bytes(), (old / rel).read_bytes(),
                                              (old_v / rel).read_bytes(), work, vdir.name + "_" + rel.replace("/", "_")))
        out = work / (vdir.name + ".dcpatch")
        posdiff.make(str(out), str(new), str(merged), rels)
        # proof: the new patch on the new base gives exactly the merged tree
        proof = work / ("p_" + vdir.name)
        for rel in rels:
            (proof / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(new / rel, proof / rel)
        apply(out, proof)
        for rel in rels:
            if (proof / rel).read_bytes() != (merged / rel).read_bytes():
                raise SystemExit("repatch: %s: %s does not round-trip" % (vdir.name, rel))
        if out.read_bytes() != vp.read_bytes():
            changed.append((out, vp))

    for src, dst in changed:
        print("%s %s" % ("would update" if args.check else "updated", dst.relative_to(ROOT)))
        if not args.check:
            shutil.copyfile(src, dst)
    if not changed:
        print("repatch: all patches already match the working tree")
    if args.keep:
        print("work folder: %s" % work)
    else:
        shutil.rmtree(work)
    return 1 if (args.check and changed) else 0


if __name__ == "__main__":
    raise SystemExit(main())
