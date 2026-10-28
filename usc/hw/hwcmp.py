"""Draw each compiled shader on the EA3 twice, with the DDK's own USP and
with sgxc's (uspsub.so), and compare the frames pixel by pixel.

    python3 -I usc/hw/hwcmp.py OUTDIR [-k name] [--stage frag|vert|both] [--dry]

OUTDIR is a run_corpus.py output (NNN-name.usp + work/name.vert/.frag). The
box needs /opt/sgx-i386 (the DDK, as for re/probe); glt and uspsub.so are
built into usc/build (see glt.c / uspsub.c). Results go to OUTDIR/hw/:
ddk/<name>.rgba, sub/<name>.rgba, ddk_usp/<tag>.usp (the DDK's binaries),
and results.txt.

Environment: EA3 (ssh helper script, default the main session's bin/ea3).
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")
BOX = "/tmp/usc"
W = H = 64

UNIFORMS = {
    "u": [0.7, 0.3, 1.6, 0.45],
    "v": [0.2, 0.9, 0.5, 1.3],
    "w": [0.25, 0.6, 0.8, 0.1],
    "k": [0.1, -0.2, 0.3, 0.05],
    "f": [0.35],
    "n": [3],
}
MAT = [0.9, 0.1, 0.0, 0.0, -0.1, 0.8, 0.05, 0.0, 0.0, 0.0, 1.0, 0.0, 0.05, -0.02, 0.0, 1.0]
A1 = [0.1, 0.2, 0.3, 0.4, 0.9, 0.25, 0.5, 0.6, 0.15, 0.85, 0.7, 0.2, 0.95, 0.9, 0.05, 1.0]
A2 = [0.6, 0.1, 0.9, 0.3, 0.2, 0.7, 0.4, 0.8, 0.5, 0.5, 0.1, 0.9, 0.3, 0.95, 0.6, 0.15]


def env_ea3():
    return os.environ.get("EA3", "/private/tmp/claude-501/-Users-josephtelaak-Documents-GitHub-openHC/"
                                 "8d4d6471-366e-445e-a0ae-48c496d608d4/scratchpad/bin/ea3")


def sh(cmd, inp=None, check=True):
    r = subprocess.run([env_ea3(), cmd], input=inp, capture_output=True)
    if check and r.returncode:
        sys.exit(f"hwcmp: box command failed ({r.returncode}): {cmd}\n{r.stderr.decode(errors='replace')}")
    return r


def put(local, remote):
    with open(local, "rb") as f:
        sh(f"cat > {remote}", inp=f.read())


def test_block(name, vs, fs, extra):
    src = open(vs).read() + open(fs).read()
    lines = [f"test {name}", f"vs {BOX}/src/{name}.vert", f"fs {BOX}/src/{name}.frag"]
    for m in re.finditer(r"uniform\s+(?:\w+\s+)?(\w+)\s+(\w+)\s*(\[[^\]]*\])?\s*;", src):
        ty, un = m.group(1), m.group(2)
        if ty.startswith("sampler"):
            lines.append(f"i {un} 0")
        elif ty == "mat4":
            lines.append(f"m {un} " + " ".join(map(str, MAT)))
        elif ty in ("int", "bool"):
            lines.append(f"i {un} {UNIFORMS.get(un, [1])[0]}")
        elif un in UNIFORMS:
            n = {"float": 1, "vec2": 2, "vec3": 3, "vec4": 4}.get(ty, 4)
            lines.append(f"u {un} " + " ".join(map(str, UNIFORMS[un][:n])))
    for m in re.finditer(r"attribute\s+(?:\w+\s+)?\w+\s+(\w+)\s*;", src):
        a = m.group(1)
        if a == "p":
            continue
        loc = 1 + len([x for x in lines if x.startswith("attr ")])
        lines.append(f"attr {a} {loc} " + " ".join(map(str, A1 if loc == 1 else A2)))
    if "sampler2D" in src:
        lines.append("tex 0 grad")
    lines += extra
    lines.append("end")
    return lines


def compare(a, b):
    da, db = open(a, "rb").read(), open(b, "rb").read()
    diff = maxd = 0
    for i in range(0, len(da), 4):
        if da[i:i + 4] != db[i:i + 4]:
            diff += 1
            maxd = max(maxd, max(abs(x - y) for x, y in zip(da[i:i + 4], db[i:i + 4])))
    return diff, maxd, len(da) // 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("-k")
    ap.add_argument("--stage", default="auto")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    hw = os.path.join(a.outdir, "hw")
    stage_dir = os.path.join(hw, "stage")
    shutil.rmtree(stage_dir, ignore_errors=True)
    os.makedirs(os.path.join(stage_dir, "src"))
    os.makedirs(os.path.join(stage_dir, "sub"))
    lines, names = [], []
    for usp in sorted(glob.glob(os.path.join(a.outdir, "*.usp"))):
        name = os.path.basename(usp)[4:-4]
        if a.k and a.k not in name:
            continue
        vs = os.path.join(a.outdir, "work", name + ".vert")
        fs = os.path.join(a.outdir, "work", name + ".frag")
        if not (os.path.exists(vs) and os.path.exists(fs)):
            continue
        kind = "vert" if open(usp, "rb").read()[0x10] == 1 else "frag"
        shutil.copy(vs, os.path.join(stage_dir, "src", name + ".vert"))
        shutil.copy(fs, os.path.join(stage_dir, "src", name + ".frag"))
        shutil.copy(usp, os.path.join(stage_dir, "sub", f"{name}.{kind}.usp"))
        # both stages from sgxc when asked (and when it compiled the other one too)
        if a.stage == "both":
            other = "frag" if kind == "vert" else "vert"
            o = os.path.join(a.outdir, "work", f"{name}.{other}.usp")
            if os.path.exists(o):
                shutil.copy(o, os.path.join(stage_dir, "sub", f"{name}.{other}.usp"))
        lines += test_block(name, vs, fs, [])
        names.append(name)
    open(os.path.join(stage_dir, "list"), "w").write("\n".join(lines) + "\n")
    shutil.copy(os.path.join(BUILD, "glt"), stage_dir)
    shutil.copy(os.path.join(BUILD, "uspsub.so"), stage_dir)
    print(f"{len(names)} tests staged in {stage_dir}")
    if a.dry:
        return
    tar = subprocess.run(["tar", "cf", "-", "-C", stage_dir, "."], capture_output=True, check=True).stdout
    sh(f"rm -rf {BOX} && mkdir -p {BOX} && tar xf - -C {BOX}", inp=tar)
    run = (f"cd {BOX} && mkdir -p ddk out_ddk out_sub && "
           f"n=$(dmesg | wc -l); R=/opt/sgx-i386; "
           f"export LD_PRELOAD=\"$R/usr/lib/sgx-busidshim.so {BOX}/uspsub.so\"; "
           f"USPSUB_SAVE={BOX}/ddk $R/lib/ld-linux.so.2 --library-path $R/usr/lib:$R/lib {BOX}/glt list out_ddk > log_ddk 2>&1; "
           f"echo ddk rc=$?; "
           f"USPSUB_DIR={BOX}/sub $R/lib/ld-linux.so.2 --library-path $R/usr/lib:$R/lib {BOX}/glt list out_sub > log_sub 2>&1; "
           f"echo sub rc=$?; "
           f"dmesg | tail -n +$((n+1)) | grep -i -E 'recover|lockup|hwr|timeout' | head -5; "
           f"tar cf res.tar out_ddk out_sub ddk log_ddk log_sub")
    r = sh(run, check=False)
    print(r.stdout.decode(errors="replace").strip())
    res = sh(f"cat {BOX}/res.tar").stdout
    shutil.rmtree(os.path.join(hw, "res"), ignore_errors=True)
    os.makedirs(os.path.join(hw, "res"))
    subprocess.run(["tar", "xf", "-", "-C", os.path.join(hw, "res")], input=res, check=True)
    out = []
    same = 0
    for name in names:
        da = os.path.join(hw, "res", "out_ddk", name + ".rgba")
        db = os.path.join(hw, "res", "out_sub", name + ".rgba")
        if not (os.path.exists(da) and os.path.exists(db)):
            out.append(f"{name:16} MISSING ({'ddk' if not os.path.exists(da) else 'sub'})")
            continue
        diff, maxd, n = compare(da, db)
        same += diff == 0
        px = open(da, "rb").read()
        mid = px[(H // 2 * W + W // 2) * 4:(H // 2 * W + W // 2) * 4 + 4].hex()
        out.append(f"{name:16} {'SAME' if diff == 0 else 'DIFF'} {diff:5}/{n} px differ, max {maxd:3}  centre {mid}")
    out.append(f"{same}/{len(names)} identical")
    open(os.path.join(hw, "results.txt"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
