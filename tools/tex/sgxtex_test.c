/*
 * sgxtex_test: prints sgxtex's texel offsets and blit words in a form
 * check_c.py compares with tex.py / present.py (which check.py and
 * present.py --check compare with the DDK's captures).
 *   cc -O1 -o sgxtex_test sgxtex_test.c sgxtex.c && ./sgxtex_test | python3 check_c.py
 */
#include <stdio.h>

#include "sgxtex.h"

static const uint32_t sizes[][3] = { { 1, 1, 1 }, { 2, 1, 2 }, { 8, 32, 4 }, { 64, 64, 4 }, { 128, 32, 2 },
				     { 256, 256, 4 }, { 5, 7, 4 }, { 100, 30, 4 }, { 16, 16, 1 } };

int main(void)
{
	for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
		uint32_t w = sizes[i][0], h = sizes[i][1], bpp = sizes[i][2];

		for (uint32_t lv = 0; (w >> lv) || (h >> lv); lv++) {
			uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;

			printf("tex %u %u %u %u", w, h, bpp, lv);
			for (uint32_t y = 0; y < lh; y++)
				for (uint32_t x = 0; x < lw; x++)
					printf(" %zu", sgxtex_texel_offset(x, y, w, h, bpp, lv));
			printf("\n");
		}
	}
	{
		/* EGL's swap context in the probe captures, and a 512x384 crop blit */
		struct sgxtex_blit_ctx c = { 0x3e107000, 0x2e003000, 0x2e005000, 0x2e003020, 0x370018e0, 0x37002000 };
		const uint32_t blits[][6] = { { 0x3e265000, 720, 480, 736, 0x3e10c000, 736 },
					      { 0x3e281d80, 512, 384, 736, 0x3e10c000, 736 } };
		uint32_t w[SGXTEX_BLIT_BG_WORDS];
		struct sgxpbe p;

		for (unsigned i = 0; i < 2; i++) {
			const uint32_t *b = blits[i];

			printf("blit %#x %u %u %u %#x %u\n", b[0], b[1], b[2], b[3], b[4], b[5]);
			sgxtex_blit_command(w, &c, b[1], b[2]);
			printf("words %#x", 0x32031000u);	/* the swap context's CCB */
			for (int k = 0; k < SGXTEX_BLIT_CMD_WORDS; k++)
				printf(" %#x", w[k]);
			printf("\n");
			sgxtex_blit_bg(w, &c, b[1], b[2]);
			printf("words %#x", c.bg);
			for (int k = 0; k < SGXTEX_BLIT_BG_WORDS; k++)
				printf(" %#x", w[k]);
			printf("\n");
			sgxtex_blit_pixel_program(w, &c, b[0], b[1], b[2], b[3]);
			printf("words %#x", c.pds_primary);
			for (int k = 0; k < SGXTEX_BLIT_PDS_WORDS; k++)
				printf(" %#x", w[k]);
			printf("\n");
			sgxpbe_words(&p, b[1], b[2], b[5], b[4], SGXPBE_FMT_BGRA8888, 0, 0);
			sgxpbe_eot_program(w, &p);
			printf("words %#x", c.eot_use);
			for (int k = 0; k < SGXPBE_EOT_WORDS; k++)
				printf(" %#x", w[k]);
			printf("\n");
		}
	}
	return 0;
}
