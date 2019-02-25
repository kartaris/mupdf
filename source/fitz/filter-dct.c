#include "mupdf/fitz.h"

#include <stdio.h>
#include <jpeglib.h>

#ifndef SHARE_JPEG
typedef void * backing_store_ptr;
#include "jmemcust.h"
#endif

typedef struct fz_dctd_s fz_dctd;

struct fz_dctd_s
{
	fz_stream *chain;
	fz_stream *jpegtables;
	fz_stream *curr_stm;
	fz_context *ctx;
	int color_transform;
	int init;
	int stride;
	int l2factor;
	unsigned char *scanline;
	unsigned char *rp, *wp;
	struct LJPEG_jpeg_decompress_struct cinfo;
	struct jpeg_source_mgr srcmgr;
	struct LJPEG_jpeg_error_mgr errmgr;
	jmp_buf jb;
	char msg[JMSG_LENGTH_MAX];

	unsigned char buffer[4096];
};

#ifdef SHARE_JPEG

#define JZ_DCT_STATE_FROM_CINFO(c) (fz_dctd *)(c->client_data)

#define fz_dct_mem_init(st)
#define fz_dct_mem_term(st)

#else /* SHARE_JPEG */

#define JZ_DCT_STATE_FROM_CINFO(c) (fz_dctd *)(GET_CUST_MEM_DATA(c)->priv)

static void *
fz_dct_mem_alloc(LJPEG_j_common_ptr cinfo, size_t size)
{
	fz_dctd *state = JZ_DCT_STATE_FROM_CINFO(cinfo);
	return fz_malloc_no_throw(state->ctx, size);
}

static void
fz_dct_mem_free(LJPEG_j_common_ptr cinfo, void *object, size_t size)
{
	fz_dctd *state = JZ_DCT_STATE_FROM_CINFO(cinfo);
	fz_free(state->ctx, object);
}

static void
fz_dct_mem_init(fz_dctd *state)
{
	LJPEG_j_common_ptr cinfo = (LJPEG_j_common_ptr)&state->cinfo;
	jpeg_cust_mem_data *custmptr;

	custmptr = fz_malloc_struct(state->ctx, jpeg_cust_mem_data);

	if (!jpeg_cust_mem_init(custmptr, (void *) state, NULL, NULL, NULL,
				fz_dct_mem_alloc, fz_dct_mem_free,
				fz_dct_mem_alloc, fz_dct_mem_free, NULL))
	{
		fz_free(state->ctx, custmptr);
		fz_throw(state->ctx, FZ_ERROR_GENERIC, "cannot initialize custom JPEG memory handler");
	}

	cinfo->client_data = custmptr;
}

static void
fz_dct_mem_term(fz_dctd *state)
{
	if(state->cinfo.client_data)
	{
		fz_free(state->ctx, state->cinfo.client_data);
		state->cinfo.client_data = NULL;
	}
}

#endif /* SHARE_JPEG */

static void error_exit_dct(LJPEG_j_common_ptr cinfo)
{
	fz_dctd *state = JZ_DCT_STATE_FROM_CINFO(cinfo);
	cinfo->err->format_message(cinfo, state->msg);
	longjmp(state->jb, 1);
}

static void init_source_dct(LJPEG_j_decompress_ptr cinfo)
{
	/* nothing to do */
}

static void term_source_dct(LJPEG_j_decompress_ptr cinfo)
{
	/* nothing to do */
}

static boolean fill_input_buffer_dct(LJPEG_j_decompress_ptr cinfo)
{
	struct jpeg_source_mgr *src = cinfo->src;
	fz_dctd *state = JZ_DCT_STATE_FROM_CINFO(cinfo);
	fz_context *ctx = state->ctx;
	fz_stream *curr_stm = state->curr_stm;

	curr_stm->rp = curr_stm->wp;
	fz_try(ctx)
	{
		src->bytes_in_buffer = fz_available(ctx, curr_stm, 1);
	}
	fz_catch(ctx)
	{
		return 0;
	}
	src->next_input_byte = curr_stm->rp;

	if (src->bytes_in_buffer == 0)
	{
		static unsigned char eoi[2] = { 0xFF, JPEG_EOI };
		fz_warn(state->ctx, "premature end of file in jpeg");
		src->next_input_byte = eoi;
		src->bytes_in_buffer = 2;
	}

	return 1;
}

static void skip_input_data_dct(LJPEG_j_decompress_ptr cinfo, long num_bytes)
{
	struct jpeg_source_mgr *src = cinfo->src;
	if (num_bytes > 0)
	{
		while ((size_t)num_bytes > src->bytes_in_buffer)
		{
			num_bytes -= (long)src->bytes_in_buffer;
			(void) src->LJPEG_fill_input_buffer(cinfo);
		}
		src->next_input_byte += num_bytes;
		src->bytes_in_buffer -= num_bytes;
	}
}

static int
next_dctd(fz_context *ctx, fz_stream *stm, size_t max)
{
	fz_dctd *state = stm->state;
	LJPEG_j_decompress_ptr cinfo = &state->cinfo;
	unsigned char *p = state->buffer;
	unsigned char *ep;

	if (max > sizeof(state->buffer))
		max = sizeof(state->buffer);
	ep = state->buffer + max;

	if (setjmp(state->jb))
	{
		if (cinfo->src)
			state->curr_stm->rp = state->curr_stm->wp - cinfo->src->bytes_in_buffer;
		fz_throw(ctx, FZ_ERROR_GENERIC, "jpeg error: %s", state->msg);
	}

	if (!state->init)
	{
		int c;

		cinfo->src = NULL;
		cinfo->client_data = state;
		cinfo->err = &state->errmgr;
		LJPEG_jpeg_std_error(cinfo->err);
		cinfo->err->error_exit = error_exit_dct;

		fz_dct_mem_init(state);

		LJPEG_jpeg_create_decompress(cinfo);
		state->init = 1;

		/* Skip over any stray whitespace at the start of the stream */
		while ((c = fz_peek_byte(ctx, state->chain)) == '\n' || c == '\r' || c == ' ')
			(void)fz_read_byte(ctx, state->chain);

		cinfo->src = &state->srcmgr;
		cinfo->src->LJPEG_init_source = init_source_dct;
		cinfo->src->LJPEG_fill_input_buffer = fill_input_buffer_dct;
		cinfo->src->LJPEG_skip_input_data = skip_input_data_dct;
		cinfo->src->resync_to_restart = LJPEG_jpeg_resync_to_restart;
		cinfo->src->LJPEG_term_source = term_source_dct;

		/* optionally load additional JPEG tables first */
		if (state->jpegtables)
		{
			state->curr_stm = state->jpegtables;
			cinfo->src->next_input_byte = state->curr_stm->rp;
			cinfo->src->bytes_in_buffer = state->curr_stm->wp - state->curr_stm->rp;
			LJPEG_jpeg_read_header(cinfo, 0);
			state->curr_stm->rp = state->curr_stm->wp - state->cinfo.src->bytes_in_buffer;
			state->curr_stm = state->chain;
		}

		cinfo->src->next_input_byte = state->curr_stm->rp;
		cinfo->src->bytes_in_buffer = state->curr_stm->wp - state->curr_stm->rp;

		LJPEG_jpeg_read_header(cinfo, 1);

		/* default value if ColorTransform is not set */
		if (state->color_transform == -1)
		{
			if (state->cinfo.num_components == 3)
				state->color_transform = 1;
			else
				state->color_transform = 0;
		}

		if (cinfo->saw_Adobe_marker)
			state->color_transform = cinfo->Adobe_transform;

		/* Guess the input colorspace, and set output colorspace accordingly */
		switch (cinfo->num_components)
		{
		case 3:
			if (state->color_transform)
				cinfo->jpeg_color_space = LJPEG_JCS_YCbCr;
			else
				cinfo->jpeg_color_space = LJPEG_JCS_RGB;
			break;
		case 4:
			if (state->color_transform)
				cinfo->jpeg_color_space = LJPEG_JCS_YCCK;
			else
				cinfo->jpeg_color_space = LJPEG_JCS_CMYK;
			break;
		}

		cinfo->scale_num = 8/(1<<state->l2factor);
		cinfo->scale_denom = 8;

		LJPEG_jpeg_start_decompress(cinfo);

		state->stride = cinfo->output_width * cinfo->output_components;
		state->scanline = fz_malloc(ctx, state->stride);
		state->rp = state->scanline;
		state->wp = state->scanline;
	}

	while (state->rp < state->wp && p < ep)
		*p++ = *state->rp++;

	while (p < ep)
	{
		if (cinfo->output_scanline == cinfo->output_height)
			break;

		if (p + state->stride <= ep)
		{
			LJPEG_jpeg_read_scanlines(cinfo, &p, 1);
			p += state->stride;
		}
		else
		{
			LJPEG_jpeg_read_scanlines(cinfo, &state->scanline, 1);
			state->rp = state->scanline;
			state->wp = state->scanline + state->stride;
		}

		while (state->rp < state->wp && p < ep)
			*p++ = *state->rp++;
	}
	stm->rp = state->buffer;
	stm->wp = p;
	stm->pos += (p - state->buffer);
	if (p == stm->rp)
		return EOF;

	return *stm->rp++;
}

static void
close_dctd(fz_context *ctx, void *state_)
{
	fz_dctd *state = (fz_dctd *)state_;

	if (setjmp(state->jb))
	{
		fz_warn(ctx, "jpeg error: %s", state->msg);
		goto skip;
	}

	/* We call LJPEG_jpeg_abort rather than the more usual
	 * LJPEG_jpeg_finish_decompress here. This has the same effect,
	 * but doesn't spew warnings if we didn't read enough data etc.
	 */
	if (state->init)
		LJPEG_jpeg_abort((LJPEG_j_common_ptr)&state->cinfo);

skip:
	if (state->cinfo.src)
		state->curr_stm->rp = state->curr_stm->wp - state->cinfo.src->bytes_in_buffer;
	if (state->init)
		LJPEG_jpeg_destroy_decompress(&state->cinfo);

	fz_dct_mem_term(state);

	fz_free(ctx, state->scanline);
	fz_drop_stream(ctx, state->chain);
	fz_drop_stream(ctx, state->jpegtables);
	fz_free(ctx, state);
}

/* Default: color_transform = -1 (unset), l2factor = 0, jpegtables = NULL */
fz_stream *
fz_open_dctd(fz_context *ctx, fz_stream *chain, int color_transform, int l2factor, fz_stream *jpegtables)
{
	fz_dctd *state = fz_malloc_struct(ctx, fz_dctd);

	state->ctx = ctx;
	state->color_transform = color_transform;
	state->init = 0;
	state->l2factor = l2factor;
	state->cinfo.client_data = NULL;

	state->chain = fz_keep_stream(ctx, chain);
	state->jpegtables = fz_keep_stream(ctx, jpegtables);
	state->curr_stm = state->chain;

	return fz_new_stream(ctx, state, next_dctd, close_dctd);
}
