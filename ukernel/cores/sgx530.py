# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Per-core microkernel configuration for the SGX530 (docs/UKERNEL-TOOLING.md §3).

Built from the fact sheet, docs/porting/cores/sgx530.md; each value carries its
tag from there (K: the GPL kernel register headers, D: derived, G: a guess).
Nothing here is confirmed on SGX530 hardware yet (no C tag). Everything this
file does not override is the SGX545's value, and the TA, 3D, DPM, ISP, TSP,
PBE and PDS registers among those are unknown on the SGX530 (the sheet's gap
13): the first-boot stages (docs/porting/microkernel.md) confirm them one at a
time, and the bounded waits below keep a wrong guess from freezing the EDM.
"""
from .sgx545 import *  # noqa: F401,F403  the SGX545 is the base; override what differs

CORE_NAME = "sgx530"
MISC_CORE = 530

# ---- identity (K) -----------------------------------------------------------
CR_CORE_ID = 0x10
CR_CORE_REVISION = 0x14      # expect 0x00010205, rev 125 (D)

# ---- clock gating (K) -------------------------------------------------------
CR_CLKGATESTATUS = 0x004     # 0x008 is CLKGATECTLOVR here
CLKGATE_ISP_MASK = 0x30      # CLKGATECTL's ISP field is b4-5 (2D takes b0-1)
CLKGATE_ISP_AUTO = 0x20      # field value 2 = on demand, as on the SGX545 (G)
CLKGATESTATUS_ISP = 0x10     # CLKGATESTATUS b4

# ---- reset (K) --------------------------------------------------------------
SOFT_RESET_TA = 0x8          # b3: every unit above the BIF moves up a bit for the
                             # 2D's b1. The microkernel resets only the TA; the 2D
                             # bit is the host's.

# ---- the BIF: one memory context (K) ----------------------------------------
MULTI_MEM_CONTEXTS = False   # only DIR_LIST_BASE0, no BANK registers. A job's PD
                             # goes in list 0 for the whole job, so every requestor,
                             # the EDM included, translates through it: the host must
                             # map the firmware's code and data at the same addresses
                             # in every client PD. raise_host_irq puts the firmware
                             # PD back (SC_KERNEL_PD) when nothing is in flight.
CR_BIF_DIR_LIST_BASE1 = None  # absent; using one is a build error
CR_BIF_BANK0 = None
FIX_HW_BRN_28889 = True      # listed for every SGX530 revision

# ---- events and kicks (K) ---------------------------------------------------
MULTI_EVENT_KICK = False     # FIX_HW_BRN_22934: EVENT_KICK only; the host's init kick
                             # uses it too. No microkernel change (see sgx545.py).
DCU_INVALID = 0              # EVENT_STATUS2 has only b0/b1 (the DPM free-list loads)
# dpm_load's OTPM waits: STATUS b20 is unknown here (kept, bounded); the SGX545's
# STATUS2 b12 doesn't exist. STATUS b21, OTPM loaded, is the SGX530's own and the
# first candidate if b20 times out (G).
OTPM_CLEAR_WAITS = [(CR_EVENT_STATUS, 0x100000, CR_EVENT_HOST_CLEAR)]  # noqa: F405

# ---- bring-up ---------------------------------------------------------------
WAIT_BOUND = 0xffff          # every wait gives up after 65535 polls and logs itself
                             # in WAIT_TIMEOUT_ID/CT (the manifest's "waits" names it)
FEATURES = 0                 # not SECURE: one context means the firmware heaps are in
                             # every client PD
