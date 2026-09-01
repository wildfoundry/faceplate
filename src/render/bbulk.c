/*
 * kmscon - Bit-Blitting Bulk Text Renderer Backend
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * SECTION:text_bbulk.c
 * @short_description: Bit-Blitting Bulk Text Renderer Backend
 * @include: text.h
 *
 * Similar to the bblit renderer but assembles an array of blit-requests and
 * pushes all of them at once to the video device.
 *
 * Only push cells that have changed from previous frame, and the frame before
 * as kmscon uses double buffering.
 * bbulk->prev holds the previous cell content, bbulk->damaged tells if the
 * previous cell content was different from its predecessor.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font/font.h"
#include "shl/log.h"
#include "shl/lru.h"
#include "shl/misc.h"
#include "text.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "text_bbulk"

#define ID_DAMAGED 0x80000001
#define ID_OVERFLOW 0x80000002

// Max horizontal distance of two damaged cells to be merged in a damage rectangle
#define DAMAGE_MERGE_LEN 3

/* Flags for each cell */
typedef union {
	struct {
		uint8_t double_width : 1;
		uint8_t damaged : 1;
	};
	uint8_t u8;
} cell_flags_t;

struct bbulk {
	unsigned int sw;	     /* screen width */
	unsigned int sh;	     /* screen height */
	unsigned int off_x;	     /* offset of the first cell */
	unsigned int off_y;	     /* offset of the first cell */
	unsigned int max_x;	     /* maximum x offset of the last cell */
	unsigned int max_y;	     /* maximum y offset of the last cell */
	struct tsm_screen_attr attr; /* attributes for background color */

	unsigned int requests; /* number of blend calls, for debugging */
	struct shl_lru *glyphs;
	struct tsm_screen_cell *cells;
	cell_flags_t *cell_flags;
	unsigned int cell_count;

	struct video_rect *damage_rects;
	unsigned int damage_rect_len;
	uint8_t redraw;
};

static int bbulk_init(struct kmscon_text *txt)
{
	struct bbulk *bb;

	bb = malloc(sizeof(*bb));
	if (!bb)
		return -ENOMEM;
	memset(bb, 0, sizeof(*bb));

	txt->data = bb;
	return 0;
}

static void bbulk_destroy(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	free(bb);
}

static void damage_cell(struct bbulk *bb, unsigned int off)
{
	bb->cells[off].ch = ID_DAMAGED;
	bb->cell_flags[off].damaged = 1;
}

static void compute_border(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int card_cols, content_h, free_h, top_bias;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		card_cols = txt->cols + 2 * FACEPLATE_CHROME_PAD_COLS;
		content_h = (txt->rows + FACEPLATE_CHROME_TOP_ROWS + FACEPLATE_CHROME_BOTTOM_ROWS) *
			    FONT_HEIGHT(txt);
		bb->off_x = (bb->sw - card_cols * FONT_WIDTH(txt)) / 2;
		/*
		 * Vertical rhythm: keep a real top inset for logo/identity, and
		 * leave most leftover space below the footer (overscan + breathing
		 * room) so nothing is pinned to the physical bottom edge.
		 */
		if (bb->sh > content_h) {
			free_h = bb->sh - content_h;
			top_bias = free_h / 5;
			if (top_bias < FONT_HEIGHT(txt))
				top_bias = FONT_HEIGHT(txt);
			if (top_bias > free_h / 3)
				top_bias = free_h / 3;
			bb->off_y = top_bias;
		} else {
			bb->off_y = 0;
		}
		bb->max_x = bb->off_x + card_cols * FONT_WIDTH(txt);
		bb->max_y = bb->off_y + content_h;
	} else {
		bb->off_x = (bb->sw - (txt->rows + 2) * FONT_HEIGHT(txt)) / 2;
		bb->off_y = (bb->sh - txt->cols * FONT_WIDTH(txt)) / 2;
		bb->max_x = bb->off_x + txt->rows * FONT_HEIGHT(txt);
		bb->max_y = bb->off_y + txt->cols * FONT_WIDTH(txt);
	}
	display_set_cursor_offset(txt->disp,
				  bb->off_x + FACEPLATE_CHROME_PAD_COLS * FONT_WIDTH(txt),
				  bb->off_y + FACEPLATE_CHROME_TOP_ROWS * FONT_HEIGHT(txt));
}

static int bbulk_set(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int max_damage_rects;
	int i;

	memset(bb, 0, sizeof(*bb));

	bb->sw = display_get_width(txt->disp);
	bb->sh = display_get_height(txt->disp);

	if (!bb->sw || !bb->sh)
		return -EINVAL;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		txt->max_cols = bb->sw / FONT_WIDTH(txt);
		txt->max_rows = bb->sh / FONT_HEIGHT(txt);
	} else {
		txt->max_rows = bb->sw / FONT_HEIGHT(txt);
		txt->max_cols = bb->sh / FONT_WIDTH(txt);
	}
	if (txt->max_cols > FACEPLATE_CHROME_HORIZONTAL_CELLS + 2 * FACEPLATE_CHROME_PAD_COLS)
		txt->max_cols -= FACEPLATE_CHROME_HORIZONTAL_CELLS + 2 * FACEPLATE_CHROME_PAD_COLS;
	if (txt->max_rows > FACEPLATE_CHROME_TOP_ROWS + FACEPLATE_CHROME_BOTTOM_ROWS +
				     FACEPLATE_CHROME_SAFE_MARGIN_ROWS)
		txt->max_rows -= FACEPLATE_CHROME_TOP_ROWS + FACEPLATE_CHROME_BOTTOM_ROWS +
				 FACEPLATE_CHROME_SAFE_MARGIN_ROWS;
	txt->cols = txt->max_cols;
	txt->rows = txt->max_rows;
	compute_border(txt);

	bb->cell_count = txt->max_cols * (txt->max_rows + 2);
	max_damage_rects =
		SHL_DIV_ROUND_UP(txt->max_cols, DAMAGE_MERGE_LEN + 1) * (txt->max_rows + 2) + 2;

	bb->cells = malloc(sizeof(*bb->cells) * bb->cell_count);
	if (!bb->cells)
		goto err_no_mem;
	memset(bb->cells, 0, sizeof(*bb->cells) * bb->cell_count);

	bb->cell_flags = malloc(sizeof(*bb->cell_flags) * bb->cell_count);
	if (!bb->cell_flags)
		goto free_prev;

	bb->damage_rects = malloc(sizeof(*bb->damage_rects) * max_damage_rects);
	if (!bb->damage_rects)
		goto free_damages;

	for (i = 0; i < (int)bb->cell_count; i++)
		damage_cell(bb, i);

	/* lru size should be at least bb->cells large */
	bb->glyphs = shl_lru_new(2 * bb->cell_count);
	if (!bb->glyphs)
		goto free_r_damages;
	return 0;

free_r_damages:
	free(bb->damage_rects);
free_damages:
	free(bb->cell_flags);
free_prev:
	free(bb->cells);
err_no_mem:
	return -ENOMEM;
}

static void bbulk_unset(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	shl_lru_free(bb->glyphs);
	free(bb->damage_rects);
	free(bb->cell_flags);
	free(bb->cells);
	bb->glyphs = NULL;
	bb->damage_rects = NULL;
	bb->cell_flags = NULL;
	bb->cells = NULL;
}

static void bbulk_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows)
{
	struct bbulk *bb = txt->data;

	txt->cols = cols;
	txt->rows = rows;
	compute_border(txt);
	bb->redraw = 2;
}

static int bbulk_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	bbulk_unset(txt);
	txt->orientation = orientation;
	return bbulk_set(txt);
}

/*
 * Rotate a glyph to the given orientation
 * Return a new rotated glyph, and free the original glyph
 */
static struct kmscon_glyph *bbulk_rotate_glyph(struct kmscon_glyph *glyph,
					       enum Orientation orientation)
{
	struct video_buffer *buf = &glyph->buf;
	struct kmscon_glyph *rglyph;
	int width, height, i, j;
	uint8_t *dst, *src;
	unsigned int size = sizeof(*rglyph) + glyph->buf.width * glyph->buf.height;

	rglyph = malloc(size);
	if (!rglyph)
		goto err_free;

	memset(rglyph, 0, size);

	if (orientation == OR_NORMAL || orientation == OR_UPSIDE_DOWN) {
		width = buf->width;
		height = buf->height;
	} else {
		width = buf->height;
		height = buf->width;
	}
	src = buf->data;
	dst = rglyph->buf.data;

	switch (orientation) {
	default:
	case OR_NORMAL:
		/* should never happen */
		break;
	case OR_RIGHT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++) {
				dst[j * width + (width - i - 1)] = src[j];
			}
			src += buf->width;
		}
		break;
	case OR_UPSIDE_DOWN:
		src += (buf->height - 1) * buf->width;
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++)
				dst[j] = src[buf->width - j - 1];
			dst += width;
			src -= buf->width;
		}
		break;
	case OR_LEFT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++) {
				dst[(height - j - 1) * width + i] = src[j];
			}
			src += buf->width;
		}
	}
	rglyph->buf.width = width;
	rglyph->buf.height = height;
	rglyph->double_width = glyph->double_width;

err_free:
	free(glyph);
	return rglyph;
}

static struct kmscon_glyph *find_glyph(struct kmscon_text *txt, const struct tsm_screen_cell *cell)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *glyph;
	struct kmscon_font *font = txt->font;
	uint32_t ch = cell->ch ? cell->ch : ' ';
	uint64_t id;

	font->attr.underline = !!cell->attr2.underline;
	font->attr.italic = !!cell->attr2.italic;
	font->attr.bold = !!cell->attr2.bold;

	if (!kmscon_font_has_glyph(font, ch))
		ch = FONT_REPLACEMENT_CHAR;

	id = kmscon_glyph_id(cell->ch, cell->attr2.u8);

	glyph = shl_lru_get(bb->glyphs, id);
	if (glyph)
		return glyph;

	glyph = kmscon_font_render(font, ch);
	if (!glyph)
		return NULL;

	if (txt->orientation != OR_NORMAL)
		glyph = bbulk_rotate_glyph(glyph, txt->orientation);

	if (shl_lru_insert(bb->glyphs, id, glyph)) {
		free(glyph);
		return NULL;
	}
	return glyph;
}

/*
 * Returns the top left corner of a Cell
 */
static void set_coordinate(struct kmscon_text *txt, unsigned int *x, unsigned int *y,
			   unsigned int posx, unsigned int posy)
{
	struct bbulk *bb = txt->data;

	switch (txt->orientation) {
	case OR_NORMAL:
		*x = posx * FONT_WIDTH(txt) + bb->off_x;
		*y = posy * FONT_HEIGHT(txt) + bb->off_y;
		break;
	case OR_UPSIDE_DOWN:
		*x = bb->max_x - (posx + 1) * FONT_WIDTH(txt);
		*y = bb->max_y - (posy + 1) * FONT_HEIGHT(txt);
		break;
	case OR_RIGHT:
		*x = bb->max_x - (posy + 1) * FONT_HEIGHT(txt);
		*y = posx * FONT_WIDTH(txt) + bb->off_y;
		break;
	case OR_LEFT:
		*x = posy * FONT_HEIGHT(txt) + bb->off_x;
		*y = bb->max_y - (posx + 1) * FONT_WIDTH(txt);
		break;
	}
}

static void set_color(struct video_blend_req *req, const struct tsm_screen_cell *cell)
{
	req->fr = cell->fg.r;
	req->fg = cell->fg.g;
	req->fb = cell->fg.b;
	req->br = cell->bg.r;
	req->bg = cell->bg.g;
	req->bb = cell->bg.b;
}

static int bbulk_draw_cell(struct kmscon_text *txt, const struct tsm_screen_cell *cell,
			   unsigned int posx, unsigned int posy)
{
	struct bbulk *bb = txt->data;
	unsigned int offset = posx + posy * txt->cols;
	struct tsm_screen_cell *cur_cell = &bb->cells[offset];
	struct kmscon_glyph *glyph;
	struct video_blend_req req;
	bool last_col = (posx == txt->cols - 1);
	bool changed;

	// left cell overflow on this cell.
	if ((cell->ch == 0 || cell->ch == ' ') && posx && bb->cell_flags[offset - 1].double_width) {
		if (cur_cell->ch == ID_OVERFLOW) {
			bb->cell_flags[offset].damaged = 0;
			return 0;
		}
		cur_cell->ch = ID_OVERFLOW;
		bb->cell_flags[offset].damaged = 1;
		return 0;
	}

	changed = memcmp(cur_cell, cell, sizeof(*cell));

	if (!changed) {
		/* Cell content is unchanged */
		if (bb->cell_flags[offset].double_width && !last_col) {
			if (!bb->cell_flags[offset].damaged && !bb->cell_flags[offset + 1].damaged)
				return 0;
			bb->cell_flags[offset].damaged = 0;
		} else {
			if (!bb->cell_flags[offset].damaged)
				return 0;
			bb->cell_flags[offset].damaged = 0;
		}
	} else {
		bb->cell_flags[offset].damaged = 1;
	}

	*cur_cell = *cell;

	glyph = find_glyph(txt, cur_cell);
	if (!glyph)
		return -ENOMEM;

	if (glyph->double_width && !last_col && changed)
		damage_cell(bb, offset + 1);

	bb->cell_flags[offset].double_width = glyph->double_width;

	if (glyph->double_width && !last_col &&
	    (txt->orientation == OR_LEFT || txt->orientation == OR_UPSIDE_DOWN))
		/*
		 * In case of left or upside down orientation, we need to draw to the
		 * next cell, as the glyph is already rotated, so start on the next cell
		 * and end on this cell
		 */
		set_coordinate(txt, &req.x, &req.y, posx + 1 + FACEPLATE_CHROME_PAD_COLS,
			       posy + FACEPLATE_CHROME_TOP_ROWS);
	else
		set_coordinate(txt, &req.x, &req.y, posx + FACEPLATE_CHROME_PAD_COLS,
			       posy + FACEPLATE_CHROME_TOP_ROWS);

	req.w = glyph->buf.width;
	req.h = glyph->buf.height;

	/* Truncate the glyph if it would overflow in the border */
	if (glyph->double_width && last_col) {
		if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN)
			req.w = FONT_WIDTH(txt);
		else if (txt->orientation == OR_RIGHT || txt->orientation == OR_LEFT)
			req.h = FONT_WIDTH(txt);
	}

	req.buf = &glyph->buf;
	set_color(&req, cur_cell);
	display_blend(txt->disp, &req);
	bb->requests++;
	return 0;
}

static int bbulk_draw(struct kmscon_text *txt, const struct tsm_screen_cell *cells,
		      struct kmscon_cursor *cursor)
{
	unsigned int posx, posy, off;

	for (posy = 0; posy < txt->rows; posy++) {
		for (posx = 0; posx < txt->cols; posx++) {
			off = posx + posy * txt->cols;

			if (cursor->visible && cursor->x == posx && cursor->y == posy)
				bbulk_draw_cell(txt, &cursor->cell, posx, posy);
			else if (cells[off].attr2.blink && txt->blinking) {
				struct tsm_screen_cell cell = cells[off];

				cell.ch = ' ';
				bbulk_draw_cell(txt, &cell, posx, posy);
			} else
				bbulk_draw_cell(txt, &cells[off], posx, posy);
		}
	}
	return 0;
}

static bool chrome_line_empty(const char *s)
{
	if (!s)
		return true;
	for (; *s; ++s) {
		if (*s != ' ' && *s != '\t')
			return false;
	}
	return true;
}

static int paint_chrome_cell(struct kmscon_text *txt, unsigned int gx, unsigned int gy,
			     uint8_t br, uint8_t bg, uint8_t bb, uint8_t fr, uint8_t fg,
			     uint8_t fb, unsigned char ch)
{
	struct tsm_screen_cell cell = {0};
	struct kmscon_glyph *glyph;
	struct video_blend_req req;

	cell.ch = ch;
	cell.fg.r = fr;
	cell.fg.g = fg;
	cell.fg.b = fb;
	cell.bg.r = br;
	cell.bg.g = bg;
	cell.bg.b = bb;
	glyph = find_glyph(txt, &cell);
	if (!glyph)
		return -ENOMEM;
	set_coordinate(txt, &req.x, &req.y, gx, gy);
	req.w = glyph->buf.width;
	req.h = glyph->buf.height;
	req.buf = &glyph->buf;
	set_color(&req, &cell);
	display_blend(txt->disp, &req);
	return 0;
}

static int paint_chrome_span(struct kmscon_text *txt, unsigned int y, unsigned int x0,
			     unsigned int x1, uint8_t br, uint8_t bg, uint8_t bb)
{
	unsigned int x;
	int ret;

	for (x = x0; x < x1; ++x) {
		ret = paint_chrome_cell(txt, x, y, br, bg, bb, 180, 196, 255, ' ');
		if (ret)
			return ret;
	}
	return 0;
}

static int fill_solid_rect(struct display *disp, unsigned int x, unsigned int y, unsigned int w,
			   unsigned int h, uint8_t r, uint8_t g, uint8_t b)
{
	struct video_buffer *buf;
	struct video_blend_req req;
	size_t n;
	unsigned int i;

	if (!w || !h)
		return 0;
	n = (size_t)w * (size_t)h;
	buf = malloc(sizeof(*buf) + n);
	if (!buf)
		return -ENOMEM;
	buf->width = w;
	buf->height = h;
	for (i = 0; i < n; ++i)
		buf->data[i] = 255;
	req.buf = buf;
	req.x = x;
	req.y = y;
	req.w = w;
	req.h = h;
	req.fr = r;
	req.fg = g;
	req.fb = b;
	req.br = r;
	req.bg = g;
	req.bb = b;
	display_blend(disp, &req);
	free(buf);
	return 0;
}

static int stroke_card_border(struct display *disp, unsigned int x, unsigned int y, unsigned int w,
			      unsigned int h, unsigned int px, uint8_t r, uint8_t g, uint8_t b)
{
	int ret;

	if (!w || !h || !px)
		return 0;
	/* Top */
	ret = fill_solid_rect(disp, x, y, w, px, r, g, b);
	if (ret)
		return ret;
	/* Bottom */
	ret = fill_solid_rect(disp, x, y + h - px, w, px, r, g, b);
	if (ret)
		return ret;
	/* Left */
	ret = fill_solid_rect(disp, x, y, px, h, r, g, b);
	if (ret)
		return ret;
	/* Right */
	return fill_solid_rect(disp, x + w - px, y, px, h, r, g, b);
}

static int paint_chrome_text(struct kmscon_text *txt, unsigned int y, unsigned int x0,
			     unsigned int max_cols, const char *line, uint8_t br, uint8_t bg,
			     uint8_t bb, uint8_t fr, uint8_t fg, uint8_t fb)
{
	size_t len = line ? strlen(line) : 0;
	unsigned int x;
	int ret;

	for (x = 0; x < max_cols && x < len; ++x) {
		ret = paint_chrome_cell(txt, x0 + x, y, br, bg, bb, fr, fg, fb,
					(unsigned char)line[x]);
		if (ret)
			return ret;
	}
	return 0;
}

static int paint_chrome_text_right(struct kmscon_text *txt, unsigned int y, unsigned int x0,
				   unsigned int max_cols, const char *line, uint8_t br, uint8_t bg,
				   uint8_t bb, uint8_t fr, uint8_t fg, uint8_t fb)
{
	size_t len = line ? strlen(line) : 0;
	unsigned int start;

	if (!len)
		return 0;
	if (len >= max_cols)
		return paint_chrome_text(txt, y, x0, max_cols, line + (len - max_cols), br, bg, bb,
					 fr, fg, fb);
	start = x0 + max_cols - (unsigned int)len;
	return paint_chrome_text(txt, y, start, max_cols, line, br, bg, bb, fr, fg, fb);
}

static void connection_fg(struct kmscon_text *txt, uint8_t *fr, uint8_t *fg, uint8_t *fb)
{
	if (txt->connection_alert) {
		*fr = FACEPLATE_ALERT_FG_R;
		*fg = FACEPLATE_ALERT_FG_G;
		*fb = FACEPLATE_ALERT_FG_B;
	} else {
		*fr = FACEPLATE_ONLINE_FG_R;
		*fg = FACEPLATE_ONLINE_FG_G;
		*fb = FACEPLATE_ONLINE_FG_B;
	}
}

static void status_fg_for_line(struct kmscon_text *txt, const char *line, uint8_t *fr, uint8_t *fg,
			      uint8_t *fb)
{
	bool alert = false;

	if (txt->connection_alert && line &&
	    (strstr(line, "Offline") || strstr(line, "Unknown")))
		alert = true;
	if (txt->rauc_alert && line && strstr(line, "RAUC"))
		alert = true;
	if (alert) {
		*fr = FACEPLATE_ALERT_FG_R;
		*fg = FACEPLATE_ALERT_FG_G;
		*fb = FACEPLATE_ALERT_FG_B;
	} else {
		*fr = FACEPLATE_MUTED_FG_R;
		*fg = FACEPLATE_MUTED_FG_G;
		*fb = FACEPLATE_MUTED_FG_B;
	}
}

static int bbulk_draw_status(struct kmscon_text *txt, const char *line)
{
	struct bbulk *bb = txt->data;
	const char *title = txt->chrome_logo ? "" : txt->chrome_title;
	unsigned int card_cols = txt->cols + 2 * FACEPLATE_CHROME_PAD_COLS;
	unsigned int card_y0 = FACEPLATE_CHROME_CARD_Y0;
	unsigned int term_y0 = FACEPLATE_CHROME_TOP_ROWS;
	unsigned int term_y1 = FACEPLATE_CHROME_TOP_ROWS + txt->rows;
	unsigned int pad_bottom_y = term_y1;
	unsigned int secondary_y = term_y1 + FACEPLATE_CHROME_PAD_ROWS +
				   FACEPLATE_CHROME_FOOTER_GAP_ROWS;
	unsigned int y, x;
	unsigned int card_px, card_py, card_pw, card_ph, logo_y, logo_gap_h;
	uint8_t fr, fg, fb;
	size_t len;
	int ret;
	struct video_blend_req req;
	const char *host = txt->identity_hostname;
	const char *status = txt->identity_status;
	const char *supporting = txt->identity_supporting;
	const char *secondary = txt->identity_secondary;
	const char *compact = txt->identity_compact[0] ? txt->identity_compact : line;
	unsigned int right_x0 = FACEPLATE_CHROME_PAD_COLS;
	unsigned int right_cols = txt->cols;

	(void)line;

	/* Keep right-band text clear of the wordmark. */
	if (txt->chrome_logo && FONT_WIDTH(txt)) {
		unsigned int logo_cols =
			(txt->chrome_logo->width + FONT_WIDTH(txt) - 1) / FONT_WIDTH(txt) + 2;
		if (right_cols > logo_cols + 8) {
			right_x0 = FACEPLATE_CHROME_PAD_COLS + logo_cols;
			right_cols -= logo_cols;
		}
	}

	/* Page chrome for the brand band + gap (never inside the terminal card). */
	for (y = 0; y < card_y0; ++y) {
		ret = paint_chrome_span(txt, y, 0, card_cols, FACEPLATE_PAGE_R, FACEPLATE_PAGE_G,
					FACEPLATE_PAGE_B);
		if (ret)
			return ret;
	}

	if (title[0]) {
		len = strlen(title);
		for (x = 0; x < card_cols && x < len; ++x) {
			ret = paint_chrome_cell(txt, x, 0, FACEPLATE_PAGE_R, FACEPLATE_PAGE_G,
						FACEPLATE_PAGE_B, 244, 248, 255,
						(unsigned char)title[x]);
			if (ret)
				return ret;
		}
	}

	/*
	 * Right band — operator priority:
	 *   Online/Offline (lead) → hostname → IP | OS
	 * Active: single compact rail on the first brand row.
	 */
	if (txt->chrome_idle) {
		connection_fg(txt, &fr, &fg, &fb);
		ret = paint_chrome_text_right(txt, 0, right_x0, right_cols,
					      status[0] ? status : "Unknown", FACEPLATE_PAGE_R,
					      FACEPLATE_PAGE_G, FACEPLATE_PAGE_B, fr, fg, fb);
		if (ret)
			return ret;
		if (FACEPLATE_CHROME_BRAND_ROWS > 1) {
			ret = paint_chrome_text_right(txt, 1, right_x0, right_cols,
						      host[0] ? host : "Device", FACEPLATE_PAGE_R,
						      FACEPLATE_PAGE_G, FACEPLATE_PAGE_B,
						      FACEPLATE_HOST_FG_R, FACEPLATE_HOST_FG_G,
						      FACEPLATE_HOST_FG_B);
			if (ret)
				return ret;
		}
		if (FACEPLATE_CHROME_BRAND_ROWS > 2 && supporting[0]) {
			ret = paint_chrome_text_right(txt, 2, right_x0, right_cols, supporting,
						      FACEPLATE_PAGE_R, FACEPLATE_PAGE_G,
						      FACEPLATE_PAGE_B, FACEPLATE_MUTED_FG_R,
						      FACEPLATE_MUTED_FG_G, FACEPLATE_MUTED_FG_B);
			if (ret)
				return ret;
		}
	} else {
		status_fg_for_line(txt, compact, &fr, &fg, &fb);
		if (!txt->connection_alert && !txt->rauc_alert)
			connection_fg(txt, &fr, &fg, &fb);
		ret = paint_chrome_text_right(txt, 0, right_x0, right_cols, compact, FACEPLATE_PAGE_R,
					      FACEPLATE_PAGE_G, FACEPLATE_PAGE_B, fr, fg, fb);
		if (ret)
			return ret;
	}

	/* Black card body: top pad, side pads, bottom pad — terminal cells only. */
	ret = paint_chrome_span(txt, card_y0, 0, card_cols, FACEPLATE_CARD_R, FACEPLATE_CARD_G,
				FACEPLATE_CARD_B);
	if (ret)
		return ret;
	for (y = term_y0; y < term_y1; ++y) {
		ret = paint_chrome_span(txt, y, 0, FACEPLATE_CHROME_PAD_COLS, FACEPLATE_CARD_R,
					FACEPLATE_CARD_G, FACEPLATE_CARD_B);
		if (ret)
			return ret;
		ret = paint_chrome_span(txt, y, FACEPLATE_CHROME_PAD_COLS + txt->cols, card_cols,
					FACEPLATE_CARD_R, FACEPLATE_CARD_G, FACEPLATE_CARD_B);
		if (ret)
			return ret;
	}
	ret = paint_chrome_span(txt, pad_bottom_y, 0, card_cols, FACEPLATE_CARD_R, FACEPLATE_CARD_G,
				FACEPLATE_CARD_B);
	if (ret)
		return ret;

	card_px = bb->off_x;
	card_py = bb->off_y + card_y0 * FONT_HEIGHT(txt);
	card_pw = card_cols * FONT_WIDTH(txt);
	card_ph = (FACEPLATE_CHROME_PAD_ROWS + txt->rows + FACEPLATE_CHROME_PAD_ROWS) *
		  FONT_HEIGHT(txt);
	ret = stroke_card_border(txt->disp, card_px, card_py, card_pw, card_ph,
				 FACEPLATE_CHROME_BORDER_PX, FACEPLATE_CARD_BORDER_R,
				 FACEPLATE_CARD_BORDER_G, FACEPLATE_CARD_BORDER_B);
	if (ret)
		return ret;

	for (y = pad_bottom_y + 1; y <= secondary_y; ++y) {
		ret = paint_chrome_span(txt, y, 0, card_cols, FACEPLATE_PAGE_R, FACEPLATE_PAGE_G,
					FACEPLATE_PAGE_B);
		if (ret)
			return ret;
	}
	if (secondary[0]) {
		status_fg_for_line(txt, secondary, &fr, &fg, &fb);
		if (!txt->rauc_alert) {
			fr = FACEPLATE_MUTED_FG_R;
			fg = FACEPLATE_MUTED_FG_G;
			fb = FACEPLATE_MUTED_FG_B;
		}
		ret = paint_chrome_text(txt, secondary_y, FACEPLATE_CHROME_PAD_COLS, txt->cols,
					secondary, FACEPLATE_PAGE_R, FACEPLATE_PAGE_G,
					FACEPLATE_PAGE_B, fr, fg, fb);
		if (ret)
			return ret;
	}

	if (txt->chrome_logo && txt->orientation == OR_NORMAL) {
		logo_gap_h = FACEPLATE_CHROME_BRAND_ROWS * FONT_HEIGHT(txt);
		logo_y = bb->off_y;
		if (logo_gap_h > txt->chrome_logo->height)
			logo_y = bb->off_y + (logo_gap_h - txt->chrome_logo->height) / 2;
		req.buf = txt->chrome_logo;
		req.x = bb->off_x + FONT_WIDTH(txt);
		req.y = logo_y;
		req.w = txt->chrome_logo->width;
		req.h = txt->chrome_logo->height;
		req.fr = 244;
		req.fg = 248;
		req.fb = 255;
		req.br = FACEPLATE_PAGE_R;
		req.bg = FACEPLATE_PAGE_G;
		req.bb = FACEPLATE_PAGE_B;
		display_blend(txt->disp, &req);
	}
	return 0;
}

/*
 * When the pointer move over, mark the 4 underlying cells as damaged.
 */
static void mark_damaged(struct kmscon_text *txt, struct bbulk *bb, unsigned int x, unsigned int y)
{
	unsigned int posx = 0;
	unsigned int posy = 0;
	unsigned int fw, fh, off;
	fw = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
	fh = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);

	if (x > fw)
		posx = (x - fw) / FONT_WIDTH(txt);
	if (y > fh)
		posy = (y - fh) / FONT_HEIGHT(txt);

	if (posx >= txt->cols)
		posx = txt->cols - 1;

	if (posy >= txt->rows)
		posy = txt->rows - 1;

	off = posx + posy * txt->cols;

	damage_cell(bb, off);

	if (posx + 1 < txt->cols)
		damage_cell(bb, off + 1);

	if (posy + 1 < txt->rows)
		damage_cell(bb, off + txt->cols);

	if (posx + 1 < txt->cols && posy + 1 < txt->rows)
		damage_cell(bb, off + 1 + txt->cols);
}

static unsigned int clamp(unsigned int val, unsigned int min, unsigned int max)
{
	if (val < min)
		return min;
	if (val > max)
		return max;
	return val;
}

/*
 * pointer_x and pointer_y are the center of the pointer sprite, in the
 * non-rotated screen.
 */
static void set_pointer_coordinate(struct bbulk *bb, struct kmscon_text *txt,
				   struct video_blend_req *req, unsigned int pointer_x,
				   unsigned int pointer_y)
{
	unsigned int hf_w, hf_h, x, y;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		hf_w = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);
	} else {
		hf_w = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
	}

	switch (txt->orientation) {
	default:
	case OR_NORMAL:
		x = pointer_x + bb->off_x + FACEPLATE_CHROME_PAD_COLS * FONT_WIDTH(txt);
		y = pointer_y + bb->off_y + FACEPLATE_CHROME_TOP_ROWS * FONT_HEIGHT(txt);
		break;
	case OR_UPSIDE_DOWN:
		x = bb->max_x - pointer_x;
		y = bb->max_y - pointer_y;
		break;
	case OR_RIGHT:
		x = bb->max_x - pointer_y;
		y = pointer_x + bb->off_y + FACEPLATE_CHROME_TOP_ROWS * FONT_HEIGHT(txt);
		break;
	case OR_LEFT:
		x = pointer_y + bb->off_x;
		y = bb->max_y - pointer_x;
		break;
	}
	x = clamp(x, hf_w, bb->max_x - hf_w);
	y = clamp(y, hf_h, bb->max_y - hf_h);

	req->x = x - hf_w;
	req->y = y - hf_h;
}

static int bbulk_draw_pointer(struct kmscon_text *txt, unsigned int pointer_x,
			      unsigned int pointer_y)
{
	struct bbulk *bb = txt->data;
	struct video_blend_req req;
	struct kmscon_glyph *bb_glyph;
	struct tsm_screen_cell pointer_cell = {0};
	unsigned int fw2 = FONT_WIDTH(txt) / 2;
	unsigned int fh2 = FONT_HEIGHT(txt) / 2;

	pointer_cell.ch = 'I';

	pointer_x = clamp(pointer_x, fw2, txt->cols * FONT_WIDTH(txt) - fw2);
	pointer_y = clamp(pointer_y, fh2, txt->rows * FONT_HEIGHT(txt) - fh2);

	bb_glyph = find_glyph(txt, &pointer_cell);
	if (!bb_glyph)
		return -ENOMEM;

	req.buf = &bb_glyph->buf;
	req.w = bb_glyph->buf.width;
	req.h = bb_glyph->buf.height;
	set_pointer_coordinate(bb, txt, &req, pointer_x, pointer_y);
	mark_damaged(txt, bb, pointer_x, pointer_y);

	req.fr = bb->attr.fr;
	req.fg = bb->attr.fg;
	req.fb = bb->attr.fb;
	req.br = bb->attr.br;
	req.bg = bb->attr.bg;
	req.bb = bb->attr.bb;
	display_blend(txt->disp, &req);
	bb->requests++;
	return 0;
}

static void add_damage(struct bbulk *bb, struct video_rect *r)
{
	struct video_rect *out = &bb->damage_rects[bb->damage_rect_len];

	*out = *r;
	bb->damage_rect_len++;
}

static void merge_damage(struct bbulk *bb, struct video_rect *r)
{
	struct video_rect *out = &bb->damage_rects[bb->damage_rect_len - 1];

	out->x1 = min(out->x1, r->x1);
	out->x2 = max(out->x2, r->x2);
	out->y1 = min(out->y1, r->y1);
	out->y2 = max(out->y2, r->y2);
}

/*
 * Simple merge algorithm, on each line, if two damaged cells are less than
 * DAMAGE_MERGE_LEN away, include the two cells in one damage rectangle.
 */
static void bbulk_compute_damage(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int posx, posy, off;
	struct video_rect r;
	unsigned int x1 = 0, y1 = 0;
	unsigned int fw, fh;
	int prev;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		fw = FONT_WIDTH(txt);
		fh = FONT_HEIGHT(txt);
	} else {
		fw = FONT_HEIGHT(txt);
		fh = FONT_WIDTH(txt);
	}

	for (posy = 0; posy < txt->rows + 2; posy++) {
		prev = 0;
		for (posx = 0; posx < txt->cols; posx++) {
			off = posx + posy * txt->cols;
			if (!bb->cell_flags[off].damaged) {
				if (prev)
					prev--;
				continue;
			}
			set_coordinate(txt, &x1, &y1, posx + FACEPLATE_CHROME_PAD_COLS,
				       posy + FACEPLATE_CHROME_TOP_ROWS);
			r.x1 = x1;
			r.y1 = y1;
			r.x2 = x1 + fw;
			r.y2 = y1 + fh;
			if (prev)
				merge_damage(bb, &r);
			else
				add_damage(bb, &r);
			prev = DAMAGE_MERGE_LEN;
		}
	}

	/* Chrome is redrawn every frame and sits outside the terminal cell grid. */
	r.x1 = 0;
	r.y1 = 0;
	r.x2 = bb->sw;
	r.y2 = bb->off_y + FACEPLATE_CHROME_TOP_ROWS * fh;
	add_damage(bb, &r);

	r.y1 = bb->off_y + (FACEPLATE_CHROME_TOP_ROWS + txt->rows) * fh;
	r.y2 = bb->sh;
	add_damage(bb, &r);
}
static int bbulk_render(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int ret = 0;

	// log_debug("bbulk, redraw %d cells", bb->requests);
	if (display_supports_damage(txt->disp)) {
		bbulk_compute_damage(txt);
		display_set_damage(txt->disp, bb->damage_rect_len, bb->damage_rects);
	}
	return ret;
}

static int bbulk_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	int i;

	bb->requests = 0;
	bb->damage_rect_len = 0;

	/*
	 * if default colors have changed, or we switch from a dirty screen,
	 * redraw completely the next 2 frames.
	 */
	if (memcmp(&bb->attr, attr, sizeof(*attr)) || display_need_redraw(txt->disp))
		bb->redraw = 2;

	bb->attr = *attr;

	if (bb->redraw) {
		display_clear(txt->disp, FACEPLATE_PAGE_R, FACEPLATE_PAGE_G, FACEPLATE_PAGE_B);
		for (i = 0; i < bb->cell_count; i++)
			damage_cell(bb, i);
	} else if (display_has_damage(txt->disp)) {
		log_debug("Carry over damage from previous frame");
		for (i = 0; i < bb->cell_count; i++) {
			if (bb->cell_flags[i].damaged)
				bb->cells[i].ch = ID_DAMAGED;
		}
	}
	if (bb->redraw)
		bb->redraw--;

	return 0;
}

struct kmscon_text_ops kmscon_text_bbulk_ops = {
	.name = "bbulk",
	.owner = NULL,
	.init = bbulk_init,
	.destroy = bbulk_destroy,
	.set = bbulk_set,
	.unset = bbulk_unset,
	.resize = bbulk_resize,
	.rotate = bbulk_rotate,
	.prepare = bbulk_prepare,
	.draw = bbulk_draw,
	.draw_status = bbulk_draw_status,
	.draw_pointer = bbulk_draw_pointer,
	.render = bbulk_render,
	.abort = NULL,
};
