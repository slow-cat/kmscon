/*
 * kmscon - OpenGL Textures Text Renderer Backend
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
 * SECTION:text_gltex.c
 * @short_description: OpenGL Textures Text Renderer Backend
 * @include: text.h
 *
 * Uses OpenGL textures to store glyph information and draws these textures with
 * a custom fragment shader.
 * Glyphs are stored in texture-atlases. OpenGL has heavy restrictions on
 * texture sizes so we need to use multiple atlases. As there is no way to pass
 * a varying amount of textures to a shader, we need to render the screen for
 * each atlas we have.
 */

#include <libtsm.h>
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shl_dlist.h"
#include "shl_gl.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "text.h"
#include "text_gltex_atlas.frag.bin.h"
#include "text_gltex_atlas.vert.bin.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_gltex"

static FILE *gltex_log_file;
static int gltex_log_users;

static void gltex_tmp_log(const char *level, const char *fmt, ...)
{
	va_list args;

	if (!log_is_debug_enabled())
		return;

	if (!gltex_log_file) {
		gltex_log_file = fopen("/tmp/kmscon-gltex.log", "a");
		if (!gltex_log_file)
			return;
		setvbuf(gltex_log_file, NULL, _IOLBF, 0);
	}

	fprintf(gltex_log_file, "[%s] ", level);
	va_start(args, fmt);
	vfprintf(gltex_log_file, fmt, args);
	va_end(args);
	fputc('\n', gltex_log_file);
}

static void gltex_tmp_log_close(void)
{
	if (!gltex_log_file)
		return;
	fclose(gltex_log_file);
	gltex_log_file = NULL;
}

#define GLTEX_LOG_INFO(...)                                                                       \
	do {                                                                                       \
		log_info(__VA_ARGS__);                                                             \
		gltex_tmp_log("INFO", __VA_ARGS__);                                                \
	} while (0)
#define GLTEX_LOG_WARNING(...)                                                                    \
	do {                                                                                       \
		log_warning(__VA_ARGS__);                                                          \
		gltex_tmp_log("WARN", __VA_ARGS__);                                                \
	} while (0)
#define GLTEX_LOG_ERROR(...)                                                                      \
	do {                                                                                       \
		log_error(__VA_ARGS__);                                                            \
		gltex_tmp_log("ERROR", __VA_ARGS__);                                               \
	} while (0)
#define GLTEX_LOG_DEBUG(...)                                                                      \
	do {                                                                                       \
		log_debug(__VA_ARGS__);                                                            \
		gltex_tmp_log("DEBUG", __VA_ARGS__);                                               \
	} while (0)

/* thanks khronos for breaking backwards compatibility.. */
#if !defined(GL_UNPACK_ROW_LENGTH) && defined(GL_UNPACK_ROW_LENGTH_EXT)
#define GL_UNPACK_ROW_LENGTH GL_UNPACK_ROW_LENGTH_EXT
#endif

struct atlas {
	struct shl_dlist list;

	GLuint tex;
	unsigned int height;
	unsigned int width;
	unsigned int count;
	unsigned int fill;

	unsigned int cache_size;
	unsigned int cache_num;
	GLfloat *cache_pos;
	GLfloat *cache_texpos;
	GLfloat *cache_fgcol;
	GLfloat *cache_bgcol;

	GLfloat advance_htex;
	GLfloat advance_vtex;
};

struct glyph {
	const struct kmscon_glyph *glyph;
	struct atlas *atlas;
	unsigned int texoff;
};

#define GLYPH_WIDTH(gly) ((gly)->glyph->buf.width)
#define GLYPH_HEIGHT(gly) ((gly)->glyph->buf.height)
#define GLYPH_STRIDE(gly) ((gly)->glyph->buf.stride)
#define GLYPH_DATA(gly) ((gly)->glyph->buf.data)

/* Cell state for differential rendering */
struct gltex_cell {
	uint64_t id;
	struct tsm_screen_attr attr;
	bool overflow;
};

struct gltex {
	struct shl_hashtable *glyphs;
	struct shl_hashtable *bold_glyphs;
	unsigned int max_tex_size;
	bool supports_rowlen;

	struct shl_dlist atlases;

	GLfloat advance_x;
	GLfloat advance_y;

	struct gl_shader *shader;
	GLuint uni_cos;
	GLuint uni_sin;
	GLuint uni_proj;
	GLuint uni_atlas;
	GLuint uni_advance_htex;
	GLuint uni_advance_vtex;

	unsigned int sw;
	unsigned int sh;

	GLfloat cos;
	GLfloat sin;

	struct tsm_screen_attr attr;

	/* Offscreen FBO for persistent rendering */
	GLuint offscreen_fbo;
	GLuint offscreen_texture;
	float bg_r, bg_g, bg_b;
	bool need_clear;
	bool use_offscreen;

	/* Function pointer for glBlitFramebuffer (dynamically loaded) */
	void (*glBlitFramebufferFunc)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
				      GLbitfield, GLenum);

	/* Cell state tracking for differential rendering */
	struct gltex_cell *prev_cells;
	unsigned int num_cells;

	unsigned int cursor_x;
	unsigned int cursor_y;
};

/**
 * Create an offscreen framebuffer for differential rendering.
 *
 * @param gt Renderer instance.
 *
 * Returns: 0 on success, -EOPNOTSUPP if blit is unavailable, negative error on failure.
 */
static int gltex_create_offscreen(struct gltex *gt)
{
	if (!gt->glBlitFramebufferFunc)
		return -EOPNOTSUPP;
	if (gt->offscreen_fbo)
		return 0;

	glGenFramebuffers(1, &gt->offscreen_fbo);
	glGenTextures(1, &gt->offscreen_texture);
	GLTEX_LOG_INFO("FBO: Generated FBO=%u, Texture=%u for size %ux%u",
		       gt->offscreen_fbo, gt->offscreen_texture, gt->sw, gt->sh);

	glBindTexture(GL_TEXTURE_2D, gt->offscreen_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gt->sw, gt->sh, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glBindFramebuffer(GL_FRAMEBUFFER, gt->offscreen_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			       gt->offscreen_texture, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		GLTEX_LOG_WARNING("FBO incomplete: status=0x%x", status);
		glDeleteFramebuffers(1, &gt->offscreen_fbo);
		glDeleteTextures(1, &gt->offscreen_texture);
		gt->offscreen_fbo = 0;
		gt->offscreen_texture = 0;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return -EIO;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	GLTEX_LOG_INFO("FBO differential rendering enabled: %ux%u", gt->sw, gt->sh);
	gt->need_clear = true;
	return 0;
}

/**
 * Destroy the offscreen framebuffer and texture if present.
 *
 * @param gt Renderer instance.
 */
static void gltex_destroy_offscreen(struct gltex *gt)
{
	if (gt->offscreen_fbo) {
		glDeleteFramebuffers(1, &gt->offscreen_fbo);
		gt->offscreen_fbo = 0;
	}
	if (gt->offscreen_texture) {
		glDeleteTextures(1, &gt->offscreen_texture);
		gt->offscreen_texture = 0;
	}
}

static int gltex_init(struct kmscon_text *txt)
{
	struct gltex *gt;

	gt = malloc(sizeof(*gt));
	if (!gt)
		return -ENOMEM;
	memset(gt, 0, sizeof(*gt));

	txt->data = gt;
	gltex_log_users++;
	GLTEX_LOG_INFO("gltex init");
	return 0;
}

static void gltex_destroy(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret;

	ret = uterm_display_use(txt->disp);
	if (ret == 0) {
		/* Clean up FBO and cell tracking on final destroy */
		if (gt->offscreen_fbo) {
			glDeleteFramebuffers(1, &gt->offscreen_fbo);
			glDeleteTextures(1, &gt->offscreen_texture);
		}
		free(gt->prev_cells);
	}

	free(gt);
	if (gltex_log_users > 0)
		gltex_log_users--;
	if (gltex_log_users == 0)
		gltex_tmp_log_close();
}

static void free_glyph(void *data)
{
	struct glyph *glyph = data;

	free(glyph);
}
// static inline void load_glblit(struct gltex*gt)
// {
// 	/* Load glBlitFramebuffer function for FBO support */
// 	gt->glBlitFramebufferFunc = NULL;
// 	if (ext) {
// 		if (strstr(ext, "GL_ANGLE_framebuffer_blit")) {
// 			gt->glBlitFramebufferFunc =
// 				(void *)eglGetProcAddress("glBlitFramebufferANGLE");
// 			if (gt->glBlitFramebufferFunc)
// 				log_info("glBlitFramebufferANGLE loaded (ANGLE)");
// 		}
// 		if (!gt->glBlitFramebufferFunc && strstr(ext, "GL_NV_framebuffer_blit")) {
// 			gt->glBlitFramebufferFunc =
// 				(void *)eglGetProcAddress("glBlitFramebufferNV");
// 			if (gt->glBlitFramebufferFunc)
// 				log_info("glBlitFramebufferNV loaded (NV)");
// 		}
// 	}
// 	if (!gt->glBlitFramebufferFunc) {
// 		gt->glBlitFramebufferFunc = (void *)eglGetProcAddress("glBlitFramebuffer");
// 		if (gt->glBlitFramebufferFunc) {
// 			log_info("glBlitFramebuffer loaded (core GLES3)");
// 		} else {
// 			log_warning("No glBlitFramebuffer support, FBO optimization disabled");
// 		}
// 	}
// }
static int gltex_set(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret, vlen, flen;
	const char *vert, *frag;
	static char *attr[] = {"position", "texture_position", "fgcolor", "bgcolor"};
	GLint s;
	const char *ext;

	if (!uterm_display_has_opengl(txt->disp))
		return -EINVAL;

	ret = uterm_display_use(txt->disp);
	if (ret < 0)
		return ret;

	shl_dlist_init(&gt->atlases);

	ret = shl_hashtable_new(&gt->glyphs, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		return ret;

	ret = shl_hashtable_new(&gt->bold_glyphs, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		goto err_htable;

	vert = _binary_text_gltex_atlas_vert_start;
	vlen = _binary_text_gltex_atlas_vert_size;
	frag = _binary_text_gltex_atlas_frag_start;
	flen = _binary_text_gltex_atlas_frag_size;
	gl_clear_error();

	ret = gl_shader_new(&gt->shader, vert, vlen, frag, flen, attr, 4, log_llog, NULL);
	if (ret)
		goto err_bold_htable;

	gt->uni_cos = gl_shader_get_uniform(gt->shader, "cos");
	gt->uni_sin = gl_shader_get_uniform(gt->shader, "sin");
	gt->uni_proj = gl_shader_get_uniform(gt->shader, "projection");
	gt->uni_atlas = gl_shader_get_uniform(gt->shader, "atlas");
	gt->uni_advance_htex = gl_shader_get_uniform(gt->shader, "advance_htex");
	gt->uni_advance_vtex = gl_shader_get_uniform(gt->shader, "advance_vtex");

	if (gl_has_error(gt->shader)) {
		GLTEX_LOG_WARNING("cannot create shader");
		goto err_shader;
	}

	gt->sw = uterm_display_get_width(txt->disp);
	gt->sh = uterm_display_get_height(txt->disp);

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		txt->cols = gt->sw / FONT_WIDTH(txt);
		txt->rows = gt->sh / FONT_HEIGHT(txt);
	} else {
		txt->cols = gt->sh / FONT_WIDTH(txt);
		txt->rows = gt->sw / FONT_HEIGHT(txt);
	}

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s);
	if (s <= 0)
		s = 64;
	else if (s > 2048)
		s = 2048;
	gt->max_tex_size = s;

	gl_clear_error();

	ext = (const char *)glGetString(GL_EXTENSIONS);
	if (ext && strstr((const char *)ext, "GL_EXT_unpack_subimage")) {
		gt->supports_rowlen = true;
	} else {
		GLTEX_LOG_WARNING("your GL implementation does not support GL_EXT_unpack_subimage, "
			    "glyph-rendering may be slower than usual");
	}

	/* Load glBlitFramebuffer function for FBO support */
	gt->glBlitFramebufferFunc = NULL;
	if (ext) {
		if (strstr(ext, "GL_ANGLE_framebuffer_blit")) {
			gt->glBlitFramebufferFunc =
				(void *)eglGetProcAddress("glBlitFramebufferANGLE");
			if (gt->glBlitFramebufferFunc)
				GLTEX_LOG_INFO("glBlitFramebufferANGLE loaded (ANGLE)");
		}
		if (!gt->glBlitFramebufferFunc && strstr(ext, "GL_NV_framebuffer_blit")) {
			gt->glBlitFramebufferFunc =
				(void *)eglGetProcAddress("glBlitFramebufferNV");
			if (gt->glBlitFramebufferFunc)
				GLTEX_LOG_INFO("glBlitFramebufferNV loaded (NV)");
		}
	}
	if (!gt->glBlitFramebufferFunc) {
		gt->glBlitFramebufferFunc = (void *)eglGetProcAddress("glBlitFramebuffer");
		if (gt->glBlitFramebufferFunc) {
			GLTEX_LOG_INFO("glBlitFramebuffer loaded (core GLES3)");
		} else {
			GLTEX_LOG_WARNING("No glBlitFramebuffer support, FBO optimization disabled");
		}
	}

	gt->use_offscreen = txt->offscreen;

	/* Update screen dimensions */
	unsigned int old_sw = gt->sw;
	unsigned int old_sh = gt->sh;
	unsigned int new_sw = uterm_display_get_width(txt->disp);
	unsigned int new_sh = uterm_display_get_height(txt->disp);
	bool size_changed = (old_sw != new_sw || old_sh != new_sh);
	gt->need_clear |= size_changed;

	gt->sw = new_sw;
	gt->sh = new_sh;

	/* Setup cell tracking array */
	unsigned int new_num_cells = txt->cols * txt->rows;
	if (!gt->prev_cells || gt->num_cells != new_num_cells) {
		free(gt->prev_cells);
		gt->num_cells = new_num_cells;
		gt->prev_cells = malloc(sizeof(*gt->prev_cells) * gt->num_cells);
		if (!gt->prev_cells) {
			GLTEX_LOG_ERROR("failed to allocate cell tracking array");
			goto err_shader;
		}
		memset(gt->prev_cells, 0, sizeof(*gt->prev_cells) * gt->num_cells);
		gt->need_clear = true; /* Need full redraw after cell array change */
	}

	/* Create or recreate FBO if needed */
	if (!gt->use_offscreen) {
		gltex_destroy_offscreen(gt);
	} else if (!gt->offscreen_fbo || size_changed) {
		gltex_destroy_offscreen(gt);
		if (gltex_create_offscreen(gt) < 0)
			GLTEX_LOG_WARNING("offscreen requested but FBO creation failed");
	}

	return 0;

err_shader:
	gl_shader_unref(gt->shader);
err_bold_htable:
	shl_hashtable_free(gt->bold_glyphs);
err_htable:
	shl_hashtable_free(gt->glyphs);
	return ret;
}

static void gltex_unset(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret;
	struct shl_dlist *iter;
	struct atlas *atlas;
	bool gl = true;

	ret = uterm_display_use(txt->disp);
	if (ret) {
		gl = false;
		GLTEX_LOG_WARNING("cannot activate OpenGL-CTX during destruction");
	}

	shl_hashtable_free(gt->bold_glyphs);
	shl_hashtable_free(gt->glyphs);

	while (!shl_dlist_empty(&gt->atlases)) {
		iter = gt->atlases.next;
		shl_dlist_unlink(iter);
		atlas = shl_dlist_entry(iter, struct atlas, list);

		free(atlas->cache_pos);
		free(atlas->cache_texpos);
		free(atlas->cache_fgcol);
		free(atlas->cache_bgcol);

		if (gl)
			gl_tex_free(&atlas->tex, 1);
		free(atlas);
	}

	/* Note: We do NOT free prev_cells and FBO here because gltex_set()
	 * may be called again (e.g. on rotate) and we want to preserve state */

	if (gl) {
		gl_shader_unref(gt->shader);
		gl_clear_error();
	}
}

/* returns an atlas with at least 1 free glyph position; NULL on error */
static struct atlas *get_atlas(struct kmscon_text *txt, unsigned int num)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	size_t newsize;
	unsigned int width, height, nsize;
	GLenum err;

	/* check whether the last added atlas has still room for one glyph */
	if (!shl_dlist_empty(&gt->atlases)) {
		atlas = shl_dlist_entry(gt->atlases.next, struct atlas, list);
		if (atlas->fill + num <= atlas->count)
			return atlas;
	}

	/* all atlases are full so we have to create a new atlas */
	atlas = malloc(sizeof(*atlas));
	if (!atlas)
		return NULL;
	memset(atlas, 0, sizeof(*atlas));

	gl_clear_error();

	gl_tex_new(&atlas->tex, 1);
	err = glGetError();
	if (err != GL_NO_ERROR || !atlas->tex) {
		gl_clear_error();
		GLTEX_LOG_WARNING("cannot create new OpenGL texture: %d", err);
		goto err_free;
	}

	newsize = gt->max_tex_size / FONT_WIDTH(txt);
	if (newsize < 1)
		newsize = 1;

	/* OpenGL texture sizes are heavily restricted so we need to find a
	 * valid texture size that is big enough to hold as many glyphs as
	 * possible but at least 1 */
try_next:
	width = shl_next_pow2(FONT_WIDTH(txt) * newsize);
	height = shl_next_pow2(FONT_HEIGHT(txt));

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
		     NULL);

	err = glGetError();
	if (err != GL_NO_ERROR) {
		if (newsize > 1) {
			--newsize;
			goto try_next;
		}
		gl_clear_error();
		GLTEX_LOG_WARNING("OpenGL textures too small for a single glyph (%d)", err);
		goto err_tex;
	}

	GLTEX_LOG_DEBUG("new atlas of size %ux%u for %zu", width, height, newsize);

	nsize = txt->cols * txt->rows + 1; // +1 for the mouse pointer

	atlas->cache_pos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	if (!atlas->cache_pos)
		goto err_mem;

	atlas->cache_texpos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	if (!atlas->cache_texpos)
		goto err_mem;

	atlas->cache_fgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	if (!atlas->cache_fgcol)
		goto err_mem;

	atlas->cache_bgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	if (!atlas->cache_bgcol)
		goto err_mem;

	atlas->cache_size = nsize;
	atlas->count = newsize;
	atlas->width = width;
	atlas->height = height;
	atlas->advance_htex = 1.0 / atlas->width * FONT_WIDTH(txt);
	atlas->advance_vtex = 1.0 / atlas->height * FONT_HEIGHT(txt);

	shl_dlist_link(&gt->atlases, &atlas->list);
	return atlas;

err_mem:
	free(atlas->cache_pos);
	free(atlas->cache_texpos);
	free(atlas->cache_fgcol);
	free(atlas->cache_bgcol);
err_tex:
	gl_tex_free(&atlas->tex, 1);
err_free:
	free(atlas);
	return NULL;
}

static int find_glyph(struct kmscon_text *txt, struct glyph **out, uint64_t id, const uint32_t *ch,
		      size_t len, const struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct glyph *glyph;
	bool res;
	int ret, i;
	GLenum err;
	uint8_t *packed_data, *dst, *src;
	struct shl_hashtable *gtable;
	struct kmscon_font *font;

	if (attr->bold) {
		gtable = gt->bold_glyphs;
		font = txt->bold_font;
	} else {
		gtable = gt->glyphs;
		font = txt->font;
	}

	if (attr->underline)
		font->attr.underline = true;
	else
		font->attr.underline = false;

	if (attr->italic)
		font->attr.italic = true;
	else
		font->attr.italic = false;

	res = shl_hashtable_find(gtable, (void **)&glyph, id);
	if (res) {
		*out = glyph;
		return 0;
	}

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;
	memset(glyph, 0, sizeof(*glyph));

	if (!len)
		ret = kmscon_font_render_empty(font, &glyph->glyph);
	else
		ret = kmscon_font_render(font, id, ch, len, &glyph->glyph);

	if (ret) {
		ret = kmscon_font_render_inval(font, &glyph->glyph);
		if (ret)
			goto err_free;
	}

	atlas = get_atlas(txt, glyph->glyph->width);
	if (!atlas) {
		ret = -EFAULT;
		goto err_free;
	}

	/* Funnily, not all OpenGLESv2 implementations support specifying the
	 * stride of a texture. Therefore, we then need to create a
	 * temporary image with a stride equal to the image width for loading
	 * the texture. This may slow down loading new glyphs but doesn't affect
	 * overall rendering performance. But driver developers should really
	 * add this! */

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if (!gt->supports_rowlen) {
		if (GLYPH_STRIDE(glyph) == GLYPH_WIDTH(glyph)) {
			glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0,
					GLYPH_WIDTH(glyph), GLYPH_HEIGHT(glyph), GL_ALPHA,
					GL_UNSIGNED_BYTE, GLYPH_DATA(glyph));
		} else {
			packed_data = malloc(GLYPH_WIDTH(glyph) * GLYPH_HEIGHT(glyph));
			if (!packed_data) {
				GLTEX_LOG_ERROR("cannot allocate memory for glyph storage");
				ret = -ENOMEM;
				goto err_free;
			}

			src = GLYPH_DATA(glyph);
			dst = packed_data;
			for (i = 0; i < GLYPH_HEIGHT(glyph); ++i) {
				memcpy(dst, src, GLYPH_WIDTH(glyph));
				dst += GLYPH_WIDTH(glyph);
				src += GLYPH_STRIDE(glyph);
			}

			glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0,
					GLYPH_WIDTH(glyph), GLYPH_HEIGHT(glyph), GL_ALPHA,
					GL_UNSIGNED_BYTE, packed_data);
			free(packed_data);
		}
	} else {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, GLYPH_STRIDE(glyph));
		glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0,
				GLYPH_WIDTH(glyph), GLYPH_HEIGHT(glyph), GL_ALPHA, GL_UNSIGNED_BYTE,
				GLYPH_DATA(glyph));
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

	/* Check for GL-errors
	 * As OpenGL is a state-machine, we cannot really tell which call failed
	 * without adding a glGetError() after each call. This is totally
	 * overkill so let us at least catch the error afterwards.
	 * We also add a hint to disable OpenGL if this does not work. This
	 * should _always_ work but OpenGL is kind of a black-box that isn't
	 * verbose at all and many things can go wrong. */

	err = glGetError();
	if (err != GL_NO_ERROR) {
		gl_clear_error();
		GLTEX_LOG_WARNING("cannot load glyph data into OpenGL texture (%d: %s); disable the "
			    "GL-renderer if this does not work reliably",
			    err, gl_err_to_str(err));
		ret = -EFAULT;
		goto err_free;
	}

	glyph->atlas = atlas;
	glyph->texoff = atlas->fill;

	ret = shl_hashtable_insert(gtable, id, glyph);
	if (ret)
		goto err_free;

	atlas->fill += glyph->glyph->width;

	*out = glyph;
	return 0;

err_free:
	free(glyph);
	return ret;
}

static void gltex_set_rotate(struct gltex *gt, enum Orientation orientation)
{
	float sin_table[5] = {0.0, 1.0, 0.0, -1.0, 0.0};

	gt->cos = sin_table[orientation + 1];
	gt->sin = sin_table[orientation];
}

static int gltex_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	struct gltex *gt = txt->data;

	txt->orientation = orientation;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		txt->cols = gt->sw / FONT_WIDTH(txt);
		txt->rows = gt->sh / FONT_HEIGHT(txt);
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt);
	} else {
		float aspect = (float)gt->sw / (float)gt->sh;
		txt->cols = gt->sh / FONT_WIDTH(txt);
		txt->rows = gt->sw / FONT_HEIGHT(txt);
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt) * aspect;
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt) * (1. / aspect);
	}

	GLTEX_LOG_INFO("gltex_rotate called - unset/set will preserve FBO state");
	gltex_unset(txt);
	gltex_set(txt);
	return 0;
}

static int gltex_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct shl_dlist *iter;
	int ret;
	bool use_offscreen = gt->use_offscreen && gt->offscreen_fbo;

	ret = uterm_display_use(txt->disp);
	if (ret)
		return ret;
	uterm_display_move_cursor(txt->disp, gt->cursor_x, gt->cursor_y);

	if (use_offscreen) {
		glBindFramebuffer(GL_FRAMEBUFFER, gt->offscreen_fbo);
	} else {
		gt->need_clear = true;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	if (uterm_display_need_redraw(txt->disp))
		gt->need_clear = true;

	/* Check if background color changed */
	float new_bg_r = attr->br / 255.0;
	float new_bg_g = attr->bg / 255.0;
	float new_bg_b = attr->bb / 255.0;

	if (gt->bg_r != new_bg_r || gt->bg_g != new_bg_g || gt->bg_b != new_bg_b) {
		GLTEX_LOG_WARNING(
			    "FBO: bg color changed from (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f), "
			    "need_clear=true",
			    gt->bg_r, gt->bg_g, gt->bg_b, new_bg_r, new_bg_g, new_bg_b);
		gt->bg_r = new_bg_r;
		gt->bg_g = new_bg_g;
		gt->bg_b = new_bg_b;
		gt->need_clear = true;
	}

	/* Only clear if needed (first frame or background color changed) */
	if (gt->need_clear) {
		GLTEX_LOG_INFO("CLEARING framebuffer (need_clear=true)");
		glClearColor(gt->bg_r, gt->bg_g, gt->bg_b, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);
		gt->need_clear = false;

		/* Also clear prev_cells to force full redraw */
		if (gt->prev_cells && gt->num_cells > 0) {
			memset(gt->prev_cells, 0, sizeof(*gt->prev_cells) * gt->num_cells);
		}
	} else {
		GLTEX_LOG_DEBUG("FBO: SKIP clear (differential mode)");
	}

	shl_dlist_for_each(iter, &gt->atlases)
	{
		atlas = shl_dlist_entry(iter, struct atlas, list);

		atlas->cache_num = 0;
	}

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt);
	} else {
		float aspect = (float)gt->sw / (float)gt->sh;
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt) * aspect;
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt) * (1. / aspect);
	}
	gltex_set_rotate(gt, txt->orientation);

	gt->attr = *attr;

	return 0;
}

static int atlas_push(struct gltex *gt, struct atlas *atlas, float gl_x1, float gl_x2, float gl_y1,
		      float gl_y2, unsigned int width, const struct glyph *glyph,
		      const struct tsm_screen_attr *attr)
{
	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;
	int i, idx;
	idx = atlas->cache_num * 2 * 6;
	atlas->cache_pos[idx + 0] = gl_x1;
	atlas->cache_pos[idx + 1] = gl_y1;
	atlas->cache_pos[idx + 2] = gl_x1;
	atlas->cache_pos[idx + 3] = gl_y2;
	atlas->cache_pos[idx + 4] = gl_x2;
	atlas->cache_pos[idx + 5] = gl_y2;

	atlas->cache_pos[idx + 6] = gl_x1;
	atlas->cache_pos[idx + 7] = gl_y1;
	atlas->cache_pos[idx + 8] = gl_x2;
	atlas->cache_pos[idx + 9] = gl_y2;
	atlas->cache_pos[idx + 10] = gl_x2;
	atlas->cache_pos[idx + 11] = gl_y1;

	atlas->cache_texpos[idx + 0] = glyph->texoff;
	atlas->cache_texpos[idx + 1] = 0.0;
	atlas->cache_texpos[idx + 2] = glyph->texoff;
	atlas->cache_texpos[idx + 3] = 1.0;
	atlas->cache_texpos[idx + 4] = glyph->texoff + width;
	atlas->cache_texpos[idx + 5] = 1.0;

	atlas->cache_texpos[idx + 6] = glyph->texoff;
	atlas->cache_texpos[idx + 7] = 0.0;
	atlas->cache_texpos[idx + 8] = glyph->texoff + width;
	atlas->cache_texpos[idx + 9] = 1.0;
	atlas->cache_texpos[idx + 10] = glyph->texoff + width;
	atlas->cache_texpos[idx + 11] = 0.0;

	for (i = 0; i < 6; ++i) {
		idx = atlas->cache_num * 3 * 6 + i * 3;
		if (attr->inverse) {
			atlas->cache_fgcol[idx + 0] = attr->br / 255.0;
			atlas->cache_fgcol[idx + 1] = attr->bg / 255.0;
			atlas->cache_fgcol[idx + 2] = attr->bb / 255.0;
			atlas->cache_bgcol[idx + 0] = attr->fr / 255.0;
			atlas->cache_bgcol[idx + 1] = attr->fg / 255.0;
			atlas->cache_bgcol[idx + 2] = attr->fb / 255.0;
		} else {
			atlas->cache_fgcol[idx + 0] = attr->fr / 255.0;
			atlas->cache_fgcol[idx + 1] = attr->fg / 255.0;
			atlas->cache_fgcol[idx + 2] = attr->fb / 255.0;
			atlas->cache_bgcol[idx + 0] = attr->br / 255.0;
			atlas->cache_bgcol[idx + 1] = attr->bg / 255.0;
			atlas->cache_bgcol[idx + 2] = attr->bb / 255.0;
		}
	}

	++atlas->cache_num;

	return 0;
}
static int gltex_draw(struct kmscon_text *txt, uint64_t id, const uint32_t *ch, size_t len,
		      unsigned int width, unsigned int posx, unsigned int posy,
		      const struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct glyph *glyph;
	unsigned int cell_idx;
	bool use_diff = gt->use_offscreen && gt->offscreen_fbo;

	if (!width)
		return 0;
	cell_idx = posy * txt->cols + posx;
	if (!len && posx && gt->prev_cells && cell_idx < gt->num_cells &&
	    gt->prev_cells[cell_idx - 1].overflow) {
		struct gltex_cell *prev = &gt->prev_cells[cell_idx];
		prev->id = id;
		prev->attr = *attr;
		prev->overflow = false;
		return 0;
	}
	int ret;

	ret = find_glyph(txt, &glyph, id, ch, len, attr);
	if (ret)
		return ret;
	atlas = glyph->atlas;

	if (width == 1 && glyph->glyph->width == 2) {
		width = 2;
	}
	/* Differential rendering: check if cell changed */
	if (gt->prev_cells && gt->num_cells > 0 && cell_idx < gt->num_cells) {
		struct gltex_cell *prev = &gt->prev_cells[cell_idx];
		bool new_overflow = (glyph->glyph->width == 2 && posx + 1 < txt->cols);

		if (use_diff) {
			if (prev->overflow && !new_overflow && posx + 1 < txt->cols) {
				struct tsm_screen_attr a = *attr;
				a.inverse = false;
				memcpy(&a.fr, &a.bg, 3);
				float gl_x1, gl_x2, gl_y1, gl_y2;
				gl_x1 = gt->advance_x * (posx + 1) - 1.0;
				gl_x2 = gl_x1 + width * gt->advance_x;
				gl_y1 = 1.0 - gt->advance_y * posy;
				gl_y2 = gl_y1 - gt->advance_y;
				atlas_push(gt, atlas, gl_x1, gl_x2, gl_y1, gl_y2, width, glyph, &a);
			}

			/* Skip if cell unchanged */
			if (!gt->need_clear && prev->id == id && prev->overflow == new_overflow &&
			    !memcmp(&prev->attr, attr, sizeof(*attr))) {
				return 0;
			}
		}

		/* Update cell state */
		prev->id = id;
		prev->attr = *attr;
		prev->overflow = new_overflow;
	}
	float gl_x1, gl_x2, gl_y1, gl_y2;
	gl_x1 = gt->advance_x * posx - 1.0;
	gl_x2 = gl_x1 + width * gt->advance_x;
	gl_y1 = 1.0 - gt->advance_y * posy;
	gl_y2 = gl_y1 - gt->advance_y;
	return atlas_push(gt, atlas, gl_x1, gl_x2, gl_y1, gl_y2, width, glyph, attr);
}

static int gltex_draw_pointer_immediate(struct kmscon_text *txt, unsigned int x, unsigned int y)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct glyph *glyph;
	float gl_x1, gl_x2, gl_y1, gl_y2;
	unsigned int sw, sh;
	int ret;
	uint32_t ch = 'I';
	uint64_t id = ch;

	ret = find_glyph(txt, &glyph, id, &ch, 1, &gt->attr);
	if (ret)
		return ret;

	atlas = glyph->atlas;

	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		sw = gt->sw;
		sh = gt->sh;
	} else {
		sw = gt->sh;
		sh = gt->sw;
	}

	if (x > sw)
		x = sw;

	if (y > sh)
		y = sh;

	gl_x1 = x * 2.0 / sw - 1.0 - gt->advance_x / 2.0;
	gl_y1 = 1.0 - y * 2.0 / sh + gt->advance_y / 2.0;
	gl_x2 = gl_x1 + gt->advance_x;
	gl_y2 = gl_y1 - gt->advance_y;

	return atlas_push(gt, atlas, gl_x1, gl_x2, gl_y1, gl_y2, 1, glyph, &gt->attr);
}

static int gltex_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y)
{
	struct gltex *gt = txt->data;
	int ret;

	gt->cursor_x = x;
	gt->cursor_y = y;
	if (gt->use_offscreen)
		return gltex_draw_pointer_immediate(txt, x, y);

	ret = uterm_display_move_cursor(txt->disp, x, y);
	if (ret == -EOPNOTSUPP)
		return gltex_draw_pointer_immediate(txt, x, y);
	return 0;

	// /* No FBO: draw cursor immediately */
	// return gltex_draw_pointer_immediate(txt, x, y);
}

/**
 * Enable or disable offscreen rendering for the GL renderer.
 *
 * Call order:
 * - update_hw_cursor_all() chooses whether offscreen rendering is needed
 * - kmscon_text_set_offscreen() forwards to this backend hook
 * - gltex_create_offscreen()/gltex_destroy_offscreen() manage GPU resources
 *
 * @param txt Text renderer instance.
 * @param enable Whether offscreen rendering should be enabled.
 */
static void gltex_set_offscreen(struct kmscon_text *txt, bool enable)
{
	struct gltex *gt = txt->data;
	bool want = enable;

	if (want && !gt->glBlitFramebufferFunc) {
		GLTEX_LOG_WARNING("offscreen requested without blit support");
		want = false;
	}

	if (gt->use_offscreen == want) {
		txt->offscreen = want;
		return;
	}

	gt->use_offscreen = want;
	txt->offscreen = want;
	if (!want) {
		gltex_destroy_offscreen(gt);
	}
	if (want && !gt->offscreen_fbo) {
		if (gltex_create_offscreen(gt) < 0)
			GLTEX_LOG_WARNING("offscreen requested but FBO creation failed");
	}
	gt->need_clear = true;
	if (gt->prev_cells && gt->num_cells > 0)
		memset(gt->prev_cells, 0, sizeof(*gt->prev_cells) * gt->num_cells);

	GLTEX_LOG_INFO("offscreen %s", want ? "enabled" : "disabled");
}

static void gltex_damage_cell(struct kmscon_text *txt, unsigned int posx, unsigned int posy)
{
	struct gltex *gt = txt->data;
	unsigned int idx;

	if (!gt->prev_cells || gt->num_cells == 0)
		return;
	if (posx >= txt->cols || posy >= txt->rows)
		return;

	idx = posy * txt->cols + posx;
	if (idx >= gt->num_cells)
		return;

	gt->prev_cells[idx].id = ~0ULL;
	gt->prev_cells[idx].overflow = false;
}
static int render_framebuffer(struct gltex *gt, GLuint fbo)
{
	struct atlas *atlas;
	struct shl_dlist *iter;
	float mat[16];

	gl_clear_error();
	if (!gt->need_clear && gt->num_cells == 0) {
		GLTEX_LOG_WARNING("No atlas:Skip rendering");
		return 0;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	gl_shader_use(gt->shader);
	glViewport(0, 0, gt->sw, gt->sh);
	glDisable(GL_BLEND);

	gl_m4_identity(mat);
	glUniformMatrix4fv(gt->uni_proj, 1, GL_FALSE, mat);
	glUniform1f(gt->uni_cos, gt->cos);
	glUniform1f(gt->uni_sin, gt->sin);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gt->uni_atlas, 0);

	shl_dlist_for_each(iter, &gt->atlases)
	{
		atlas = shl_dlist_entry(iter, struct atlas, list);
		if (!atlas->cache_num)
			continue;

		glBindTexture(GL_TEXTURE_2D, atlas->tex);
		glUniform1f(gt->uni_advance_htex, atlas->advance_htex);
		glUniform1f(gt->uni_advance_vtex, atlas->advance_vtex);

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, atlas->cache_pos);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, atlas->cache_texpos);
		glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, atlas->cache_fgcol);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 0, atlas->cache_bgcol);
		glDrawArrays(GL_TRIANGLES, 0, 6 * atlas->cache_num);
	}

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (gl_has_error(gt->shader)) {
		GLTEX_LOG_WARNING("rendering console caused OpenGL errors");
		return -EFAULT;
	}
	return 0;
}
static int gltex_render(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret;

	if (!gt->use_offscreen || !gt->offscreen_fbo)
		return render_framebuffer(gt, 0);

	ret = render_framebuffer(gt, gt->offscreen_fbo);
	if (ret)
		return ret;

	if (gt->glBlitFramebufferFunc) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER_NV, gt->offscreen_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER_NV, 0);
		gt->glBlitFramebufferFunc(0, 0, gt->sw, gt->sh, 0, 0, gt->sw, gt->sh,
					  GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	} else {
		GLTEX_LOG_WARNING("FBO enabled but no blit function - should not happen");
	}

	if (gl_has_error(gt->shader)) {
		GLTEX_LOG_WARNING("rendering console caused OpenGL errors");
		return -EFAULT;
	}

	return 0;
}

struct kmscon_text_ops kmscon_text_gltex_ops = {
	.name = "gltex",
	.owner = NULL,
	.init = gltex_init,
	.destroy = gltex_destroy,
	.set = gltex_set,
	.unset = gltex_unset,
	.rotate = gltex_rotate,
	.prepare = gltex_prepare,
	.draw = gltex_draw,
	.draw_pointer = gltex_draw_pointer,
	.damage_cell = gltex_damage_cell,
	.set_offscreen = gltex_set_offscreen,
	.render = gltex_render,
	.abort = NULL,
};
