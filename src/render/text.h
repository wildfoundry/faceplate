/*
 * kmscon - Text Renderer
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

/*
 * Text Renderer
 * The Text-Renderer subsystem provides a simple way to draw text into a
 * framebuffer. The system is modular and several different backends are
 * available that can be used.
 */

#ifndef KMSCON_TEXT_H
#define KMSCON_TEXT_H

#include <errno.h>
#include <libtsm.h>
#include <stdlib.h>
#include "font/font.h"
#include "shl/module.h"
#include "video/video.h"

/* text renderer */

enum Orientation {
	OR_NORMAL = 0,	// 0 Degree
	OR_RIGHT,	// 90 Degree
	OR_UPSIDE_DOWN, // 180 Degree
	OR_LEFT,	// 270 Degree
};

struct kmscon_text;
struct kmscon_text_ops;

struct kmscon_text {
	unsigned long ref;
	struct shl_register_record *record;
	const struct kmscon_text_ops *ops;
	void *data;

	struct kmscon_font *font;
	struct display *disp;
	unsigned int cols;
	unsigned int rows;
	unsigned int max_cols;
	unsigned int max_rows;
	bool rendering;
	enum Orientation orientation;
	bool blinking;
	char status_line[160];
	bool status_visible;
	char detail_line[192];
	char chrome_title[64];
	char chrome_context[96];
	struct video_buffer *chrome_logo;
	/* Idle = login face; active = compact right rail. */
	bool chrome_idle;
	char identity_hostname[64];
	char identity_status[24]; /* Online / Offline / Unknown — lead signal */
	char identity_supporting[160]; /* labeled facts: IP … | OS … */
	char identity_secondary[160];
	char identity_compact[160];
	bool connection_alert;
	bool rauc_alert;
};

/*
 * Layout (OR_NORMAL) — terminal card is terminal-only:
 *   [ brand band on page: large logo LEFT | Online+host+facts RIGHT ]
 *   [ small gap ]
 *   [ black terminal card: pad + pty cells + pad ]
 *   [ gap ]
 *   [ quiet footer: Uptime | Serial (+ RAUC if unhealthy) ]
 *   [ safe margin / overscan ]
 *
 * TOP_ROWS = brand + gap + card top pad
 * BOTTOM_ROWS = card bottom pad + footer gap + secondary
 */
#define FACEPLATE_CHROME_BRAND_ROWS 3
#define FACEPLATE_CHROME_BRAND_GAP_ROWS 1
#define FACEPLATE_CHROME_PAD_COLS 2
#define FACEPLATE_CHROME_PAD_ROWS 1
/* Empty page rows between card bottom and footer. */
#define FACEPLATE_CHROME_FOOTER_GAP_ROWS 2
#define FACEPLATE_CHROME_CARD_Y0 \
	(FACEPLATE_CHROME_BRAND_ROWS + FACEPLATE_CHROME_BRAND_GAP_ROWS)
#define FACEPLATE_CHROME_TOP_ROWS (FACEPLATE_CHROME_CARD_Y0 + FACEPLATE_CHROME_PAD_ROWS)
#define FACEPLATE_CHROME_BOTTOM_ROWS \
	(FACEPLATE_CHROME_PAD_ROWS + FACEPLATE_CHROME_FOOTER_GAP_ROWS + 1)
/* Page gutters outside the card (total cells subtracted from width). */
#define FACEPLATE_CHROME_HORIZONTAL_CELLS 14
/* Thin card outline in pixels — frames, does not glow. */
#define FACEPLATE_CHROME_BORDER_PX 1
/* Page / card colours (RGB). */
#define FACEPLATE_PAGE_R 11
#define FACEPLATE_PAGE_G 18
#define FACEPLATE_PAGE_B 32
#define FACEPLATE_CARD_R 0
#define FACEPLATE_CARD_G 0
#define FACEPLATE_CARD_B 0
/* Quiet slate outline — define the card, not a neon frame. */
#define FACEPLATE_CARD_BORDER_R 51
#define FACEPLATE_CARD_BORDER_G 65
#define FACEPLATE_CARD_BORDER_B 85
/* Calm / muted page chrome. */
#define FACEPLATE_STATUS_FG_R 226
#define FACEPLATE_STATUS_FG_G 232
#define FACEPLATE_STATUS_FG_B 240
#define FACEPLATE_MUTED_FG_R 148
#define FACEPLATE_MUTED_FG_G 163
#define FACEPLATE_MUTED_FG_B 184
/* Online (calm but strong) vs Offline/RAUC alert. */
#define FACEPLATE_ONLINE_FG_R 110
#define FACEPLATE_ONLINE_FG_G 231
#define FACEPLATE_ONLINE_FG_B 183
#define FACEPLATE_ALERT_FG_R 251
#define FACEPLATE_ALERT_FG_G 146
#define FACEPLATE_ALERT_FG_B 60
#define FACEPLATE_HOST_FG_R 248
#define FACEPLATE_HOST_FG_G 250
#define FACEPLATE_HOST_FG_B 252
/* Extra space below footer so TV overscan does not crop status. */
#define FACEPLATE_CHROME_SAFE_MARGIN_ROWS 8

/* Legacy aliases used by older chrome math comments. */
#define FACEPLATE_CHROME_LOGO_ROWS FACEPLATE_CHROME_BRAND_ROWS
#define FACEPLATE_CHROME_IDENTITY_ROWS 0

struct kmscon_cursor {
	struct tsm_screen_cell cell;
	unsigned int x;
	unsigned int y;
	bool visible;
};

struct kmscon_text_ops {
	const char *name;
	struct shl_module *owner;
	int (*init)(struct kmscon_text *txt);
	void (*destroy)(struct kmscon_text *txt);
	int (*set)(struct kmscon_text *txt);
	void (*unset)(struct kmscon_text *txt);
	void (*resize)(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
	int (*rotate)(struct kmscon_text *txt, enum Orientation orientation);
	int (*prepare)(struct kmscon_text *txt, struct tsm_screen_attr *attr);
	int (*draw)(struct kmscon_text *txt, const struct tsm_screen_cell *cells,
		    struct kmscon_cursor *cursor);
	int (*draw_status)(struct kmscon_text *txt, const char *line);
	int (*draw_pointer)(struct kmscon_text *txt, unsigned int x, unsigned int y);
	int (*render)(struct kmscon_text *txt);
	void (*abort)(struct kmscon_text *txt);
};

#define FONT_WIDTH(txt) ((txt)->font->attr.width)
#define FONT_HEIGHT(txt) ((txt)->font->attr.height)

int kmscon_text_register(const struct kmscon_text_ops *ops);
void kmscon_text_unregister(const char *name);

int kmscon_text_new(struct kmscon_text **out, const char *backend, const char *rotate);
void kmscon_text_ref(struct kmscon_text *txt);
void kmscon_text_unref(struct kmscon_text *txt);

int kmscon_text_set(struct kmscon_text *txt, struct kmscon_font *font, struct display *disp);
void kmscon_text_unset(struct kmscon_text *txt);
unsigned int kmscon_text_get_cols(struct kmscon_text *txt);
unsigned int kmscon_text_get_rows(struct kmscon_text *txt);

enum Orientation kmscon_text_get_orientation(struct kmscon_text *txt);
void kmscon_text_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
int kmscon_text_rotate(struct kmscon_text *txt, enum Orientation orientation);

int kmscon_text_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr, bool blinking);
int kmscon_text_draw(struct kmscon_text *txt, struct tsm_screen *con, bool cursor_blink);
void kmscon_text_set_status(struct kmscon_text *txt, const char *line);
void kmscon_text_set_detail(struct kmscon_text *txt, const char *line);
void kmscon_text_set_identity(struct kmscon_text *txt, const char *title, const char *context,
			      const char *logo_path);
void kmscon_text_set_chrome_idle(struct kmscon_text *txt, bool idle);
void kmscon_text_set_faceplate_status(struct kmscon_text *txt, const char *hostname,
				      const char *status, const char *supporting,
				      const char *secondary, const char *compact,
				      bool connection_alert, bool rauc_alert);
int kmscon_text_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y);
int kmscon_text_render(struct kmscon_text *txt);
void kmscon_text_abort(struct kmscon_text *txt);

/* modularized backends */

extern struct kmscon_text_ops kmscon_text_bbulk_ops;
extern struct kmscon_text_ops kmscon_text_gltex_ops;

#endif /* KMSCON_TEXT_H */
