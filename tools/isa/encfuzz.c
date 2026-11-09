/*
 * encfuzz: catch every instruction libusc's USE assembler encodes, and
 * re-encode mutated copies of it to map input fields to output bits.
 *
 * LD_PRELOAD it (instead of uschook.so) in front of shc. On the first
 * PVRUniFlexCompileToUspBin call it patches a jump over libusc's
 * UseAssembleInstruction (useasm.c; offset 0x109f50 in libusc.so.1.7.862890,
 * args: target, USE_INST *, base (NULL here), out, code offset, context). Each call is
 * passed through, and the 0x100-byte USE_INST and the two output words are
 * logged to $ENCFUZZ_OUT as text:
 *
 *   I <64 hex words of the USE_INST> = <ret> <word0> <word1>
 *
 * With ENCFUZZ_MODE=1, new (opcode, output) pairs (up to PEROP per opcode and
 * hardware opcode; ENCFUZZ_HW=<hex mask> limits it to those hardware opcodes)
 * are also re-encoded in a forked child with one input bit flipped at a time
 * (dwords 0..NFUZZ-1, bits 0..31, skipping dwords that hold pointers), and the opcode dword set to
 * 0..0x1ff, and each arg's type dword set to 0..31; each mutation whose output or return differs is logged as
 *
 *   M <dword> <bit or v=value> = <ret> <word0> <word1>
 *
 * A mutation that crashes or escapes the encoder kills only that child; the
 * parent restarts after it. Nothing is drawn: shc only compiles.
 *
 *   zig cc -target x86-linux-gnu.2.17 -O1 -mstackrealign -shared -fPIC -o encfuzz.so encfuzz.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ENC_OFF 0x109f50
#define NW 64
#define NFUZZ 40	/* dwords mutated: opcode, flags, test, 7 args */
#define PEROP 6	/* instances fuzzed per USEASM opcode */

typedef int (*compile_fn)(void *, void *, void *, void *, void *, void **);
typedef int (*asm_fn)(void *, uint32_t *, uint32_t *, uint32_t *, uint32_t, void *);

static asm_fn enc;
static unsigned char saved[5];
static int fd = -1, mode, in_child;
static uint64_t seen[65536];
static int nseen, perop[0x400 * 32];
static unsigned hwmask = 0xffffffff;	/* $ENCFUZZ_HW: hex bitmask of hardware opcodes to fuzz */

static void put(const char *s)
{
	if (write(fd, s, strlen(s)) < 0)
		_exit(4);
}

static void patch(int on);

static int hook(void *tgt, uint32_t *inst, uint32_t *base, uint32_t *out, uint32_t off, void *ctx);

static void patch(int on)
{
	static unsigned char jmp[5];
	uintptr_t page = (uintptr_t)enc & ~4095u;

	mprotect((void *)page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
	if (on) {
		jmp[0] = 0xe9;
		*(int32_t *)(jmp + 1) = (int32_t)((uintptr_t)hook - ((uintptr_t)enc + 5));
		memcpy((void *)enc, jmp, 5);
	} else {
		memcpy((void *)enc, saved, 5);
	}
	mprotect((void *)page, 8192, PROT_READ | PROT_EXEC);
}

static int is_ptr(uint32_t v)
{
	return v >= 0x01000000 && v < 0xfff00000;
}

/* run mutations [from, total) of one instruction in a child; returns the index to resume at, or total */
static int fuzz_run(void *tgt, const uint32_t *inst, uint32_t *base, uint32_t off, void *ctx,
		    int from, int total, uint32_t r0, uint32_t w0, uint32_t w1)
{
	int pfd[2], st, last = from - 1;
	pid_t pid;

	if (pipe(pfd))
		return total;
	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		in_child = 1;
		for (int m = from; m < total; m++) {
			uint32_t c[NW + 64], o[16] = { 0 };
			char line[96];
			int k, r;

			memcpy(c, inst, NW * 4);
			memset(c + NW, 0, 64 * 4);
			if (m < NFUZZ * 32) {
				k = m / 32;
				if (k == 0 || is_ptr(inst[k]))
					continue;
				c[k] ^= 1u << (m % 32);
			} else if (m < NFUZZ * 32 + 0x200) {
				k = 0;
				c[0] = m - NFUZZ * 32;
				if (c[0] == inst[0])
					continue;
			} else {
				/* arg a's type dword set to 0..31 */
				int a = (m - NFUZZ * 32 - 0x200) / 32;

				k = 6 + 5 * a;
				c[k] = (m - NFUZZ * 32 - 0x200) % 32;
				if (c[k] == inst[k])
					continue;
			}
			if (write(pfd[1], &m, sizeof m) != sizeof m)
				_exit(5);
			r = enc(tgt, c, base, o, off, ctx);
			if ((uint32_t)r == r0 && o[0] == w0 && o[1] == w1)
				continue;
			if (m < NFUZZ * 32)
				snprintf(line, sizeof line, "M %d %d = %d %08x %08x\n", k, m % 32, r, o[0], o[1]);
			else
				snprintf(line, sizeof line, "M %d v=%d = %d %08x %08x\n", k, c[k], r, o[0], o[1]);
			put(line);
		}
		_exit(0);
	}
	close(pfd[1]);
	while (read(pfd[0], &last, sizeof last) == sizeof last)
		;
	close(pfd[0]);
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		return total;
	{
		char line[64];

		snprintf(line, sizeof line, "X %d status %d\n", last, st);
		put(line);
	}
	return last + 1;
}

static int hook(void *tgt, uint32_t *inst, uint32_t *base, uint32_t *out, uint32_t off, void *ctx)
{
	char line[1024];
	int r, n = 0;
	uint64_t key;

	if (in_child)
		_exit(3);	/* an encoder error escaped back into the compiler */
	patch(0);
	r = enc(tgt, inst, base, out, off, ctx);
	n += snprintf(line + n, sizeof line - n, "I");
	for (int i = 0; i < NW; i++)
		n += snprintf(line + n, sizeof line - n, " %x", inst[i]);
	snprintf(line + n, sizeof line - n, " = %d %08x %08x\n", r, out[0], out[1]);
	put(line);
	key = (uint64_t)inst[0] << 48 ^ (uint64_t)out[1] << 32 ^ out[0];
	if (mode == 1) {
		int i;

		for (i = 0; i < nseen && seen[i] != key; i++)
			;
		int po = inst[0] * 32 + (out[1] >> 27);

		if (i == nseen && nseen < 65536 && inst[0] < 0x400 && perop[po] < PEROP && (hwmask >> (out[1] >> 27) & 1)) {
			int total = NFUZZ * 32 + 0x200 + 7 * 32;

			perop[po]++;

			seen[nseen++] = key;
			put("F\n");
			for (int m = 0; m < total;)
				m = fuzz_run(tgt, inst, base, off, ctx, m, total, r, out[0], out[1]);
			put("E\n");
		}
	}
	patch(1);
	return r;
}

static char tag[64] = "untagged";

void uschook_tag(const char *t)
{
	snprintf(tag, sizeof tag, "%s", t);
}

int PVRUniFlexCompileToUspBin(void *ctx, void *flags, void *prog, void *params, void *x, void **out)
{
	static compile_fn real;
	int r;

	if (in_child)
		_exit(3);
	if (!real) {
		void *h = dlopen("libusc.so", RTLD_NOW | RTLD_NOLOAD);
		Dl_info di;

		real = (compile_fn)dlsym(RTLD_NEXT, "PVRUniFlexCompileToUspBin");
		if (!real && h)
			real = (compile_fn)dlsym(h, "PVRUniFlexCompileToUspBin");
		if (!real || !dladdr((void *)real, &di))
			return -1;
		enc = (asm_fn)((char *)di.dli_fbase + ENC_OFF);
		memcpy(saved, (void *)enc, 5);
		mode = getenv("ENCFUZZ_MODE") ? atoi(getenv("ENCFUZZ_MODE")) : 0;
		if (getenv("ENCFUZZ_HW"))
			hwmask = strtoul(getenv("ENCFUZZ_HW"), NULL, 16);
		fd = open(getenv("ENCFUZZ_OUT") ? getenv("ENCFUZZ_OUT") : "/tmp/encfuzz.txt",
			  O_WRONLY | O_CREAT | O_APPEND, 0644);
		patch(1);
	}
	{
		char line[128];

		snprintf(line, sizeof line, "S %s\n", tag);
		put(line);
	}
	r = real(ctx, flags, prog, params, x, out);
	if (in_child)
		_exit(3);
	return r;
}
