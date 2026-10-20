#pragma once
#include <stdint.h>

/*
 * Bootloader framebuffer on the Pixel 3: reserved-memory
 * cont_splash_region@9d400000 (downstream and mainline DT), 1080x2160,
 * a8r8g8b8, stride 1080*4 (mainline sdm845-google-blueline.dts).
 */
#define FB_BASE		0x9d400000UL
#define FB_W		1080
#define FB_H		2160
#define FB_STRIDE	(FB_W * 4)

#define C_BLACK		0xff000000U
#define C_WHITE		0xffffffffU
#define C_RED		0xffff0000U
#define C_GREEN		0xff00ff00U
#define C_BLUE		0xff0000ffU
#define C_YELLOW	0xffffff00U
#define C_GREY		0xff808080U
#define C_PANIC		0xff0000a0U

/* Text grid: 8x16 glyphs drawn at 2x -> 16x32 cells, 64 columns x 66 rows. */
#define CON_SCALE	2
#define CON_CW		(8 * CON_SCALE)
#define CON_CH		(16 * CON_SCALE)
#define CON_MARGIN	28
#define CON_COLS	((FB_W - 2 * CON_MARGIN) / CON_CW)
#define CON_ROWS	((FB_H - 2 * CON_MARGIN) / CON_CH)

void fb_fill(uint32_t c);
void fb_rect(int x, int y, int w, int h, uint32_t c);

/* Text at a grid cell; returns the column after the last character. */
int con_at(int col, int row, const char *s, uint32_t fg, uint32_t bg);

/* Line-oriented output on a background colour set by con_reset(). */
void con_reset(uint32_t bg);
int con_row(void);
void con_set_row(int row);
void con_line(const char *label, const char *text);
void con_kv(const char *label, const char *k1, uint64_t v1, const char *k2, uint64_t v2);
void con_skip(void);

/* Formatting into caller buffers (no libc). */
char *fmt_hex(char *out, uint64_t v, int digits);	/* writes "0x" + digits, returns end */
char *fmt_str(char *out, const char *s);
char *fmt_dec(char *out, uint64_t v);
