/*
 * Framebuffer drawing and a tiny text console.
 *
 * The MMU is off, so every access is Device memory: keep accesses
 * naturally aligned (built with -mstrict-align) and never rely on
 * memcpy/memset from a library.
 */
#include "fb.h"
#include "font.h"

static volatile uint32_t *const fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_bg = C_BLACK;
static int cur_row;

void fb_rect(int x, int y, int w, int h, uint32_t c)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > FB_W) w = FB_W - x;
	if (y + h > FB_H) h = FB_H - y;
	for (int yy = y; yy < y + h; yy++) {
		volatile uint32_t *line = fb + yy * (FB_STRIDE / 4);
		for (int xx = x; xx < x + w; xx++)
			line[xx] = c;
	}
}

void fb_fill(uint32_t c)
{
	fb_rect(0, 0, FB_W, FB_H, c);
}

static void glyph(int col, int row, char ch, uint32_t fg, uint32_t bg)
{
	if (col < 0 || col >= CON_COLS || row < 0 || row >= CON_ROWS)
		return;
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = CON_MARGIN + col * CON_CW, y0 = CON_MARGIN + row * CON_CH;
	for (int gy = 0; gy < 16; gy++)
		for (int sy = 0; sy < CON_SCALE; sy++) {
			volatile uint32_t *p = fb + (y0 + gy * CON_SCALE + sy) * (FB_STRIDE / 4) + x0;
			for (int gx = 0; gx < 8; gx++) {
				uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : bg;
				for (int sx = 0; sx < CON_SCALE; sx++)
					*p++ = c;
			}
		}
}

int con_at(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
	while (*s)
		glyph(col++, row, *s++, fg, bg);
	return col;
}

void con_reset(uint32_t bg)
{
	con_bg = bg;
	cur_row = 0;
	fb_fill(bg);
}

int con_row(void) { return cur_row; }
void con_set_row(int row) { cur_row = row; }
void con_skip(void) { cur_row++; }

#define LABEL_COLS 9

void con_line(const char *label, const char *text)
{
	con_at(0, cur_row, label, C_YELLOW, con_bg);
	con_at(LABEL_COLS, cur_row, text, C_WHITE, con_bg);
	cur_row++;
}

void con_kv(const char *label, const char *k1, uint64_t v1, const char *k2, uint64_t v2)
{
	char buf[96], *p = buf;
	p = fmt_str(p, k1);
	p = fmt_hex(p, v1, v1 >> 32 ? 16 : 8);
	if (k2) {
		p = fmt_str(p, "  ");
		p = fmt_str(p, k2);
		p = fmt_hex(p, v2, v2 >> 32 ? 16 : 8);
	}
	*p = 0;
	con_line(label, buf);
}

char *fmt_str(char *out, const char *s)
{
	while (*s)
		*out++ = *s++;
	*out = 0;
	return out;
}

char *fmt_hex(char *out, uint64_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";
	*out++ = '0';
	*out++ = 'x';
	for (int i = digits - 1; i >= 0; i--)
		*out++ = hex[(v >> (4 * i)) & 0xf];
	*out = 0;
	return out;
}

char *fmt_dec(char *out, uint64_t v)
{
	char tmp[21];
	int n = 0;
	do {
		tmp[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		*out++ = tmp[--n];
	*out = 0;
	return out;
}
