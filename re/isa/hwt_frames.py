"""hwt.py suite for full-frame dumps (run with --full DIR): the replay with its captured main program
(no test program: the uniform colour over the hwt clear colour), and two tests from p1/p2."""
import hwt_p1
import hwt_p2

TESTS = [("base", [], None)] + [t for t in hwt_p1.TESTS + hwt_p2.TESTS if t[0] in ("fmad_plain", "br_2")]
