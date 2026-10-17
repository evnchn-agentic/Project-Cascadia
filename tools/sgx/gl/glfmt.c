/* glfmt -- texture formats beyond 8-bit RGBA through the driver (docs/
 * research/p105-mesa.md, M43): float and half float textures
 * (ARB_texture_float), signed (EXT_texture_snorm), 16-bit and 10-bit
 * channels, the shared exponent format (EXT_texture_shared_exponent).  A
 * 2 x 2 texture each, its texels sampled at their centres (and between
 * them, filtered), brought into [0, 1] by the program -- value * scale +
 * bias -- so that values past [0, 1] can be seen on an RGBA8 target, and
 * read back against what GL says the format holds.  A format the context
 * has not is said, not counted.
 *
 *   sgx-gl glfmt [CASE]        (CASE: a name's start; all by default)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static uint8_t px[W * H * 4];
static int cases, failed, skipped;
static const char *only;
static GLuint prog;

/* how a format keeps a value: what GL hands back for it */
enum keep { F32, F16, SNORM8, UNORM16, UNORM10_2, E5 };

struct fmt_case {
	const char *name;
	GLenum ifmt, fmt;
	enum keep keep;
	int channels;        /* the internal format's: 1 L or A or I by kind, 3 RGB, 4 RGBA */
	char kind;           /* 'c' colour, 'l' luminance, 'a' alpha, 'L' luminance alpha, 'i' intensity */
};

static const struct fmt_case fmts[] = {
	{ "rgba32f", GL_RGBA32F_ARB, GL_RGBA, F32, 4, 'c' },
	{ "rgba16f", GL_RGBA16F_ARB, GL_RGBA, F16, 4, 'c' },
	{ "rgb32f", GL_RGB32F_ARB, GL_RGB, F32, 3, 'c' },
	{ "rgb16f", GL_RGB16F_ARB, GL_RGB, F16, 3, 'c' },
	{ "alpha16f", GL_ALPHA16F_ARB, GL_RGBA, F16, 1, 'a' },
	{ "luminance32f", GL_LUMINANCE32F_ARB, GL_RGBA, F32, 1, 'l' },
	{ "lum-alpha16f", GL_LUMINANCE_ALPHA16F_ARB, GL_RGBA, F16, 2, 'L' },
	{ "intensity16f", GL_INTENSITY16F_ARB, GL_RGBA, F16, 1, 'i' },
	{ "rgba8-snorm", GL_RGBA8_SNORM, GL_RGBA, SNORM8, 4, 'c' },
	{ "rgba16", GL_RGBA16, GL_RGBA, UNORM16, 4, 'c' },
	{ "rgb10-a2", GL_RGB10_A2, GL_RGBA, UNORM10_2, 4, 'c' },
	{ "rgb9-e5", GL_RGB9_E5_EXT, GL_RGB, E5, 3, 'c' },
};

static float
half(float x)
{
	/* (round to F16's 11 bits of mantissa; no subnormals needed here) */
	int e;
	float m = frexpf(x, &e);

	return ldexpf(roundf(m * 2048) / 2048, e);
}

/* what the format keeps of v, channel c (3: alpha) */
static float
kept(enum keep k, int c, float v)
{
	switch (k) {
	case F32:      return v;
	case F16:      return half(v);
	case SNORM8:   return roundf(fminf(fmaxf(v, -1), 1) * 127) / 127;
	case UNORM16:  return roundf(fminf(fmaxf(v, 0), 1) * 65535) / 65535;
	case UNORM10_2:
		return c == 3 ? roundf(fminf(fmaxf(v, 0), 1) * 3) / 3 :
		                roundf(fminf(fmaxf(v, 0), 1) * 1023) / 1023;
	case E5:       return fmaxf(v, 0);
	}
	return v;
}

/* what a lookup of the texel gives: the channels the format has, as GL's
 * base formats spread them (RGB: alpha 1; luminance L L L 1; ...) */
static void
looked_up(const struct fmt_case *f, const float t[4], float out[4])
{
	float r = kept(f->keep, 0, t[0]), g = kept(f->keep, 1, t[1]);
	float b = kept(f->keep, 2, t[2]), a = kept(f->keep, 3, t[3]);

	switch (f->kind) {
	case 'l': out[0] = out[1] = out[2] = r, out[3] = 1; break;
	case 'a': out[0] = out[1] = out[2] = 0, out[3] = kept(f->keep, 3, t[0]); break;
	case 'L': out[0] = out[1] = out[2] = r, out[3] = kept(f->keep, 3, t[1]); break;
	case 'i': out[0] = out[1] = out[2] = out[3] = r; break;
	default:
		out[0] = r, out[1] = g, out[2] = b, out[3] = f->channels == 4 ? a : 1;
	}
}

static int
to8(float x)
{
	return (int)(fminf(fmaxf(x, 0), 1) * 255 + 0.5f);
}

static GLuint
program(void)
{
	static const char *vs =
		"#version 120\n"
		"attribute vec4 a_position;\n"
		"varying vec2 v;\n"
		"void main() { gl_Position = a_position; v = a_position.xy * 0.5 + 0.5; }\n";
	static const char *fs =
		"#version 120\n"
		"varying vec2 v;\nuniform sampler2D t;\nuniform vec4 scale, bias;\n"
		"void main() { gl_FragColor = texture2D(t, v) * scale + bias; }\n";
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	char log[512];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glfmt: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	return p;
}

static void
quad(void)
{
	static const float v[] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* One case: a 2 x 2 texture of the four texels t (RGBA each, as floats),
 * scaled and biased into [0, 1]; filtered: GL_LINEAR, and each pixel then
 * what the four texels around its sample point give */
static void
run(const struct fmt_case *f, const char *what, const float t[4][4], float scale, float bias,
    int filtered, int tolerance)
{
	char name[64];
	float data[4][4], tex[4][4];
	int bad = 0, at = -1;
	float want[4];
	GLuint id;

	snprintf(name, sizeof(name), "%s %s", f->name, what);
	if (only && strncmp(name, only, strlen(only)))
		return;
	/* the data as the format's upload takes it: its channels in order */
	for (int i = 0; i < 4; i++)
		for (int c = 0; c < 4; c++)
			data[i][c] = t[i][c];
	if (f->fmt == GL_RGB)
		for (int i = 0; i < 4; i++)
			memcpy(&((float *)data)[3 * i], t[i], 3 * sizeof(float));
	/* (luminance, alpha, intensity: a value a texel; luminance alpha two) */
	if (f->kind != 'c')
		for (int i = 0; i < 4; i++)
			for (int c = 0; c < (f->kind == 'L' ? 2 : 1); c++)
				((float *)data)[(f->kind == 'L' ? 2 : 1) * i + c] = t[i][c];
	while (glGetError())
		;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexImage2D(GL_TEXTURE_2D, 0, f->ifmt, 2, 2, 0, f->kind == 'c' ? f->fmt :
	             f->kind == 'a' ? GL_ALPHA : f->kind == 'L' ? GL_LUMINANCE_ALPHA : GL_LUMINANCE,
	             GL_FLOAT, data);
	if (glGetError()) {
		printf("%-30s not here\n", name);
		skipped++;
		glDeleteTextures(1, &id);
		return;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtered ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtered ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glUniform4f(glGetUniformLocation(prog, "scale"), scale, scale, scale, scale);
	glUniform4f(glGetUniformLocation(prog, "bias"), bias, bias, bias, bias);
	quad();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < 4; i++)
		looked_up(f, t[i], tex[i]);
	for (int i = 0; i < W * H; i++) {
		float s = (i % W + 0.5f) / W, u = (i / W + 0.5f) / H;
		int off = 0;

		if (filtered) {
			/* (texel centres at 0.25 and 0.75; clamped outside them) */
			float fx = fminf(fmaxf(s * 2 - 0.5f, 0), 1), fy = fminf(fmaxf(u * 2 - 0.5f, 0), 1);

			for (int c = 0; c < 4; c++) {
				float lo = tex[0][c] + (tex[1][c] - tex[0][c]) * fx;
				float hi = tex[2][c] + (tex[3][c] - tex[2][c]) * fx;

				want[c] = (lo + (hi - lo) * fy) * scale + bias;
			}
		} else {
			/* (pixels near a texel's edge left out) */
			if (fabsf(s - 0.5f) < 1.5f / W || fabsf(u - 0.5f) < 1.5f / H)
				continue;
			for (int c = 0; c < 4; c++)
				want[c] = tex[(u > 0.5f) * 2 + (s > 0.5f)][c] * scale + bias;
		}
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(want[c])) > tolerance;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	glDeleteTextures(1, &id);
	cases++;
	if (!bad) {
		printf("%-30s ok\n", name);
		return;
	}
	failed++;
	printf("%-30s WRONG: %d pixels, the first at %d, %d: %d %d %d %d\n", name, bad, at % W, at / W,
	       px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3]);
}

int
main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	/* values past [0, 1] (scale 1/50, bias 0.5), within it (exactly, as
	 * far as 8 bits show), and for the formats that keep no sign */
	static const float range[4][4] = {
		{ -20, 12.5f, 3.25f, 1 }, { 18, -7.75f, 0.5f, -2 },
		{ 0.25f, 24, -16, 9 }, { -1.5f, 2, 20, -24 },
	};
	static const float unit[4][4] = {
		{ 0.1f, 0.9f, 0.5f, 1 }, { 0.75f, 0.25f, 0.6f, 0.2f },
		{ 0.33f, 0.66f, 0.05f, 0.95f }, { 1, 0, 0.45f, 0.55f },
	};
	static const float signed_unit[4][4] = {
		{ -0.8f, 0.9f, -0.5f, 1 }, { 0.75f, -0.25f, 0.6f, -1 },
		{ -0.33f, 0.66f, 0.05f, -0.95f }, { 1, 0, -0.45f, 0.55f },
	};
	static const float positive[4][4] = {
		{ 20, 12.5f, 3.25f, 1 }, { 18, 7.75f, 0.5f, 2 },
		{ 0.25f, 24, 16, 9 }, { 1.5f, 2, 20, 24 },
	};
	GLuint rt, fbo;
	EGLDisplay dpy;
	EGLContext ctx;

	only = argc > 1 ? argv[1] : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
		return fprintf(stderr, "glfmt: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glfmt: no OpenGL 2.1 context\n"), 2;
	printf("%s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fprintf(stderr, "glfmt: the target is not complete\n"), 2;
	glViewport(0, 0, W, H);
	prog = program();
	glActiveTexture(GL_TEXTURE0);

	for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
		const struct fmt_case *f = &fmts[i];

		if (f->keep == F32 || f->keep == F16) {
			run(f, "range", range, 1 / 50.0f, 0.5f, 0, 2);
			run(f, "unit", unit, 1, 0, 0, 2);
			run(f, "filtered", unit, 1, 0, 1, 3);
		} else if (f->keep == SNORM8) {
			run(f, "unit", signed_unit, 0.5f, 0.5f, 0, 2);
			run(f, "filtered", signed_unit, 0.5f, 0.5f, 1, 3);
		} else if (f->keep == E5) {
			run(f, "range", positive, 1 / 25.0f, 0, 0, 2);
			run(f, "unit", unit, 1, 0, 0, 2);
		} else {
			run(f, "unit", unit, 1, 0, 0, 2);
			run(f, "filtered", unit, 1, 0, 1, 3);
		}
	}
	printf("%d of %d cases right (%d formats not here)\n", cases - failed, cases, skipped);
	return failed ? 1 : 0;
}
