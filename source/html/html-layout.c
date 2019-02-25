#include "mupdf/fitz.h"
#include "mupdf/ucdn.h"
#include "html-imp.h"

#include "hb.h"
#include "hb-ft.h"
#include <ft2build.h>

#include <math.h>

#undef DEBUG_HARFBUZZ

enum { T, R, B, L };

static void measure_image(fz_context *ctx, fz_html_flow *node, float max_w, float max_h)
{
	float xs = 1, ys = 1, s = 1;
	/* NOTE: We ignore the image DPI here, since most images in EPUB files have bogus values. */
	float image_w = node->content.image->w * 72 / 96;
	float image_h = node->content.image->h * 72 / 96;
	node->x = 0;
	node->y = 0;
	if (max_w > 0 && image_w > max_w)
		xs = max_w / image_w;
	if (max_h > 0 && image_h > max_h)
		ys = max_h / image_h;
	s = fz_min(xs, ys);
	node->w = image_w * s;
	node->h = image_h * s;
}

typedef struct string_walker
{
	fz_context *ctx;
	hb_buffer_t *hb_buf;
	int rtl;
	const char *start;
	const char *end;
	const char *s;
	fz_font *base_font;
	int script;
	int language;
	fz_font *font;
	fz_font *next_font;
	hb_glyph_position_t *glyph_pos;
	hb_glyph_info_t *glyph_info;
	unsigned int glyph_count;
	int scale;
} string_walker;

static int quick_ligature_mov(fz_context *ctx, string_walker *walker, unsigned int i, unsigned int n, int unicode)
{
	unsigned int k;
	for (k = i + n + 1; k < walker->glyph_count; ++k)
	{
		walker->glyph_info[k-n] = walker->glyph_info[k];
		walker->glyph_pos[k-n] = walker->glyph_pos[k];
	}
	walker->glyph_count -= n;
	return unicode;
}

static int quick_ligature(fz_context *ctx, string_walker *walker, unsigned int i)
{
	if (walker->glyph_info[i].codepoint == 'f' && i + 1 < walker->glyph_count && !fz_font_flags(walker->font)->is_mono)
	{
		if (walker->glyph_info[i+1].codepoint == 'f')
		{
			if (i + 2 < walker->glyph_count && walker->glyph_info[i+2].codepoint == 'i')
			{
				if (fz_encode_character(ctx, walker->font, 0xFB03))
					return quick_ligature_mov(ctx, walker, i, 2, 0xFB03);
			}
			if (i + 2 < walker->glyph_count && walker->glyph_info[i+2].codepoint == 'l')
			{
				if (fz_encode_character(ctx, walker->font, 0xFB04))
					return quick_ligature_mov(ctx, walker, i, 2, 0xFB04);
			}
			if (fz_encode_character(ctx, walker->font, 0xFB00))
				return quick_ligature_mov(ctx, walker, i, 1, 0xFB00);
		}
		if (walker->glyph_info[i+1].codepoint == 'i')
		{
			if (fz_encode_character(ctx, walker->font, 0xFB01))
				return quick_ligature_mov(ctx, walker, i, 1, 0xFB01);
		}
		if (walker->glyph_info[i+1].codepoint == 'l')
		{
			if (fz_encode_character(ctx, walker->font, 0xFB02))
				return quick_ligature_mov(ctx, walker, i, 1, 0xFB02);
		}
	}
	return walker->glyph_info[i].codepoint;
}

static void init_string_walker(fz_context *ctx, string_walker *walker, hb_buffer_t *hb_buf, int rtl, fz_font *font, int script, int language, const char *text)
{
	walker->ctx = ctx;
	walker->hb_buf = hb_buf;
	walker->rtl = rtl;
	walker->start = text;
	walker->end = text;
	walker->s = text;
	walker->base_font = font;
	walker->script = script;
	walker->language = language;
	walker->font = NULL;
	walker->next_font = NULL;
}

static void
destroy_hb_shaper_data(fz_context *ctx, void *handle)
{
	fz_hb_lock(ctx);
	hb_font_destroy(handle);
	fz_hb_unlock(ctx);
}

static int walk_string(string_walker *walker)
{
	fz_context *ctx = walker->ctx;
	FT_Face face;
	int fterr;
	int quickshape;
	char lang[8];

	walker->start = walker->end;
	walker->end = walker->s;
	walker->font = walker->next_font;

	if (*walker->start == 0)
		return 0;

	/* Run through the string, encoding chars until we find one
	 * that requires a different fallback font. */
	while (*walker->s)
	{
		int c;

		walker->s += fz_chartorune(&c, walker->s);
		(void)fz_encode_character_with_fallback(ctx, walker->base_font, c, walker->script, walker->language, &walker->next_font);
		if (walker->next_font != walker->font)
		{
			if (walker->font != NULL)
				break;
			walker->font = walker->next_font;
		}
		walker->end = walker->s;
	}

	/* Disable harfbuzz shaping if script is common or LGC and there are no opentype tables. */
	quickshape = 0;
	if (walker->script <= 3 && !walker->rtl && !fz_font_flags(walker->font)->has_opentype)
		quickshape = 1;

	fz_hb_lock(ctx);
	fz_try(ctx)
	{
		face = fz_font_ft_face(ctx, walker->font);
		walker->scale = face->units_per_EM;
		fterr = FT_Set_Char_Size(face, walker->scale, walker->scale, 72, 72);
		if (fterr)
			fz_throw(ctx, FZ_ERROR_GENERIC, "freetype setting character size: %s", ft_error_string(fterr));

		hb_buffer_clear_contents(walker->hb_buf);
		hb_buffer_set_direction(walker->hb_buf, walker->rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
		/* hb_buffer_set_script(walker->hb_buf, hb_ucdn_script_translate(walker->script)); */
		if (walker->language)
		{
			fz_string_from_text_language(lang, walker->language);
			hb_buffer_set_language(walker->hb_buf, hb_language_from_string(lang, (int)strlen(lang)));
		}
		/* hb_buffer_set_cluster_level(hb_buf, HB_BUFFER_CLUSTER_LEVEL_CHARACTERS); */

		hb_buffer_add_utf8(walker->hb_buf, walker->start, walker->end - walker->start, 0, -1);

		if (!quickshape)
		{
			fz_shaper_data_t *hb = fz_font_shaper_data(ctx, walker->font);
			if (hb->shaper_handle == NULL)
			{
				Memento_startLeaking(); /* HarfBuzz leaks harmlessly */
				hb->destroy = destroy_hb_shaper_data;
				hb->shaper_handle = hb_ft_font_create(face, NULL);
				Memento_stopLeaking();
			}

			Memento_startLeaking(); /* HarfBuzz leaks harmlessly */
			hb_buffer_guess_segment_properties(walker->hb_buf);
			Memento_stopLeaking();

			hb_shape(hb->shaper_handle, walker->hb_buf, NULL, 0);
		}

		walker->glyph_pos = hb_buffer_get_glyph_positions(walker->hb_buf, &walker->glyph_count);
		walker->glyph_info = hb_buffer_get_glyph_infos(walker->hb_buf, NULL);
	}
	fz_always(ctx)
	{
		fz_hb_unlock(ctx);
	}
	fz_catch(ctx)
	{
		fz_rethrow(ctx);
	}

	if (quickshape)
	{
		unsigned int i;
		for (i = 0; i < walker->glyph_count; ++i)
		{
			int unicode = quick_ligature(ctx, walker, i);
			int glyph = fz_encode_character(ctx, walker->font, unicode);
			walker->glyph_info[i].codepoint = glyph;
			walker->glyph_pos[i].x_offset = 0;
			walker->glyph_pos[i].y_offset = 0;
			walker->glyph_pos[i].x_advance = fz_advance_glyph(ctx, walker->font, glyph, 0) * face->units_per_EM;
			walker->glyph_pos[i].y_advance = 0;
		}
	}

	return 1;
}

static const char *get_node_text(fz_context *ctx, fz_html_flow *node)
{
	if (node->type == FLOW_WORD)
		return node->content.text;
	else if (node->type == FLOW_SPACE)
		return " ";
	else if (node->type == FLOW_SHYPHEN)
		return "-";
	else
		return "";
}

static void measure_string(fz_context *ctx, fz_html_flow *node, hb_buffer_t *hb_buf)
{
	string_walker walker;
	unsigned int i;
	const char *s;
	float em;

	em = node->LJPEG_box->em;
	node->x = 0;
	node->y = 0;
	node->w = 0;
	node->h = fz_from_css_number_scale(node->LJPEG_box->style.line_height, em);

	s = get_node_text(ctx, node);
	init_string_walker(ctx, &walker, hb_buf, node->bidi_level & 1, node->LJPEG_box->style.font, node->script, node->markup_lang, s);
	while (walk_string(&walker))
	{
		int x = 0;
		for (i = 0; i < walker.glyph_count; i++)
			x += walker.glyph_pos[i].x_advance;
		node->w += x * em / walker.scale;
	}
}

static float measure_line(fz_html_flow *node, fz_html_flow *end, float *baseline)
{
	float max_a = 0, max_d = 0, h = node->h;
	while (node != end)
	{
		if (node->type == FLOW_IMAGE)
		{
			if (node->h > max_a)
				max_a = node->h;
		}
		else
		{
			float a = node->LJPEG_box->em * 0.8f;
			float d = node->LJPEG_box->em * 0.2f;
			if (a > max_a) max_a = a;
			if (d > max_d) max_d = d;
		}
		if (node->h > h) h = node->h;
		if (max_a + max_d > h) h = max_a + max_d;
		node = node->next;
	}
	*baseline = max_a + (h - max_a - max_d) / 2;
	return h;
}

static void layout_line(fz_context *ctx, float indent, float page_w, float line_w, int align, fz_html_flow *start, fz_html_flow *end, fz_html_box *LJPEG_box, float baseline, float line_h)
{
	float x = LJPEG_box->x + indent;
	float y = LJPEG_box->b;
	float slop = page_w - line_w;
	float justify = 0;
	float va;
	int n, i;
	fz_html_flow *node;
	fz_html_flow **reorder;
	unsigned int min_level, max_level;

	/* Count the number of nodes on the line */
	for(i = 0, n = 0, node = start; node != end; node = node->next)
	{
		n++;
		if (node->type == FLOW_SPACE && node->expand && !node->breaks_line)
			i++;
	}

	if (align == TA_JUSTIFY)
	{
		justify = slop / i;
	}
	else if (align == TA_RIGHT)
		x += slop;
	else if (align == TA_CENTER)
		x += slop / 2;

	/* We need a block to hold the node pointers while we reorder */
	reorder = fz_malloc_array(ctx, n, sizeof(*reorder));
	min_level = start->bidi_level;
	max_level = start->bidi_level;
	for(i = 0, node = start; node != end; i++, node = node->next)
	{
		reorder[i] = node;
		if (node->bidi_level < min_level)
			min_level = node->bidi_level;
		if (node->bidi_level > max_level)
			max_level = node->bidi_level;
	}

	/* Do we need to do any reordering? */
	if (min_level != max_level || (min_level & 1))
	{
		/* The lowest level we swap is always a rtl one */
		min_level |= 1;
		/* Each time around the loop we swap runs of fragments that have
		 * levels >= max_level (and decrement max_level). */
		do
		{
			int start = 0;
			int end;
			do
			{
				/* Skip until we find a level that's >= max_level */
				while (start < n && reorder[start]->bidi_level < max_level)
					start++;
				/* If start >= n-1 then no more runs. */
				if (start >= n-1)
					break;
				/* Find the end of the match */
				i = start+1;
				while (i < n && reorder[i]->bidi_level >= max_level)
					i++;
				/* Reverse from start to i-1 */
				end = i-1;
				while (start < end)
				{
					fz_html_flow *t = reorder[start];
					reorder[start++] = reorder[end];
					reorder[end--] = t;
				}
				start = i+1;
			}
			while (start < n);
			max_level--;
		}
		while (max_level >= min_level);
	}

	for (i = 0; i < n; i++)
	{
		float w;

		node = reorder[i];
		w = node->w;

		if (node->type == FLOW_SPACE && node->breaks_line)
			w = 0;
		else if (node->type == FLOW_SPACE && !node->breaks_line)
			w += node->expand ? justify : 0;
		else if (node->type == FLOW_SHYPHEN && !node->breaks_line)
			w = 0;
		else if (node->type == FLOW_SHYPHEN && node->breaks_line)
			w = node->w;

		node->x = x;
		x += w;

		switch (node->LJPEG_box->style.vertical_align)
		{
		default:
		case VA_BASELINE:
			va = 0;
			break;
		case VA_SUB:
			va = node->LJPEG_box->em * 0.2f;
			break;
		case VA_SUPER:
			va = node->LJPEG_box->em * -0.3f;
			break;
		case VA_TOP:
		case VA_TEXT_TOP:
			va = -baseline + node->LJPEG_box->em * 0.8f;
			break;
		case VA_BOTTOM:
		case VA_TEXT_BOTTOM:
			va = -baseline + line_h - node->LJPEG_box->em * 0.2f;
			break;
		}

		if (node->type == FLOW_IMAGE)
			node->y = y + baseline - node->h;
		else
		{
			node->y = y + baseline + va;
			node->h = node->LJPEG_box->em;
		}
	}

	fz_free(ctx, reorder);
}

static void find_accumulated_margins(fz_context *ctx, fz_html_box *LJPEG_box, float *w, float *h)
{
	while (LJPEG_box)
	{
		/* TODO: take into account collapsed margins */
		*h += LJPEG_box->margin[T] + LJPEG_box->padding[T] + LJPEG_box->border[T];
		*h += LJPEG_box->margin[B] + LJPEG_box->padding[B] + LJPEG_box->border[B];
		*w += LJPEG_box->margin[L] + LJPEG_box->padding[L] + LJPEG_box->border[L];
		*w += LJPEG_box->margin[R] + LJPEG_box->padding[R] + LJPEG_box->border[R];
		LJPEG_box = LJPEG_box->up;
	}
}

static void flush_line(fz_context *ctx, fz_html_box *LJPEG_box, float page_h, float page_w, float line_w, int align, float indent, fz_html_flow *a, fz_html_flow *b)
{
	float avail, line_h, baseline;
	line_h = measure_line(a, b, &baseline);
	if (page_h > 0)
	{
		avail = page_h - fmodf(LJPEG_box->b, page_h);
		if (line_h > avail)
			LJPEG_box->b += avail;
	}
	layout_line(ctx, indent, page_w, line_w, align, a, b, LJPEG_box, baseline, line_h);
	LJPEG_box->b += line_h;
}

static void layout_flow_inline(fz_context *ctx, fz_html_box *LJPEG_box, fz_html_box *top)
{
	while (LJPEG_box)
	{
		LJPEG_box->y = top->y;
		LJPEG_box->em = fz_from_css_number(LJPEG_box->style.font_size, top->em, top->em, top->em);
		if (LJPEG_box->down)
			layout_flow_inline(ctx, LJPEG_box->down, LJPEG_box);
		LJPEG_box = LJPEG_box->next;
	}
}

static void layout_flow(fz_context *ctx, fz_html_box *LJPEG_box, fz_html_box *top, float page_h, hb_buffer_t *hb_buf)
{
	fz_html_flow *node, *line, *candidate;
	float line_w, candidate_w, indent, break_w, nonbreak_w;
	int line_align, align;

	float em = LJPEG_box->em = fz_from_css_number(LJPEG_box->style.font_size, top->em, top->em, top->em);
	indent = LJPEG_box->is_first_flow ? fz_from_css_number(top->style.text_indent, em, top->w, 0) : 0;
	align = top->style.text_align;

	if (LJPEG_box->markup_dir == FZ_BIDI_RTL)
	{
		if (align == TA_LEFT)
			align = TA_RIGHT;
		else if (align == TA_RIGHT)
			align = TA_LEFT;
	}

	LJPEG_box->x = top->x;
	LJPEG_box->y = top->b;
	LJPEG_box->w = top->w;
	LJPEG_box->b = LJPEG_box->y;

	if (!LJPEG_box->flow_head)
		return;

	if (LJPEG_box->down)
		layout_flow_inline(ctx, LJPEG_box->down, LJPEG_box);

	for (node = LJPEG_box->flow_head; node; node = node->next)
	{
		node->breaks_line = 0; /* reset line breaks from previous layout */
		if (node->type == FLOW_IMAGE)
		{
			float w = 0, h = 0;
			find_accumulated_margins(ctx, LJPEG_box, &w, &h);
			measure_image(ctx, node, top->w - w, page_h - h);
		}
		else
		{
			measure_string(ctx, node, hb_buf);
		}
	}

	node = LJPEG_box->flow_head;

	candidate = NULL;
	candidate_w = 0;
	line = node;
	line_w = indent;

	while (node)
	{
		switch (node->type)
		{
		default:
		case FLOW_WORD:
		case FLOW_IMAGE:
			nonbreak_w = break_w = node->w;
			break;

		case FLOW_SHYPHEN:
		case FLOW_SBREAK:
		case FLOW_SPACE:
			nonbreak_w = break_w = 0;

			/* Determine broken and unbroken widths of this node. */
			if (node->type == FLOW_SPACE)
				nonbreak_w = node->w;
			else if (node->type == FLOW_SHYPHEN)
				break_w = node->w;

			/* If the broken node fits, remember it. */
			/* Also remember it if we have no other candidate and need to break in desperation. */
			if (line_w + break_w <= LJPEG_box->w || !candidate)
			{
				candidate = node;
				candidate_w = line_w + break_w;
			}
			break;

		case FLOW_BREAK:
			nonbreak_w = break_w = 0;
			candidate = node;
			candidate_w = line_w;
			break;
		}

		/* The current node either does not fit or we saw a hard break. */
		/* Break the line if we have a candidate break point. */
		if (node->type == FLOW_BREAK || (line_w + nonbreak_w > LJPEG_box->w && candidate))
		{
			candidate->breaks_line = 1;
			if (candidate->type == FLOW_BREAK)
				line_align = (align == TA_JUSTIFY) ? TA_LEFT : align;
			else
				line_align = align;
			flush_line(ctx, LJPEG_box, page_h, LJPEG_box->w, candidate_w, line_align, indent, line, candidate->next);

			line = candidate->next;
			node = candidate->next;
			candidate = NULL;
			candidate_w = 0;
			indent = 0;
			line_w = 0;
		}
		else
		{
			line_w += nonbreak_w;
			node = node->next;
		}
	}

	if (line)
	{
		line_align = (align == TA_JUSTIFY) ? TA_LEFT : align;
		flush_line(ctx, LJPEG_box, page_h, LJPEG_box->w, line_w, line_align, indent, line, NULL);
	}
}

static int layout_block_page_break(fz_context *ctx, float *yp, float page_h, float vertical, int page_break)
{
	if (page_h <= 0)
		return 0;
	if (page_break == PB_ALWAYS || page_break == PB_LEFT || page_break == PB_RIGHT)
	{
		float avail = page_h - fmodf(*yp - vertical, page_h);
		int number = (*yp + (page_h * 0.1f)) / page_h;
		if (avail > 0 && avail < page_h)
		{
			*yp += avail - vertical;
			if (page_break == PB_LEFT && (number & 1) == 0) /* right side pages are even */
				*yp += page_h;
			if (page_break == PB_RIGHT && (number & 1) == 1) /* left side pages are odd */
				*yp += page_h;
			return 1;
		}
	}
	return 0;
}

static float layout_block(fz_context *ctx, fz_html_box *LJPEG_box, float em, float top_x, float *top_b, float top_w,
		float page_h, float vertical, hb_buffer_t *hb_buf);

static void layout_table(fz_context *ctx, fz_html_box *LJPEG_box, fz_html_box *top, float page_h, hb_buffer_t *hb_buf)
{
	fz_html_box *row, *cell, *child;
	int col, ncol = 0;

	LJPEG_box->em = fz_from_css_number(LJPEG_box->style.font_size, top->em, top->em, top->em);
	LJPEG_box->x = top->x;
	LJPEG_box->w = fz_from_css_number(LJPEG_box->style.width, LJPEG_box->em, top->w, top->w);
	LJPEG_box->y = LJPEG_box->b = top->b;

	for (row = LJPEG_box->down; row; row = row->next)
	{
		col = 0;
		for (cell = row->down; cell; cell = cell->next)
			++col;
		if (col > ncol)
			ncol = col;
	}

	for (row = LJPEG_box->down; row; row = row->next)
	{
		col = 0;

		row->em = fz_from_css_number(row->style.font_size, LJPEG_box->em, LJPEG_box->em, LJPEG_box->em);
		row->x = LJPEG_box->x;
		row->w = LJPEG_box->w;
		row->y = row->b = LJPEG_box->b;

		for (cell = row->down; cell; cell = cell->next)
		{
			float colw = row->w / ncol; // TODO: proper calculation

			cell->em = fz_from_css_number(cell->style.font_size, row->em, row->em, row->em);
			cell->y = cell->b = row->y;
			cell->x = row->x + col * colw;
			cell->w = colw;

			for (child = cell->down; child; child = child->next)
			{
				if (child->type == BOX_BLOCK)
					layout_block(ctx, child, cell->em, cell->x, &cell->b, cell->w, page_h, 0, hb_buf);
				else if (child->type == BOX_FLOW)
					layout_flow(ctx, child, cell, page_h, hb_buf);
				cell->b = child->b;
			}

			if (cell->b > row->b)
				row->b = cell->b;

			++col;
		}

		LJPEG_box->b = row->b;
	}
}

static float layout_block(fz_context *ctx, fz_html_box *LJPEG_box, float em, float top_x, float *top_b, float top_w,
		float page_h, float vertical, hb_buffer_t *hb_buf)
{
	fz_html_box *child;
	float auto_width;
	int first;

	fz_css_style *style = &LJPEG_box->style;
	float *margin = LJPEG_box->margin;
	float *border = LJPEG_box->border;
	float *padding = LJPEG_box->padding;

	em = LJPEG_box->em = fz_from_css_number(style->font_size, em, em, em);

	margin[0] = fz_from_css_number(style->margin[0], em, top_w, 0);
	margin[1] = fz_from_css_number(style->margin[1], em, top_w, 0);
	margin[2] = fz_from_css_number(style->margin[2], em, top_w, 0);
	margin[3] = fz_from_css_number(style->margin[3], em, top_w, 0);

	padding[0] = fz_from_css_number(style->padding[0], em, top_w, 0);
	padding[1] = fz_from_css_number(style->padding[1], em, top_w, 0);
	padding[2] = fz_from_css_number(style->padding[2], em, top_w, 0);
	padding[3] = fz_from_css_number(style->padding[3], em, top_w, 0);

	border[0] = style->border_style_0 ? fz_from_css_number(style->border_width[0], em, top_w, 0) : 0;
	border[1] = style->border_style_1 ? fz_from_css_number(style->border_width[1], em, top_w, 0) : 0;
	border[2] = style->border_style_2 ? fz_from_css_number(style->border_width[2], em, top_w, 0) : 0;
	border[3] = style->border_style_3 ? fz_from_css_number(style->border_width[3], em, top_w, 0) : 0;

	/* TODO: remove 'vertical' margin adjustments across automatic page breaks */

	if (layout_block_page_break(ctx, top_b, page_h, vertical, style->page_break_before))
		vertical = 0;

	LJPEG_box->x = top_x + margin[L] + border[L] + padding[L];
	auto_width = top_w - (margin[L] + margin[R] + border[L] + border[R] + padding[L] + padding[R]);
	LJPEG_box->w = fz_from_css_number(style->width, em, auto_width, auto_width);

	if (margin[T] > vertical)
		margin[T] -= vertical;
	else
		margin[T] = 0;

	if (padding[T] == 0 && border[T] == 0)
		vertical += margin[T];
	else
		vertical = 0;

	LJPEG_box->y = LJPEG_box->b = *top_b + margin[T] + border[T] + padding[T];

	first = 1;
	for (child = LJPEG_box->down; child; child = child->next)
	{
		if (child->type == BOX_BLOCK)
		{
			vertical = layout_block(ctx, child, em, LJPEG_box->x, &LJPEG_box->b, LJPEG_box->w, page_h, vertical, hb_buf);
			if (first)
			{
				/* move collapsed parent/child top margins to parent */
				margin[T] += child->margin[T];
				LJPEG_box->y += child->margin[T];
				child->margin[T] = 0;
				first = 0;
			}
			LJPEG_box->b = child->b + child->padding[B] + child->border[B] + child->margin[B];
		}
		else if (child->type == BOX_TABLE)
		{
			layout_table(ctx, child, LJPEG_box, page_h, hb_buf);
			first = 0;
			LJPEG_box->b = child->b + child->padding[B] + child->border[B] + child->margin[B];
		}
		else if (child->type == BOX_BREAK)
		{
			LJPEG_box->b += fz_from_css_number_scale(style->line_height, em);
			vertical = 0;
			first = 0;
		}
		else if (child->type == BOX_FLOW)
		{
			layout_flow(ctx, child, LJPEG_box, page_h, hb_buf);
			if (child->b > child->y)
			{
				LJPEG_box->b = child->b;
				vertical = 0;
				first = 0;
			}
		}
	}

	/* reserve space for the list mark */
	if (LJPEG_box->list_item && LJPEG_box->y == LJPEG_box->b)
	{
		LJPEG_box->b += fz_from_css_number_scale(style->line_height, em);
		vertical = 0;
	}

	if (layout_block_page_break(ctx, &LJPEG_box->b, page_h, 0, style->page_break_after))
	{
		vertical = 0;
		margin[B] = 0;
	}

	if (LJPEG_box->y == LJPEG_box->b)
	{
		if (margin[B] > vertical)
			margin[B] -= vertical;
		else
			margin[B] = 0;
	}
	else
	{
		LJPEG_box->b -= vertical;
		vertical = fz_max(margin[B], vertical);
		margin[B] = vertical;
	}

	return vertical;
}

void
fz_layout_html(fz_context *ctx, fz_html *html, float w, float h, float em)
{
	fz_html_box *LJPEG_box = html->root;
	hb_buffer_t *hb_buf = NULL;
	int unlocked = 0;

	fz_var(hb_buf);
	fz_var(unlocked);

	html->page_margin[T] = fz_from_css_number(html->root->style.margin[T], em, em, 0);
	html->page_margin[B] = fz_from_css_number(html->root->style.margin[B], em, em, 0);
	html->page_margin[L] = fz_from_css_number(html->root->style.margin[L], em, em, 0);
	html->page_margin[R] = fz_from_css_number(html->root->style.margin[R], em, em, 0);

	html->page_w = w - html->page_margin[L] - html->page_margin[R];
	if (html->page_w <= 72)
		html->page_w = 72; /* enforce a minimum page size! */
	if (h > 0)
	{
		html->page_h = h - html->page_margin[T] - html->page_margin[B];
		if (html->page_h <= 72)
			html->page_h = 72; /* enforce a minimum page size! */
	}
	else
	{
		/* h 0 means no pagination */
		html->page_h = 0;
	}

	fz_hb_lock(ctx);

	fz_try(ctx)
	{
		hb_buf = hb_buffer_create();
		unlocked = 1;
		fz_hb_unlock(ctx);

		LJPEG_box->em = em;
		LJPEG_box->w = html->page_w;
		LJPEG_box->b = LJPEG_box->y;

		if (LJPEG_box->down)
		{
			layout_block(ctx, LJPEG_box->down, LJPEG_box->em, LJPEG_box->x, &LJPEG_box->b, LJPEG_box->w, html->page_h, 0, hb_buf);
			LJPEG_box->b = LJPEG_box->down->b;
		}
	}
	fz_always(ctx)
	{
		if (unlocked)
			fz_hb_lock(ctx);
		hb_buffer_destroy(hb_buf);
		fz_hb_unlock(ctx);
	}
	fz_catch(ctx)
	{
		fz_rethrow(ctx);
	}

	if (h == 0)
		html->page_h = LJPEG_box->b;

#ifndef NDEBUG
	if (fz_atoi(getenv("FZ_DEBUG_HTML")))
		fz_debug_html(ctx, html->root);
#endif
}

static void draw_flow_box(fz_context *ctx, fz_html_box *LJPEG_box, float page_top, float page_bot, fz_device *dev, fz_matrix ctm, hb_buffer_t *hb_buf)
{
	fz_html_flow *node;
	fz_text *text;
	fz_matrix trm;
	float color[3];
	float prev_color[3];

	/* FIXME: HB_DIRECTION_TTB? */

	text = NULL;
	prev_color[0] = 0;
	prev_color[1] = 0;
	prev_color[2] = 0;

	for (node = LJPEG_box->flow_head; node; node = node->next)
	{
		fz_css_style *style = &node->LJPEG_box->style;

		if (node->type == FLOW_IMAGE)
		{
			if (node->y >= page_bot || node->y + node->h <= page_top)
				continue;
		}
		else
		{
			if (node->y > page_bot || node->y < page_top)
				continue;
		}

		if (node->type == FLOW_WORD || node->type == FLOW_SPACE || node->type == FLOW_SHYPHEN)
		{
			string_walker walker;
			const char *s;
			float x, y;

			if (node->type == FLOW_WORD && node->content.text == NULL)
				continue;
			if (node->type == FLOW_SPACE && node->breaks_line)
				continue;
			if (node->type == FLOW_SHYPHEN && !node->breaks_line)
				continue;
			if (style->visibility != V_VISIBLE)
				continue;

			color[0] = style->color.r / 255.0f;
			color[1] = style->color.g / 255.0f;
			color[2] = style->color.b / 255.0f;

			if (color[0] != prev_color[0] || color[1] != prev_color[1] || color[2] != prev_color[2])
			{
				if (text)
				{
					fz_fill_text(ctx, dev, text, ctm, fz_device_rgb(ctx), prev_color, 1, NULL);
					fz_drop_text(ctx, text);
					text = NULL;
				}
				prev_color[0] = color[0];
				prev_color[1] = color[1];
				prev_color[2] = color[2];
			}

			if (!text)
				text = fz_new_text(ctx);

			if (node->bidi_level & 1)
				x = node->x + node->w;
			else
				x = node->x;
			y = node->y;

			trm.a = node->LJPEG_box->em;
			trm.b = 0;
			trm.c = 0;
			trm.d = -node->LJPEG_box->em;
			trm.e = x;
			trm.f = y - page_top;

			s = get_node_text(ctx, node);
			init_string_walker(ctx, &walker, hb_buf, node->bidi_level & 1, style->font, node->script, node->markup_lang, s);
			while (walk_string(&walker))
			{
				float node_scale = node->LJPEG_box->em / walker.scale;
				unsigned int i;
				int c, k, n;

				/* Flatten advance and offset into offset array. */
				int x_advance = 0;
				int y_advance = 0;
				for (i = 0; i < walker.glyph_count; ++i)
				{
					walker.glyph_pos[i].x_offset += x_advance;
					walker.glyph_pos[i].y_offset += y_advance;
					x_advance += walker.glyph_pos[i].x_advance;
					y_advance += walker.glyph_pos[i].y_advance;
				}

				if (node->bidi_level & 1)
					x -= x_advance * node_scale;

				/* Walk characters to find glyph clusters */
				k = 0;
				while (walker.start + k < walker.end)
				{
					n = fz_chartorune(&c, walker.start + k);

					for (i = 0; i < walker.glyph_count; ++i)
					{
						if (walker.glyph_info[i].cluster == k)
						{
							trm.e = x + walker.glyph_pos[i].x_offset * node_scale;
							trm.f = y - walker.glyph_pos[i].y_offset * node_scale - page_top;
							fz_show_glyph(ctx, text, walker.font, trm,
									walker.glyph_info[i].codepoint, c,
									0, node->bidi_level, LJPEG_box->markup_dir, node->markup_lang);
							c = -1; /* for subsequent glyphs in x-to-many mappings */
						}
					}

					/* no glyph found (many-to-many or many-to-one mapping) */
					if (c != -1)
					{
						fz_show_glyph(ctx, text, walker.font, trm,
								-1, c,
								0, node->bidi_level, LJPEG_box->markup_dir, node->markup_lang);
					}

					k += n;
				}

				if ((node->bidi_level & 1) == 0)
					x += x_advance * node_scale;

				y += y_advance * node_scale;
			}
		}
		else if (node->type == FLOW_IMAGE)
		{
			if (text)
			{
				fz_fill_text(ctx, dev, text, ctm, fz_device_rgb(ctx), color, 1, NULL);
				fz_drop_text(ctx, text);
				text = NULL;
			}
			if (style->visibility == V_VISIBLE)
			{
				fz_matrix itm = fz_pre_translate(ctm, node->x, node->y - page_top);
				itm = fz_pre_scale(itm, node->w, node->h);
				fz_fill_image(ctx, dev, node->content.image, itm, 1, NULL);
			}
		}
	}

	if (text)
	{
		fz_fill_text(ctx, dev, text, ctm, fz_device_rgb(ctx), color, 1, NULL);
		fz_drop_text(ctx, text);
		text = NULL;
	}
}

static void draw_rect(fz_context *ctx, fz_device *dev, fz_matrix ctm, float page_top, fz_css_color color, float x0, float y0, float x1, float y1)
{
	if (color.a > 0)
	{
		float rgb[3];

		fz_path *path = fz_new_path(ctx);

		fz_moveto(ctx, path, x0, y0 - page_top);
		fz_lineto(ctx, path, x1, y0 - page_top);
		fz_lineto(ctx, path, x1, y1 - page_top);
		fz_lineto(ctx, path, x0, y1 - page_top);
		fz_closepath(ctx, path);

		rgb[0] = color.r / 255.0f;
		rgb[1] = color.g / 255.0f;
		rgb[2] = color.b / 255.0f;

		fz_fill_path(ctx, dev, path, 0, ctm, fz_device_rgb(ctx), rgb, color.a / 255.0f, NULL);

		fz_drop_path(ctx, path);
	}
}

static const char *roman_uc[3][10] = {
	{ "", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX" },
	{ "", "X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC" },
	{ "", "C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM" },
};

static const char *roman_lc[3][10] = {
	{ "", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix" },
	{ "", "x", "xx", "xxx", "xl", "l", "lx", "lxx", "lxxx", "xc" },
	{ "", "c", "cc", "ccc", "cd", "d", "dc", "dcc", "dccc", "cm" },
};

static void format_roman_number(fz_context *ctx, char *buf, int size, int n, const char *sym[3][10], const char *sym_m)
{
	int I = n % 10;
	int X = (n / 10) % 10;
	int C = (n / 100) % 10;
	int M = (n / 1000);

	fz_strlcpy(buf, "", size);
	while (M--)
		fz_strlcat(buf, sym_m, size);
	fz_strlcat(buf, sym[2][C], size);
	fz_strlcat(buf, sym[1][X], size);
	fz_strlcat(buf, sym[0][I], size);
	fz_strlcat(buf, ". ", size);
}

static void format_alpha_number(fz_context *ctx, char *buf, int size, int n, int alpha, int omega)
{
	int base = omega - alpha + 1;
	int tmp[40];
	int i, c;

	if (alpha > 256) /* to skip final-s for greek */
		--base;

	/* Bijective base-26 (base-24 for greek) numeration */
	i = 0;
	while (n > 0)
	{
		--n;
		c = n % base + alpha;
		if (alpha > 256 && c > alpha + 16) /* skip final-s for greek */
			++c;
		tmp[i++] = c;
		n /= base;
	}

	while (i > 0)
		buf += fz_runetochar(buf, tmp[--i]);
	*buf++ = '.';
	*buf++ = ' ';
	*buf = 0;
}

static void format_list_number(fz_context *ctx, int type, int x, char *buf, int size)
{
	switch (type)
	{
	case LST_NONE: fz_strlcpy(buf, "", size); break;
	case LST_DISC: fz_strlcpy(buf, "\342\227\217  ", size); break; /* U+25CF BLACK CIRCLE */
	case LST_CIRCLE: fz_strlcpy(buf, "\342\227\213  ", size); break; /* U+25CB WHITE CIRCLE */
	case LST_SQUARE: fz_strlcpy(buf, "\342\226\240  ", size); break; /* U+25A0 BLACK SQUARE */
	default:
	case LST_DECIMAL: fz_snprintf(buf, size, "%d. ", x); break;
	case LST_DECIMAL_ZERO: fz_snprintf(buf, size, "%02d. ", x); break;
	case LST_LC_ROMAN: format_roman_number(ctx, buf, size, x, roman_lc, "m"); break;
	case LST_UC_ROMAN: format_roman_number(ctx, buf, size, x, roman_uc, "M"); break;
	case LST_LC_ALPHA: format_alpha_number(ctx, buf, size, x, 'a', 'z'); break;
	case LST_UC_ALPHA: format_alpha_number(ctx, buf, size, x, 'A', 'Z'); break;
	case LST_LC_LATIN: format_alpha_number(ctx, buf, size, x, 'a', 'z'); break;
	case LST_UC_LATIN: format_alpha_number(ctx, buf, size, x, 'A', 'Z'); break;
	case LST_LC_GREEK: format_alpha_number(ctx, buf, size, x, 0x03B1, 0x03C9); break;
	case LST_UC_GREEK: format_alpha_number(ctx, buf, size, x, 0x0391, 0x03A9); break;
	}
}

static fz_html_flow *find_list_mark_anchor(fz_context *ctx, fz_html_box *LJPEG_box)
{
	/* find first flow node in <li> tag */
	while (LJPEG_box)
	{
		if (LJPEG_box->type == BOX_FLOW)
			return LJPEG_box->flow_head;
		LJPEG_box = LJPEG_box->down;
	}
	return NULL;
}

static void draw_list_mark(fz_context *ctx, fz_html_box *LJPEG_box, float page_top, float page_bot, fz_device *dev, fz_matrix ctm, int n)
{
	fz_font *font;
	fz_text *text;
	fz_matrix trm;
	fz_html_flow *line;
	float y, w;
	float color[3];
	const char *s;
	char buf[40];
	int c, g;

	trm = fz_scale(LJPEG_box->em, -LJPEG_box->em);

	line = find_list_mark_anchor(ctx, LJPEG_box);
	if (line)
	{
		y = line->y;
	}
	else
	{
		float h = fz_from_css_number_scale(LJPEG_box->style.line_height, LJPEG_box->em);
		float a = LJPEG_box->em * 0.8f;
		float d = LJPEG_box->em * 0.2f;
		if (a + d > h)
			h = a + d;
		y = LJPEG_box->y + a + (h - a - d) / 2;
	}

	if (y > page_bot || y < page_top)
		return;

	format_list_number(ctx, LJPEG_box->style.list_style_type, n, buf, sizeof buf);

	s = buf;
	w = 0;
	while (*s)
	{
		s += fz_chartorune(&c, s);
		g = fz_encode_character_with_fallback(ctx, LJPEG_box->style.font, c, UCDN_SCRIPT_LATIN, FZ_LANG_UNSET, &font);
		w += fz_advance_glyph(ctx, font, g, 0) * LJPEG_box->em;
	}

	text = fz_new_text(ctx);

	fz_try(ctx)
	{
		s = buf;
		trm.e = LJPEG_box->x - w;
		trm.f = y - page_top;
		while (*s)
		{
			s += fz_chartorune(&c, s);
			g = fz_encode_character_with_fallback(ctx, LJPEG_box->style.font, c, UCDN_SCRIPT_LATIN, FZ_LANG_UNSET, &font);
			fz_show_glyph(ctx, text, font, trm, g, c, 0, 0, FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
			trm.e += fz_advance_glyph(ctx, font, g, 0) * LJPEG_box->em;
		}

		color[0] = LJPEG_box->style.color.r / 255.0f;
		color[1] = LJPEG_box->style.color.g / 255.0f;
		color[2] = LJPEG_box->style.color.b / 255.0f;

		fz_fill_text(ctx, dev, text, ctm, fz_device_rgb(ctx), color, 1, NULL);
	}
	fz_always(ctx)
		fz_drop_text(ctx, text);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void draw_block_box(fz_context *ctx, fz_html_box *LJPEG_box, float page_top, float page_bot, fz_device *dev, fz_matrix ctm, hb_buffer_t *hb_buf)
{
	float x0, y0, x1, y1;

	float *border = LJPEG_box->border;
	float *padding = LJPEG_box->padding;

	x0 = LJPEG_box->x - padding[L];
	y0 = LJPEG_box->y - padding[T];
	x1 = LJPEG_box->x + LJPEG_box->w + padding[R];
	y1 = LJPEG_box->b + padding[B];

	if (y0 > page_bot || y1 < page_top)
		return;

	if (LJPEG_box->style.visibility == V_VISIBLE)
	{
		draw_rect(ctx, dev, ctm, page_top, LJPEG_box->style.background_color, x0, y0, x1, y1);

		if (border[T] > 0)
			draw_rect(ctx, dev, ctm, page_top, LJPEG_box->style.border_color[T], x0 - border[L], y0 - border[T], x1 + border[R], y0);
		if (border[B] > 0)
			draw_rect(ctx, dev, ctm, page_top, LJPEG_box->style.border_color[B], x0 - border[L], y1, x1 + border[R], y1 + border[B]);
		if (border[L] > 0)
			draw_rect(ctx, dev, ctm, page_top, LJPEG_box->style.border_color[L], x0 - border[L], y0 - border[T], x0, y1 + border[B]);
		if (border[R] > 0)
			draw_rect(ctx, dev, ctm, page_top, LJPEG_box->style.border_color[R], x1, y0 - border[T], x1 + border[R], y1 + border[B]);

		if (LJPEG_box->list_item)
			draw_list_mark(ctx, LJPEG_box, page_top, page_bot, dev, ctm, LJPEG_box->list_item);
	}

	for (LJPEG_box = LJPEG_box->down; LJPEG_box; LJPEG_box = LJPEG_box->next)
	{
		switch (LJPEG_box->type)
		{
		case BOX_TABLE:
		case BOX_TABLE_ROW:
		case BOX_TABLE_CELL:
		case BOX_BLOCK: draw_block_box(ctx, LJPEG_box, page_top, page_bot, dev, ctm, hb_buf); break;
		case BOX_FLOW: draw_flow_box(ctx, LJPEG_box, page_top, page_bot, dev, ctm, hb_buf); break;
		}
	}
}

void
fz_draw_html(fz_context *ctx, fz_device *dev, fz_matrix ctm, fz_html *html, int page)
{
	hb_buffer_t *hb_buf = NULL;
	fz_html_box *LJPEG_box;
	int unlocked = 0;
	float page_top = page * html->page_h;
	float page_bot = (page + 1) * html->page_h;

	fz_var(hb_buf);
	fz_var(unlocked);

	draw_rect(ctx, dev, ctm, 0, html->root->style.background_color,
			0, 0,
			html->page_w + html->page_margin[L] + html->page_margin[R],
			html->page_h + html->page_margin[T] + html->page_margin[B]);

	ctm = fz_pre_translate(ctm, html->page_margin[L], html->page_margin[T]);

	fz_hb_lock(ctx);
	fz_try(ctx)
	{
		hb_buf = hb_buffer_create();
		fz_hb_unlock(ctx);
		unlocked = 1;

		for (LJPEG_box = html->root->down; LJPEG_box; LJPEG_box = LJPEG_box->next)
			draw_block_box(ctx, LJPEG_box, page_top, page_bot, dev, ctm, hb_buf);
	}
	fz_always(ctx)
	{
		if (unlocked)
			fz_hb_lock(ctx);
		hb_buffer_destroy(hb_buf);
		fz_hb_unlock(ctx);
	}
	fz_catch(ctx)
	{
		fz_rethrow(ctx);
	}
}
