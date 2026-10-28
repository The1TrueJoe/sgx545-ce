"""Run a microkernel through a captured or generated frame in the emulator and
print what it does: every register access, every device-memory store and the
tasks it starts, event by event.

    python3 tools/ukernel/run.py FIRMWARE TRACE [-o out.txt] [--ours]

FIRMWARE is the DDK microkernel (srvinit/fetch-firmware.sh) or ours
(ukernel/build.sh). TRACE is a pvrtrace log: a DDK capture or tools/gen's
`frame-fake -t`. The host side is played as the kernel driver plays it
(init, one CCB TA command per SGX_DOKICK), and the hardware's answers are
sgxsim.py's model: after each TA kick a TA-finished event, after the last
kick of a scene the 3D end-of-render and DPM 3D-memory-free events.

The output is for reading and for diffing two microkernels against each
other; it is not committed for the DDK's (docs/UKERNEL.md says why).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fromtrace  # noqa: E402
from sgxsim import Machine, PD_PHYS, DDK_ENTRIES  # noqa: E402
from emu import EmuError  # noqa: E402

# what the hardware reports when each unit finishes: the event word the event
# program gets (pds3 in1 = in2) and the EVENT_STATUS bit behind it
EV_TA_FINISHED = (0x200, 0x2000)
EV_3D_END = (0x10, 0x40000)
EV_3D_FREE = (0x400000, 0x1)


class Runner:
    def __init__(self, fw, trace, entries=DDK_ENTRIES, out=sys.stdout, poll_assist=True):
        self.out = out
        self.ctxs = []
        self.events = fromtrace.kicks(trace, self.ctxs)
        self.m = Machine(fw, entries)
        self.ok = True
        self.step("init", self.m.init)
        if poll_assist:
            self.m.auto_polls()
        for va, data, pdo in self.ctxs:
            self.m.mem.load(va, data)
            self.m.mem.wr(va + pdo, PD_PHYS)
        self.m.mem.log = []

    def p(self, s):
        print(s, file=self.out)

    def flush(self, title):
        m = self.m
        self.p(f"== {title}")
        for l in m.log:
            self.p(f"   {l}")
        for op, a, v, pc in m.regs.log:
            self.p(f"   {op} {a:#06x} = {v:#010x}  @{pc:#06x}")
        for op, a, v, pc in m.mem.log or []:
            if op == "st" and pc is not None:
                self.p(f"   mem {a:#010x} = {v:#010x}  @{pc:#06x}")
        m.log.clear()
        m.regs.log.clear()
        if m.mem.log is not None:
            m.mem.log.clear()

    def step(self, title, f, *a):
        self.m.u.trace = []
        self.m.u.trace_max = 40
        try:
            f(*a)
        except (EmuError, RuntimeError) as e:
            self.ok = False
            self.p(f"ERROR {e}")
            for pc, t in self.m.u.trace:
                self.p(f"     {pc:04x} {t}")
        self.m.u.trace = None
        self.flush(title)

    def event(self, title, ev):
        word, status = ev
        self.m.regs.v[0x12C] = self.m.regs.v.get(0x12C, 0) | status
        self.step(title, self.m.event, 0, word, word)

    def shared(self, k):
        """CMDTA_SHARED as SGXDoKickKM fills it (sync objects, status values,
        ready flag). A trace snapshots memory before the ioctl, so kick n's is
        read back from a later snapshot when there is one."""
        cmd = k.ccb_va + k.ccb_off
        later = [e for e in self.events if e.kind == "kick" and e.n > k.n]
        d = later[0].memory.read(cmd, 0x1E0) if later else None
        if d:
            self.m.mem.load(cmd, d)
        self.m.mem.wr(cmd, self.m.mem.rd(cmd) | 1)

    def run(self):
        prev = None
        for k in self.events:
            if k.kind != "kick":
                continue
            if prev is None:
                k.load(self.m.mem)
            else:
                # later kicks: only what the client changed since the last one,
                # so the microkernel's own writes stay
                for (va, d, _), (va2, d2, _) in zip(prev.memory.regions, k.memory.regions):
                    for i in range(0, len(d) - 3, 4):
                        if d[i:i + 4] != d2[i:i + 4]:
                            self.m.mem.wr(va + i, struct.unpack_from("<I", d2, i)[0])
            self.shared(k)
            self.step(f"TA kick {k.n}", self.m.kick, 0, k.data)
            self.event(f"TA finished ({k.n})", EV_TA_FINISHED)
            if k.last:
                self.event("3D end of render", EV_3D_END)
                self.event("DPM 3D memory free", EV_3D_FREE)
            prev = k
        self.p("hostctl " + " ".join(f"{x:x}" for x in self.m.hostctl()))
        return self.ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fw")
    ap.add_argument("trace")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    out = open(a.out, "w") if a.out else sys.stdout
    ok = Runner(open(a.fw, "rb").read(), a.trace, out=out).run()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
