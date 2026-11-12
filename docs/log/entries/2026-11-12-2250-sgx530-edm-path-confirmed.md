# SGX530: EDM-with-host-memory path confirmed — fresh-constant str and loads retire

**Board:** our OMAP3630 test board, up 14 h 06 min (the same boot as the whole §9 series, no
reboot), the owner-reread pinned ED25519 host key unchanged, the board lock held
22:44:32–22:50:27 UTC. 77 → 75 °C. No hang, no plug cycle; clean `rmmod` after both loads.

**What ran:** powervr_sgx from claude/sgx530-gap19-tests at b1689a57 (omap3630 7.3.3 CI green;
x86 golden/checkpatch green), two loads, `allow_unisolated=1 stage1_vlaunch=1`. Both images
bit-identical to the microkernel session's artifacts, rebuilt from committed source
(ukernel/build.py + drm/tools/mkukfw.py, `--core-rev 0x10205`), no DDK inputs:

| Image | source commit | img sha256 | pkg sha256 | crc |
|---|---|---|---|---|
| `sgx530_limmwr_r5_5c3` | 0e69b9df | c72392bf… | 03e1cbd8… | 0x6d713278 |
| `sgx530_ldret` | 7c3ef8f7 | da0f5462… | 4b669b8d… | 0x558044f1 |

**1. limmwr_5c3 — a fresh constant settles str/limm.** `limm r5, #0x5c3; str r5 -> EVENT_TIMER
(0xacc); loop`. The after-kick diagnostic reads EVENT_TIMER (the `timer` field); no driver
change. **`EVENT_TIMER` reads 0x000005c3**, stable 100–500 ms after the kick. Both the
"clocks on" and "before the kick" reads show `timer 0x00000000`, so **init clears EVENT_TIMER
to 0** (SRVINIT part 1) and 0x5c3 is the task's own write.

**2. ldret — loads retire.** `ld.a.d r5, r17(=ctlblk), #0x7f` (reads ctlblk+0x200) `; wdf;
ld.a.d r3, r5, #0; wdf; loop`. The driver (new `stage1_ld_sentinel`, this commit) pre-writes
an unmapped sentinel 0x0dddd000 at ctlblk+0x200 (data arena +0x3200) before the kick. **`BIF_FAULT`
reads 0x0dddd000** (BIF int 0x4002, the fault-req bit): the first `ld` landed the sentinel in
r5, and the fault-read of r5 faulted at exactly that value.

Both loads end with "microkernel did not start … error -110" — expected: a probe task halts in
`ba usse_loop` and never writes `ui32InitStatus`, so the host init wait times out. The result
is in the registers, not the init status.

**Facts (C):**
- **`str`/`limm` work under §9, confirmed with a fresh constant.** 0x5c3 cannot be residual
  (it is nowhere in the driver or a prior run), and init clears EVENT_TIMER to 0 first. This
  removes the residual-value note in log 2026-11-12-2214 (the earlier 0xa5a = the task's write;
  the xorr5 read-through was pre-clear residual, now explained by the confirmed init-clear).
- **Loads retire.** A value loaded from memory lands in a retiring temporary (r5), same as the
  ALU-write temp retire in log 2026-11-12-2149.

**What this settles — the EDM-with-host-memory path is green.** The open microkernel can run as
an EDM task on the SGX530: it computes (temps retire), loads constants (`limm`), reads memory
into temps (`ld` retires), and programs every register (`str`: BIF/DPM/TA-3D kicks). It needs
**no committing `st.a.d`** (log 2026-11-12-2201) and **no pa-ALU writes** — the host (the kernel
driver) owns the CCB ring, control block, sync objects and signals `dma_fence` from the
EVENT_STATUS completion IRQ; pa goes via DOUTA. The full plan is docs/porting/cores/sgx530-edm-path.md
(microkernel session, 0e69b9df).

**Next:** §6 of sgx530-edm-path.md — fold the §9 launch words + the DOUTD control-block DMA into
the SGX530 real init (host not waiting on `ui32InitStatus`), then a minimal EDM-driven TA/3D
kick with host-owned memory. Code only; no board. The microkernel session supplies the reduced
USSE per handler (load→str→kick→wait→ack, no `st.a.d`).
