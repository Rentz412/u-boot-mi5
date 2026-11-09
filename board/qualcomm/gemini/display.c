// SPDX-License-Identifier: GPL-2.0+
/* Output console using the stock bootloader's initialized MDP pipeline. */
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <lmb.h>
#include <stdio_dev.h>
#include <string.h>
#include <video_font_8x16.h>


#define GEMINI_FB_BASE 0x83401000UL
#define GEMINI_WIDTH 1080U
#define GEMINI_HEIGHT 1920U
#define GEMINI_STRIDE (GEMINI_WIDTH * 3U)
#define GEMINI_FB_SIZE (GEMINI_STRIDE * GEMINI_HEIGHT)
#define GEMINI_MDP_REV 0x00901000UL
#define GEMINI_CTL_START 0x0090201cUL
#define GEMINI_MDP_107 0x10070000U

static void gemini_pixel(unsigned int x, unsigned int y, u32 color)
{
	u8 *p;

	if (x >= GEMINI_WIDTH || y >= GEMINI_HEIGHT)
		return;
	p = (u8 *)GEMINI_FB_BASE + y * GEMINI_STRIDE + x * 3;
	/* LK's RGB888 console writes the least-significant byte first. */
	p[0] = color;
	p[1] = color >> 8;
	p[2] = color >> 16;
}

static void gemini_rect(unsigned int x, unsigned int y, unsigned int width,
			unsigned int height, u32 color)
{
	unsigned int row, col;

	for (row = 0; row < height; row++)
		for (col = 0; col < width; col++)
			gemini_pixel(x + col, y + row, color);
}

/* Only reuse a pipeline still pointing at the expected 1080-wide LK buffer. */
static bool gemini_matching_pipe(void)
{
	static const ulong pipes[] = {
		0x00905000, 0x00907000, /* VIG0 / VIG1 */
		0x00915000, 0x00917000, /* RGB0 / RGB1 */
		0x00925000, 0x00927000, /* DMA0 / DMA1 */
	};
	unsigned int i;

	if ((readl((void *)GEMINI_MDP_REV) & 0xffff0000) != GEMINI_MDP_107)
		return false;
	for (i = 0; i < sizeof(pipes) / sizeof(pipes[0]); i++) {
		if (readl((void *)(pipes[i] + 0x14)) == GEMINI_FB_BASE &&
		    (readl((void *)(pipes[i] + 0x24)) & 0xffff) == GEMINI_STRIDE)
			return true;
	}
	return false;
}

#define GEMINI_CHAR_WIDTH 16U
#define GEMINI_CHAR_HEIGHT 32U
#define GEMINI_COLS (GEMINI_WIDTH / GEMINI_CHAR_WIDTH)
#define GEMINI_ROWS (GEMINI_HEIGHT / GEMINI_CHAR_HEIGHT)

static unsigned int gemini_col, gemini_row, gemini_escape;
static bool gemini_console_started;

static void gemini_console_sync(void)
{
	flush_dcache_range(GEMINI_FB_BASE, GEMINI_FB_BASE + GEMINI_FB_SIZE);
	writel(1, (void *)GEMINI_CTL_START);
	udelay(20000);
}

static void gemini_console_newline(void)
{
	gemini_col = 0;
	if (++gemini_row < GEMINI_ROWS)
		return;
	memmove((void *)GEMINI_FB_BASE,
		(void *)(GEMINI_FB_BASE + GEMINI_CHAR_HEIGHT * GEMINI_STRIDE),
		GEMINI_FB_SIZE - GEMINI_CHAR_HEIGHT * GEMINI_STRIDE);
	memset((void *)(GEMINI_FB_BASE + GEMINI_FB_SIZE -
		       GEMINI_CHAR_HEIGHT * GEMINI_STRIDE), 0x18,
	       GEMINI_CHAR_HEIGHT * GEMINI_STRIDE);
	gemini_row = GEMINI_ROWS - 1;
}

static void gemini_console_char(char c)
{
	unsigned int row, col, dx, dy, x, y;
	u8 bits;

	/* Strip CSI controls; this first output-only console uses plain text. */
	if (gemini_escape) {
		if (gemini_escape == 1)
			gemini_escape = c == '[' ? 2 : 0;
		else if (c >= 0x40 && c <= 0x7e)
			gemini_escape = 0;
		return;
	}
	if (c == '\033') {
		gemini_escape = 1;
		return;
	}
	if (c == '\r') {
		gemini_col = 0;
		return;
	}
	if (c == '\n') {
		gemini_console_newline();
		return;
	}
	if (c == '\b') {
		if (gemini_col)
			gemini_col--;
		return;
	}
	if (c == '\t') {
		do {
			gemini_console_char(' ');
		} while (gemini_col & 7);
		return;
	}
	if ((u8)c < 0x20)
		return;
	x = gemini_col * GEMINI_CHAR_WIDTH;
	y = gemini_row * GEMINI_CHAR_HEIGHT;
	gemini_rect(x, y, GEMINI_CHAR_WIDTH, GEMINI_CHAR_HEIGHT, 0x181818);
	for (row = 0; row < 16; row++) {
		bits = video_fontdata_8x16[(u8)c * 16 + row];
		for (col = 0; col < 8; col++) {
			if (!(bits & (0x80 >> col)))
				continue;
			for (dy = 0; dy < 2; dy++)
				for (dx = 0; dx < 2; dx++)
					gemini_pixel(x + col * 2 + dx,
						     y + row * 2 + dy, 0xffffff);
		}
	}
	if (++gemini_col == GEMINI_COLS)
		gemini_console_newline();
}

static void gemini_console_putc(struct stdio_dev *dev, const char c)
{
	gemini_console_char(c);
	gemini_console_sync();
}

static void gemini_console_puts(struct stdio_dev *dev, const char *s)
{
	while (*s)
		gemini_console_char(*s++);
	gemini_console_sync();
}

static int gemini_console_start(struct stdio_dev *dev)
{
	if (gemini_console_started)
		return 0;
	memset((void *)GEMINI_FB_BASE, 0x18, GEMINI_FB_SIZE);
	gemini_console_started = true;
	gemini_console_puts(dev, "GEMINI RGB888 STDIO\nOutput console ready\n\n");
	return 0;
}

int gemini_fb_console_register(void)
{
	struct stdio_dev dev = { 0 };
	phys_addr_t addr = GEMINI_FB_BASE;
	int ret;

	if (!gemini_matching_pipe())
		return -ENODEV;
	/* Protect the inherited buffer from later LMB allocations. */
	ret = lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, 0, &addr,
			    ALIGN(GEMINI_FB_SIZE, 4096), LMB_NOOVERWRITE);
	if (ret)
		return ret;
	strcpy(dev.name, "gemini-fb");
	dev.flags = DEV_FLAGS_OUTPUT;
	dev.start = gemini_console_start;
	dev.putc = gemini_console_putc;
	dev.puts = gemini_console_puts;
	return stdio_register(&dev);
}
