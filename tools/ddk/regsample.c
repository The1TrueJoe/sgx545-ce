// SPDX-License-Identifier: MIT
// Copyright © 2026 Joseph Telaak
//
// regsample -- poll a fixed set of NAMED SGX545 registers via /dev/mem in a tight
// loop, timestamped with rdtsc, and record every TRANSITION (a row only when some
// register changed). Delta-compression lets a multi-second, multi-frame window fit
// in a few thousand rows. Read-only; touches only the named registers below, never
// a sweep (a BAR sweep hung an EA3 -- ce5300-vdc-mmio-hazard).
//
//   regsample <bar0_base_hex> <duration_ms> <out.csv> [stopfile]
//
// bar0_base_hex: the SGX PCI BAR0 physical base (from sgxregs.sh / sysfs resource).
// Stops at duration_ms, or early if <stopfile> appears. Needs CONFIG_STRICT_DEVMEM=n
// and root (same as sgxregs.sh). Build: zig cc -target x86_64-linux-musl -static -O2.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

// NAMED registers only (offsets from ukernel.py / sgx545defs.h). No sweep.
// The second group is the 3D-START page-release setup (ta3d-state 2026-11-04): the drain
// is automatic, so ours leaving r724 un-drained means the 3D was armed with the wrong
// pages at 3D start. These capture what do_3d's load_3d_state writes (0x600/0x604/0x64c/
// 0x660 dealloc) + the release-arm (0x63c=3) + the TA counters feeding save_ta_state, so
// the A/B can diff ours vs the DDK's runtime 3D-start values on HW.
static const struct { const char *name; unsigned off; } REGS[] = {
    {"EVENT_STATUS",  0x12C},   // EUR_CR_EVENT_STATUS
    {"EVENT_STATUS2", 0x118},   // EUR_CR_EVENT_STATUS2
    {"r700",          0x700},   // DPM readback (of 0x61c); TA page counter
    {"r710",          0x710},   // TA page counter (feeds save_ta_state)
    {"r724",          0x724},   // DPM counter (the one that drains in the DDK, not ours)
    {"r72c",          0x72C},   // DPM counter
    {"r740",          0x740},   // DPM slot-1 free readback
    {"dealloc600",    0x600},   // 3D free-list / dealloc base (load_3d_state)
    {"dealloc604",    0x604},   // 3D dealloc = pb+0x18 (CR0x700)
    {"arm63c",        0x63C},   // release-arm (ours writes 3 via D3_CONST)
    {"dealloc64c",    0x64C},   // 3D dealloc = pb+0x1c
    {"dealloc660",    0x660},   // 3D dealloc = pb+0x20 | pb+0x24<<16
    {"DPM_3D_FL",     0x680},   // 3D free-list load selector readback
    {"BIF_BANK0",     0xC78},   // EUR_CR_BIF_BANK0
    // out-of-memory path (UKERNEL.md "Out of memory"): which branch the DDK's handler takes
    {"r734",          0x734},   // DPM status the handler tests (& 3 == 1, pc 0x0b90)
    {"r69c",          0x69C},   // written 1 (resume) / 4 (partial render) by the handler
    {"thrTA620",      0x620},   // TA threshold (pb+0x28)
    {"thrZLS624",     0x624},   // ZLS threshold (pb+0x2c); +32 on the partial-render branch
    {"thrGBL628",     0x628},   // global threshold; := pages in use on the reload branch
    {"thrPDS668",     0x668},   // PDS threshold (pb+0x34)
    {"head61c",       0x61C},   // TA free-list head, reloaded from 0x700
    {"r648",          0x648},   // TA free-list state, reloaded from 0x710<<16
    {"r638",          0x638},   // TA free-list state, reloaded from 0x724/0x72c
    {"a74",           0xA74},   // b0 = TA paused by the handler
};
#define NREG (sizeof(REGS) / sizeof(REGS[0]))

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: regsample <bar0_hex> <dur_ms> <out.csv> [stopfile]\n");
        return 2;
    }
    uint64_t base = strtoull(argv[1], 0, 16);
    long dur_ms = atol(argv[2]);
    const char *out = argv[3];
    const char *stopfile = argc > 4 ? argv[4] : 0;

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    uint64_t pgbase = base & ~0xFFFULL, pgoff = base - pgbase;
    size_t maplen = pgoff + 0x4000;             // 16 KB register window (covers 0xC78)
    volatile uint8_t *m = mmap(0, maplen, PROT_READ, MAP_SHARED, fd, pgbase);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    volatile uint8_t *reg = m + pgoff;

    size_t cap = 1u << 21;                       // up to ~2M transition rows
    uint64_t *ts  = malloc(cap * sizeof(uint64_t));
    uint32_t *val = malloc(cap * NREG * sizeof(uint32_t));
    if (!ts || !val) { perror("malloc"); return 1; }

    uint32_t last[NREG];
    for (size_t i = 0; i < NREG; i++) last[i] = ~0u;
    size_t rows = 0, polls = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        uint64_t t = rdtsc();
        uint32_t cur[NREG];
        int changed = 0;
        for (size_t i = 0; i < NREG; i++) {
            cur[i] = *(volatile uint32_t *)(reg + REGS[i].off);
            if (cur[i] != last[i]) changed = 1;
        }
        if (changed && rows < cap) {
            ts[rows] = t;
            memcpy(&val[rows * NREG], cur, sizeof(cur));
            memcpy(last, cur, sizeof(cur));
            rows++;
        }
        polls++;
        if ((polls & 0x3FFF) == 0) {             // check stop conditions every 16K polls
            struct timespec ts1; clock_gettime(CLOCK_MONOTONIC, &ts1);
            long ms = (ts1.tv_sec - t0.tv_sec) * 1000 + (ts1.tv_nsec - t0.tv_nsec) / 1000000;
            if (ms >= dur_ms) break;
            if (stopfile) { struct stat st; if (stat(stopfile, &st) == 0) break; }
            if (rows >= cap) break;
        }
    }

    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 1; }
    fprintf(f, "# base=0x%llx polls=%zu rows=%zu dur_ms=%ld\n",
            (unsigned long long)base, polls, rows, dur_ms);
    fprintf(f, "tsc");
    for (size_t i = 0; i < NREG; i++) fprintf(f, ",%s", REGS[i].name);
    fprintf(f, "\n");
    for (size_t k = 0; k < rows; k++) {
        fprintf(f, "%llu", (unsigned long long)ts[k]);
        for (size_t i = 0; i < NREG; i++) fprintf(f, ",0x%08x", val[k * NREG + i]);
        fprintf(f, "\n");
    }
    fclose(f);
    fprintf(stderr, "regsample: %zu transitions over %zu polls (%ld ms budget)\n",
            rows, polls, dur_ms);
    return 0;
}
