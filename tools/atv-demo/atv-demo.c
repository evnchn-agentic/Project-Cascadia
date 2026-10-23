/*
 * atv-demo: a remote-driven show-and-tell app for Linux on the Apple TV 3 (J33i).
 *
 *   atv-demo [-a ASSETDIR]                  run on /dev/fb0, input from "Apple TV IR remote"
 *   atv-demo -a DIR --render-screens OUT    host preview: every screen as OUT/NN-name.ppm
 *   atv-demo --write-clip FILE.wav          the Sound card's clip, to listen to it on any machine
 *   atv-demo --restore-tty                  console back to text mode (stage 2, after a crash)
 *
 * Draws into a 1920x1080 back buffer (a8r8g8b8, premultiplied), copies only what
 * changed to the framebuffer.  The GPU and sound cards run the existing tools as
 * children (sgx2d sprites/demo2, aplay) and keep reading the remote meanwhile.
 * Assets (fonts.bin, qr.txt) come from gen-assets.py; the sound clip is made here.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

#define W 1920
#define H 1080
#define SAFE 96 /* TV overscan margin */

/* ------------------------------------------------------------ colours --- */
#define RGB(r, g, b) (0xff000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
/* brutalist: near-black, white 2 px frames, one cyan, black ink on cyan (as nicegui.evnchn.io) */
static const uint32_t C_BG = RGB(0x0a, 0x0a, 0x0a);
static const uint32_t C_TEXT = RGB(0xf5, 0xf5, 0xf5);
static const uint32_t C_SOFT = RGB(0xc4, 0xc4, 0xc4);
static const uint32_t C_DIM = RGB(0x7a, 0x7a, 0x7a);
static const uint32_t C_FAINT = RGB(0x4a, 0x4a, 0x4a);
static const uint32_t C_CARD = RGB(0x12, 0x12, 0x12);
static const uint32_t C_CARD2 = RGB(0x1c, 0x1c, 0x1c);
static const uint32_t C_LINE = RGB(0x2e, 0x2e, 0x2e);
static const uint32_t C_ACCENT = RGB(0x00, 0xff, 0xff);
static const uint32_t C_INK = RGB(0x0a, 0x0a, 0x0a);
static const uint32_t C_WARN = RGB(0xff, 0xd4, 0x00);

/* ------------------------------------------------------------ surface --- */
typedef struct { int w, h; uint32_t *px; } surf;
typedef struct { int x0, y0, x1, y1; } rect;
static surf canvas, bgbase, bgcache;
static rect clip = {0, 0, W, H};

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "atv-demo: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static surf surf_new(int w, int h)
{
	surf s = {w, h, calloc((size_t)w * h, 4)};
	if (!s.px)
		die("out of memory (%dx%d)", w, h);
	return s;
}

static inline unsigned div255(unsigned x) { return (x + 1 + (x >> 8)) >> 8; }

/* premultiplied "over": colour c (straight RGB) at coverage a 0..255 */
static inline void over(uint32_t *d, uint32_t c, unsigned a)
{
	if (a == 0)
		return;
	if (a >= 255) {
		*d = c | 0xff000000u;
		return;
	}
	uint32_t v = *d, ia = 255 - a;
	unsigned r = div255(((c >> 16) & 255) * a + ((v >> 16) & 255) * ia);
	unsigned g = div255(((c >> 8) & 255) * a + ((v >> 8) & 255) * ia);
	unsigned b = div255((c & 255) * a + (v & 255) * ia);
	unsigned al = a + div255((v >> 24) * ia);
	*d = (al << 24) | (r << 16) | (g << 8) | b;
}

/* premultiplied source pixel over destination, with an extra global alpha */
static inline void over_pm(uint32_t *d, uint32_t s, unsigned ga)
{
	unsigned sa = s >> 24;
	if (ga < 255) {
		sa = div255(sa * ga);
		s = (sa << 24) | (div255(((s >> 16) & 255) * ga) << 16) | (div255(((s >> 8) & 255) * ga) << 8) |
		    div255((s & 255) * ga);
	}
	if (sa == 0)
		return;
	if (sa == 255) {
		*d = s;
		return;
	}
	uint32_t v = *d, ia = 255 - sa;
	unsigned r = ((s >> 16) & 255) + div255(((v >> 16) & 255) * ia);
	unsigned g = ((s >> 8) & 255) + div255(((v >> 8) & 255) * ia);
	unsigned b = (s & 255) + div255((v & 255) * ia);
	unsigned al = sa + div255((v >> 24) * ia);
	*d = (al << 24) | (r << 16) | (g << 8) | b;
}

static surf *tgt = &canvas;
static int cx0, cy0, cx1, cy1; /* effective clip on tgt */
static void set_target(surf *s)
{
	tgt = s;
	if (s == &canvas || s == &bgcache) {
		cx0 = clip.x0; cy0 = clip.y0; cx1 = clip.x1; cy1 = clip.y1;
	} else {
		cx0 = 0; cy0 = 0; cx1 = s->w; cy1 = s->h;
	}
}
static void set_clip(int x0, int y0, int x1, int y1)
{
	clip = (rect){x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, x1 > W ? W : x1, y1 > H ? H : y1};
	set_target(tgt);
}
static void clip_all(void) { set_clip(0, 0, W, H); }

static void fill_rect(int x, int y, int w, int h, uint32_t c, unsigned a)
{
	int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
	int x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = tgt->px + (size_t)yy * tgt->w;
		for (int xx = x0; xx < x1; xx++)
			over(p + xx, c, a);
	}
}

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* signed distance to a rounded box centred at (cx,cy), half size (hx,hy), radius r */
static inline float sd_rrect(float px, float py, float cx, float cy, float hx, float hy, float r)
{
	float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float in = qx > qy ? qx : qy;
	return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

/*
 * Rounded rectangle: filled (t <= 0) or a stroke t wide on the inside of the
 * edge; glow > 0 adds a soft falloff that far outside (shadows, halos).
 * Pixels well inside a filled shape skip the distance maths.
 */
static void shape_rrect(float x, float y, float w, float h, float r, float t, float glow, uint32_t c, unsigned a)
{
	float hx = w / 2, hy = h / 2, ccx = x + hx, ccy = y + hy;
	int x0 = (int)floorf(x - glow - 1), y0 = (int)floorf(y - glow - 1);
	int x1 = (int)ceilf(x + w + glow + 1), y1 = (int)ceilf(y + h + glow + 1);
	if (x0 < cx0) x0 = cx0;
	if (y0 < cy0) y0 = cy0;
	if (x1 > cx1) x1 = cx1;
	if (y1 > cy1) y1 = cy1;
	float inner = (t > 0 ? t : 0) + 1.5f;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = tgt->px + (size_t)yy * tgt->w;
		float py = yy + 0.5f;
		int mid = py > y + r + 1 && py < y + h - r - 1;
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f;
			if (t <= 0 && glow <= 0 && px > x + r + 1 && px < x + w - r - 1 && py > y + 1 && py < y + h - 1) {
				/* interior span */
				int e = (int)(x + w - r - 1);
				if (e > x1) e = x1;
				if (e > xx + 1) {
					for (; xx < e; xx++)
						over(p + xx, c, a);
					xx--;
					continue;
				}
			}
			if (t > 0 && glow <= 0 && mid && px > x + inner && px < x + w - inner) {
				int e = (int)(x + w - inner);
				if (e > x1) e = x1;
				if (e > xx + 1) {
					xx = e - 1;
					continue;
				}
			}
			float d = sd_rrect(px, py, ccx, ccy, hx, hy, r);
			float cov;
			if (glow > 0) {
				cov = d <= 0 ? 1.0f : expf(-(d * d) / (glow * glow * 0.35f));
				if (t > 0 && d < -t)
					cov = 0;
			} else if (t > 0) {
				cov = clampf(0.5f - d, 0, 1) - clampf(0.5f - (d + t), 0, 1);
			} else {
				cov = clampf(0.5f - d, 0, 1);
			}
			if (cov > 0)
				over(p + xx, c, (unsigned)(cov * a + 0.5f));
		}
	}
}
static void fill_rrect(float x, float y, float w, float h, float r, uint32_t c, unsigned a)
{
	shape_rrect(x, y, w, h, r, 0, 0, c, a);
}
static void stroke_rrect(float x, float y, float w, float h, float r, float t, uint32_t c, unsigned a)
{
	shape_rrect(x, y, w, h, r, t, 0, c, a);
}

/* circle / ring / arc and a thick line, by distance */
static void shape_circle(float cx, float cy, float r, float t, float a0, float a1, uint32_t c, unsigned a)
{
	int x0 = (int)(cx - r - 2), x1 = (int)(cx + r + 2), y0 = (int)(cy - r - 2), y1 = (int)(cy + r + 2);
	if (x0 < cx0) x0 = cx0;
	if (y0 < cy0) y0 = cy0;
	if (x1 > cx1) x1 = cx1;
	if (y1 > cy1) y1 = cy1;
	int arc = a1 > a0;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy;
			float dd = sqrtf(dx * dx + dy * dy) - r, cov;
			if (t > 0)
				cov = clampf(0.5f - dd, 0, 1) - clampf(0.5f - (dd + t), 0, 1);
			else
				cov = clampf(0.5f - dd, 0, 1);
			if (cov <= 0)
				continue;
			if (arc) {
				float ang = atan2f(-dy, dx);
				if (ang < 0)
					ang += 2 * (float)M_PI;
				if (ang < a0 || ang > a1)
					continue;
			}
			over(tgt->px + (size_t)yy * tgt->w + xx, c, (unsigned)(cov * a));
		}
}
static void line(float ax, float ay, float bx, float by, float th, uint32_t c, unsigned a)
{
	float r = th / 2;
	int x0 = (int)(fminf(ax, bx) - r - 2), x1 = (int)(fmaxf(ax, bx) + r + 2);
	int y0 = (int)(fminf(ay, by) - r - 2), y1 = (int)(fmaxf(ay, by) + r + 2);
	if (x0 < cx0) x0 = cx0;
	if (y0 < cy0) y0 = cy0;
	if (x1 > cx1) x1 = cx1;
	if (y1 > cy1) y1 = cy1;
	float vx = bx - ax, vy = by - ay, ll = vx * vx + vy * vy;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f - ax, py = yy + 0.5f - ay;
			float h = ll > 0 ? clampf((px * vx + py * vy) / ll, 0, 1) : 0;
			float dx = px - vx * h, dy = py - vy * h;
			float cov = clampf(r + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1);
			if (cov > 0)
				over(tgt->px + (size_t)yy * tgt->w + xx, c, (unsigned)(cov * a));
		}
}
static void triangle(float ax, float ay, float bx, float by, float qx, float qy, uint32_t c, unsigned a)
{
	int x0 = (int)fminf(ax, fminf(bx, qx)) - 1, x1 = (int)fmaxf(ax, fmaxf(bx, qx)) + 2;
	int y0 = (int)fminf(ay, fminf(by, qy)) - 1, y1 = (int)fmaxf(ay, fmaxf(by, qy)) + 2;
	if (x0 < cx0) x0 = cx0;
	if (y0 < cy0) y0 = cy0;
	if (x1 > cx1) x1 = cx1;
	if (y1 > cy1) y1 = cy1;
	float P[3][2] = {{ax, ay}, {bx, by}, {qx, qy}};
	float area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
	float s = area < 0 ? -1.f : 1.f;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f, py = yy + 0.5f, m = 1e9f;
			for (int i = 0; i < 3; i++) {
				float *e0 = P[i], *e1 = P[(i + 1) % 3];
				float ex = e1[0] - e0[0], ey = e1[1] - e0[1], len = sqrtf(ex * ex + ey * ey);
				float dist = s * ((px - e0[0]) * ey - (py - e0[1]) * ex) / len;
				if (-dist < m)
					m = -dist;
			}
			float cov = clampf(m + 0.5f, 0, 1);
			if (cov > 0)
				over(tgt->px + (size_t)yy * tgt->w + xx, c, (unsigned)(cov * a));
		}
}

/* a square frame t pixels wide, on the inside of the box */
static void box(float fx, float fy, float fw, float fh, int t, uint32_t c, unsigned a)
{
	int x = (int)fx, y = (int)fy, w = (int)fw, h = (int)fh;
	fill_rect(x, y, w, t, c, a);
	fill_rect(x, y + h - t, w, t, c, a);
	fill_rect(x, y + t, t, h - 2 * t, c, a);
	fill_rect(x + w - t, y + t, t, h - 2 * t, c, a);
}

/* a w x h block turned deg degrees about its centre (the tilted badge) */
static void tilted(float cx, float cy, float w, float h, float deg, uint32_t c, unsigned a)
{
	float r = deg * (float)M_PI / 180, ca = cosf(r), sa = sinf(r);
	static const float sg[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	float P[4][2], x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	for (int i = 0; i < 4; i++) {
		float dx = sg[i][0] * w / 2, dy = sg[i][1] * h / 2;
		P[i][0] = cx + dx * ca - dy * sa;
		P[i][1] = cy + dx * sa + dy * ca;
		x0 = fminf(x0, P[i][0]), x1 = fmaxf(x1, P[i][0]), y0 = fminf(y0, P[i][1]), y1 = fmaxf(y1, P[i][1]);
	}
	int X0 = (int)x0 - 1, X1 = (int)x1 + 2, Y0 = (int)y0 - 1, Y1 = (int)y1 + 2;
	if (X0 < cx0) X0 = cx0;
	if (Y0 < cy0) Y0 = cy0;
	if (X1 > cx1) X1 = cx1;
	if (Y1 > cy1) Y1 = cy1;
	for (int yy = Y0; yy < Y1; yy++)
		for (int xx = X0; xx < X1; xx++) {
			float px = xx + 0.5f, py = yy + 0.5f, m = 1e9f;
			for (int i = 0; i < 4; i++) {
				float *e0 = P[i], *e1 = P[(i + 1) % 4];
				float ex = e1[0] - e0[0], ey = e1[1] - e0[1], len = sqrtf(ex * ex + ey * ey);
				float d = ((px - e0[0]) * ey - (py - e0[1]) * ex) / len;
				if (-d < m)
					m = -d;
			}
			float cov = clampf(m + 0.5f, 0, 1);
			if (cov > 0)
				over(tgt->px + (size_t)yy * tgt->w + xx, c, (unsigned)(cov * a));
		}
}

static void blit(const surf *s, int x, int y, unsigned ga)
{
	int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
	int x1 = x + s->w > cx1 ? cx1 : x + s->w, y1 = y + s->h > cy1 ? cy1 : y + s->h;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *d = tgt->px + (size_t)yy * tgt->w;
		const uint32_t *sp = s->px + (size_t)(yy - y) * s->w - x;
		for (int xx = x0; xx < x1; xx++)
			over_pm(d + xx, sp[xx], ga);
	}
}

/* copy a rectangle of one full-screen surface into another (no blending) */
static void copy_rect(surf *dst, const surf *src, int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > W) x1 = W;
	if (y1 > H) y1 = H;
	for (int y = y0; y < y1; y++)
		memcpy(dst->px + (size_t)y * W + x0, src->px + (size_t)y * W + x0, (size_t)(x1 - x0) * 4);
}

/* -------------------------------------------------------------- fonts --- */
typedef struct { uint32_t cp; int16_t w, h, x0, y0; int32_t adv; uint32_t off; } glyph;
typedef struct { char id[9]; int px, asc, desc, n; glyph *g; uint8_t *bm; glyph *ascii[128]; } face;
static face faces[16];
static int nfaces;
enum { F_TITLE, F_HERO, F_LEAD, F_CARDT, F_CARDS, F_BODY, F_MED, F_BIG, F_LABEL, F_FOOT, F_SMALL, F_N };
static const char *face_ids[F_N] = {"title", "hero", "lead", "cardt", "cards", "body", "med", "big", "label", "foot", "small"};
static face *F[F_N];

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *b = malloc(n + 1);
	if (!b || fread(b, 1, n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	((char *)b)[n] = 0;
	if (len)
		*len = n;
	return b;
}

static void load_fonts(const char *dir)
{
	char path[600];
	size_t len;
	snprintf(path, sizeof path, "%.500s/fonts.bin", dir);
	uint8_t *b = slurp(path, &len), *p;
	if (!b || len < 8 || memcmp(b, "ATVF", 4))
		die("cannot read %s", path);
	uint32_t n;
	memcpy(&n, b + 4, 4);
	p = b + 8;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		face *f = &faces[nfaces++];
		memcpy(f->id, p, 8);
		f->id[8] = 0;
		uint16_t px, ng;
		int16_t asc, desc;
		memcpy(&px, p + 8, 2);
		memcpy(&asc, p + 10, 2);
		memcpy(&desc, p + 12, 2);
		memcpy(&ng, p + 14, 2);
		p += 16;
		f->px = px; f->asc = asc; f->desc = desc; f->n = ng;
		f->g = calloc(ng, sizeof(glyph));
		for (int k = 0; k < ng; k++, p += 20) {
			glyph *g = &f->g[k];
			memcpy(&g->cp, p, 4);
			memcpy(&g->w, p + 4, 2);
			memcpy(&g->h, p + 6, 2);
			memcpy(&g->x0, p + 8, 2);
			memcpy(&g->y0, p + 10, 2);
			memcpy(&g->adv, p + 12, 4);
			memcpy(&g->off, p + 16, 4);
			if (g->cp < 128)
				f->ascii[g->cp] = g;
		}
		uint32_t bl;
		memcpy(&bl, p, 4);
		f->bm = p + 4;
		p += 4 + bl;
	}
	for (int i = 0; i < F_N; i++) {
		for (int k = 0; k < nfaces; k++)
			if (!strcmp(faces[k].id, face_ids[i]))
				F[i] = &faces[k];
		if (!F[i])
			die("fonts.bin has no face '%s' (regenerate the assets)", face_ids[i]);
	}
}

static uint32_t utf8(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	if (c >= 0xf0 && p[0] && p[1] && p[2]) {
		c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
		p += 3;
	} else if (c >= 0xe0 && p[0] && p[1]) {
		c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
		p += 2;
	} else if (c >= 0xc0 && p[0]) {
		c = ((c & 31) << 6) | (p[0] & 63);
		p += 1;
	}
	*s = (const char *)p;
	return c;
}

static glyph *find_glyph(face *f, uint32_t cp)
{
	if (cp < 128)
		return f->ascii[cp] ? f->ascii[cp] : f->ascii['?'];
	for (int i = 0; i < f->n; i++)
		if (f->g[i].cp == cp)
			return &f->g[i];
	return f->ascii['?'];
}

/* width in pixels; track = extra letter spacing in px */
static float text_w(face *f, const char *s, float track)
{
	float x = 0;
	int n = 0;
	while (*s) {
		glyph *g = find_glyph(f, utf8(&s));
		if (g)
			x += g->adv / 64.0f + track;
		n++;
	}
	return n ? x - track : 0;
}

static float text(face *f, float x, float y, const char *s, uint32_t c, unsigned a, float track)
{
	while (*s) {
		glyph *g = find_glyph(f, utf8(&s));
		if (!g)
			continue;
		int gx = (int)floorf(x + 0.5f) + g->x0, gy = (int)y + g->y0;
		const uint8_t *bm = f->bm + g->off;
		for (int j = 0; j < g->h; j++) {
			int yy = gy + j;
			if (yy < cy0 || yy >= cy1)
				continue;
			uint32_t *d = tgt->px + (size_t)yy * tgt->w;
			for (int i = 0; i < g->w; i++) {
				int xx = gx + i;
				if (xx < cx0 || xx >= cx1)
					continue;
				unsigned cov = bm[j * g->w + i];
				if (cov)
					over(d + xx, c, a == 255 ? cov : div255(cov * a));
			}
		}
		x += g->adv / 64.0f + track;
	}
	return x;
}
static void text_c(face *f, float cx, float y, const char *s, uint32_t c, unsigned a)
{
	text(f, cx - text_w(f, s, 0) / 2, y, s, c, a, 0);
}
static void text_r(face *f, float rx, float y, const char *s, uint32_t c, unsigned a)
{
	text(f, rx - text_w(f, s, 0), y, s, c, a, 0);
}

/* word-wrapped paragraph; returns the y of the next line */
static float para(face *f, float x, float y, float maxw, float lh, const char *s, uint32_t c)
{
	char buf[512], word[128];
	buf[0] = 0;
	const char *p = s;
	while (1) {
		int n = 0;
		while (*p == ' ')
			p++;
		while (*p && *p != ' ' && n < 127)
			word[n++] = *p++;
		word[n] = 0;
		if (n) {
			char trial[640];
			snprintf(trial, sizeof trial, "%s%s%s", buf, buf[0] ? " " : "", word);
			if (buf[0] && text_w(f, trial, 0) > maxw) {
				text(f, x, y, buf, c, 255, 0);
				y += lh;
				snprintf(buf, sizeof buf, "%s", word);
			} else
				snprintf(buf, sizeof buf, "%.500s", trial);
		}
		if (!*p)
			break;
	}
	if (buf[0]) {
		text(f, x, y, buf, c, 255, 0);
		y += lh;
	}
	return y;
}

/* a key cap: the button's name in capitals in a 2 px frame, returns the x after it */
static float keycap(float x, float baseline, const char *k)
{
	face *f = F[F_LABEL];
	char u[64];
	int n = 0;
	for (; k[n] && n < 63; n++)
		u[n] = k[n] >= 'a' && k[n] <= 'z' ? k[n] - 32 : k[n];
	u[n] = 0;
	float w = text_w(f, u, 2.0f) + 28;
	box(x, baseline - 31, w, 42, 2, C_TEXT, 255);
	text(f, x + 14, baseline - 3, u, C_TEXT, 255, 2.0f);
	return x + w;
}

/* an uppercase label in a cyan block, ink on cyan; returns its width */
static float tag(float x, float baseline, const char *s, uint32_t bg, uint32_t fg)
{
	float w = text_w(F[F_LABEL], s, 2.0f) + 24;
	fill_rect((int)x, (int)baseline - 27, (int)w, 36, bg, 255);
	text(F[F_LABEL], x + 12, baseline - 3, s, fg, 255, 2.0f);
	return w;
}

/* ------------------------------------------------------------- assets --- */
static char assetdir[512] = "/usr/local/share/atv-demo";
static int preview;
static int qr_n;
static char qr_mod[64][64], qr_url[128];
typedef struct { float t; char name[9]; } chordmark;
static int viz_nf, viz_nc;
static float viz_fps = 30;
static uint8_t *viz_v;
static chordmark viz_ch[16];
static float clip_secs = 10;

static void load_assets(void)
{
	char path[600];
	snprintf(path, sizeof path, "%s/qr.txt", assetdir);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%d %127s", &qr_n, qr_url) == 2 && qr_n > 0 && qr_n <= 64) {
			for (int y = 0; y < qr_n; y++) {
				char row[80];
				if (fscanf(f, "%79s", row) != 1)
					break;
				for (int x = 0; x < qr_n; x++)
					qr_mod[y][x] = row[x] == '1';
			}
		} else
			qr_n = 0;
		fclose(f);
	}
}

/*
 * The sound card's clip, synthesized here rather than shipped: a WAV is 2 MB the
 * image would carry (and it would not compress).  Cmaj7 - Am7 - Fmaj7 - G6 as an
 * arpeggio of eighths at 100 bpm, then a Cmaj9 left to ring; 48 kHz S16_LE stereo,
 * written to /tmp the first time the Sound card is opened.  The chromagram the
 * card shows is analysed from the same samples (Goertzel per semitone, folded into
 * 12 pitch classes, 30 frames a second).
 */
#define RATE 48000
static float *mixL, *mixR;
static int clip_n;
static char clip_path[256];
static int clip_ready;

static float note_hz(const char *n)
{
	static const int pc[7] = {9, 11, 0, 2, 4, 5, 7}; /* A B C D E F G */
	int k = pc[n[0] - 'A'], i = 1;
	if (n[1] == '#')
		k++, i++;
	int midi = 12 * (n[i] - '0' + 1) + k;
	return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

static void tone(float t0, float f, float dur, float amp, float pan, float decay)
{
	int n0 = (int)(t0 * RATE), n = (int)(dur * RATE);
	float gl = cosf(pan * (float)M_PI / 2), gr = sinf(pan * (float)M_PI / 2);
	double w = 2 * M_PI * f / RATE;
	/* oscillators and envelopes by recurrence: no sin/exp per sample */
	double c1 = cos(w), s1 = sin(w), x1 = 1, y1 = 0;
	double ed = exp(-1.0 / (decay * RATE)), e2 = exp(-1.0 / (0.25 * RATE)), e3 = exp(-1.0 / (0.08 * RATE));
	double env = 1, env2 = 1, env3 = 1;
	for (int i = 0; i < n && n0 + i < clip_n; i++) {
		double t = (double)i / RATE;
		double att = t < 0.006 ? t / 0.006 : 1;
		double sn = y1, cs = x1;
		double s2 = 2 * sn * cs, s3 = sn * (3 - 4 * sn * sn);
		double v = amp * att * env * (sn + 0.35 * env2 * s2 + 0.12 * env3 * s3);
		mixL[n0 + i] += (float)(v * gl);
		mixR[n0 + i] += (float)(v * gr);
		double nx = x1 * c1 - y1 * s1;
		y1 = y1 * c1 + x1 * s1;
		x1 = nx;
		env *= ed, env2 *= e2, env3 *= e3;
	}
}

static void make_clip(void)
{
	static const struct { const char *name, *bass, *arp[6]; } prog[4] = {
		{"Cmaj7", "C3", {"C4", "E4", "G4", "B4", "G4", "E4"}},
		{"Am7", "A2", {"A3", "C4", "E4", "G4", "E4", "C4"}},
		{"Fmaj7", "F2", {"F3", "A3", "C4", "E4", "C4", "A3"}},
		{"G6", "G2", {"G3", "B3", "D4", "E4", "D4", "B3"}},
	};
	static const char *fin[5] = {"C4", "E4", "G4", "B4", "D5"};
	const float eighth = 0.3f;
	float total = 4 * 6 * eighth + 3.2f, t = 0.25f;
	clip_n = (int)(total * RATE);
	mixL = calloc(clip_n, sizeof(float));
	mixR = calloc(clip_n, sizeof(float));
	if (!mixL || !mixR)
		return;
	viz_nc = 0;
	for (int c = 0; c < 4; c++) {
		viz_ch[viz_nc].t = t;
		snprintf(viz_ch[viz_nc++].name, 9, "%s", prog[c].name);
		tone(t, note_hz(prog[c].bass), 6 * eighth + 0.6f, 0.30f, 0.5f, 1.2f);
		for (int j = 0; j < 6; j++)
			tone(t + j * eighth, note_hz(prog[c].arp[j]), 1.6f, 0.22f, 0.25f + 0.25f * (j % 3), 0.7f);
		t += 6 * eighth;
	}
	viz_ch[viz_nc].t = t;
	snprintf(viz_ch[viz_nc++].name, 9, "Cmaj9");
	tone(t, note_hz("C3"), 3.0f, 0.30f, 0.5f, 1.4f);
	for (int j = 0; j < 5; j++)
		tone(t + j * 0.045f, note_hz(fin[j]), 3.0f, 0.18f, 0.2f + 0.15f * j, 1.1f);
	/* a little room: two feedback delays, crossed between the channels */
	const int dl[2] = {(int)(0.083 * RATE), (int)(0.131 * RATE)};
	const float dg[2] = {0.22f, 0.16f};
	for (int k = 0; k < 2; k++)
		for (int i = dl[k]; i < clip_n; i++) {
			mixL[i] += dg[k] * mixR[i - dl[k]];
			mixR[i] += dg[k] * mixL[i - dl[k]];
		}
	int fade = (int)(0.6 * RATE);
	float peak = 1e-6f;
	for (int i = 0; i < clip_n; i++) {
		if (i >= clip_n - fade) {
			float g = (float)(clip_n - 1 - i) / fade;
			mixL[i] *= g, mixR[i] *= g;
		}
		peak = fmaxf(peak, fmaxf(fabsf(mixL[i]), fabsf(mixR[i])));
	}
	float k = 0.5f / peak; /* -6 dBFS */
	for (int i = 0; i < clip_n; i++)
		mixL[i] *= k, mixR[i] *= k;
	clip_secs = (float)clip_n / RATE;

	/* the chromagram */
	viz_fps = 30;
	viz_nf = (int)(clip_secs * viz_fps);
	float *val = calloc((size_t)viz_nf * 12, sizeof(float)), top = 1e-12f;
	viz_v = calloc((size_t)viz_nf * 12, 1);
	if (!val || !viz_v)
		return;
	const int win = 4096;
	for (int fi = 0; fi < viz_nf; fi++) {
		int c = (int)((long)fi * RATE / 30), a = c - win / 2 < 0 ? 0 : c - win / 2, b = c + win / 2 > clip_n ? clip_n : c + win / 2;
		float *ch = val + fi * 12, m = 0;
		for (int midi = 45; midi < 84 && b - a > 64; midi++) {
			float coeff = 2 * cosf(2 * (float)M_PI * 440.0f * powf(2.0f, (midi - 69) / 12.0f) / RATE);
			float q1 = 0, q2 = 0;
			for (int i = a; i < b; i++) {
				float q0 = 0.5f * (mixL[i] + mixR[i]) + coeff * q1 - q2;
				q2 = q1, q1 = q0;
			}
			ch[midi % 12] += q1 * q1 + q2 * q2 - coeff * q1 * q2;
		}
		for (int i = 0; i < 12; i++)
			m = fmaxf(m, ch[i]);
		/* contrast: the chord's own notes stand out from the leakage around them */
		if (m > 0)
			for (int i = 0; i < 12; i++)
				ch[i] = ch[i] / m * ch[i] / m * m;
		top = fmaxf(top, m);
	}
	for (int i = 0; i < viz_nf * 12; i++) {
		float v = 255 * sqrtf(val[i] / top);
		viz_v[i] = v > 255 ? 255 : (uint8_t)v;
	}
	free(val);
}

static int write_clip(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	uint32_t data = (uint32_t)clip_n * 4, u32;
	uint16_t u16;
	fwrite("RIFF", 1, 4, f);
	u32 = 36 + data; fwrite(&u32, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	u32 = 16; fwrite(&u32, 4, 1, f);
	u16 = 1; fwrite(&u16, 2, 1, f);           /* PCM */
	u16 = 2; fwrite(&u16, 2, 1, f);           /* stereo */
	u32 = RATE; fwrite(&u32, 4, 1, f);
	u32 = RATE * 4; fwrite(&u32, 4, 1, f);
	u16 = 4; fwrite(&u16, 2, 1, f);
	u16 = 16; fwrite(&u16, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data, 4, 1, f);
	for (int i = 0; i < clip_n; i++) {
		int16_t s2[2] = {(int16_t)lrintf(mixL[i] * 32767), (int16_t)lrintf(mixR[i] * 32767)};
		fwrite(s2, 2, 2, f);
	}
	return fclose(f);
}

static void clip_prepare(void)
{
	if (clip_ready)
		return;
	make_clip();
	snprintf(clip_path, sizeof clip_path, "/tmp/atv-demo-chord.wav");
	if (!preview && write_clip(clip_path))
		fprintf(stderr, "atv-demo: cannot write %s: %s\n", clip_path, strerror(errno));
	clip_ready = 1;
}

/* --------------------------------------------------------------- time --- */
static double sim_now;
static double now_s(void)
{
	if (preview)
		return sim_now;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}
static float ease_out(float t) { t = clampf(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }

/* ------------------------------------------------------- system facts --- */
typedef struct {
	char kernel[96], model[64];
	double uptime;
	long mem_total_kb, mem_avail_kb;
	float cpu_pct, load1;
	float hist[60];
	int nhist;
	unsigned long long last_busy, last_total;
	int fbw, fbh;
} sysinfo_t;
static sysinfo_t si;

static void read_stats(void)
{
	if (preview) {
		snprintf(si.kernel, sizeof si.kernel, "Linux 6.12 (preview data)");
		snprintf(si.model, sizeof si.model, "AppleTV3,2");
		si.uptime = 3725; si.mem_total_kb = 495 * 1024; si.mem_avail_kb = 431 * 1024;
		si.cpu_pct = 14; si.load1 = 0.18f;
		si.nhist = 60;
		for (int i = 0; i < 60; i++)
			si.hist[i] = 8 + 6 * sinf(i * 0.35f) + (i > 40 ? 20 : 0) * expf(-(i - 40) * 0.2f) * (i > 40);
		si.fbw = W; si.fbh = H;
		return;
	}
	struct utsname u;
	if (!uname(&u))
		snprintf(si.kernel, sizeof si.kernel, "%.20s %.70s", u.sysname, u.release);
	char *dirty = strstr(si.kernel, "-dirty"); /* a local-tree build tag, not part of the version */
	if (dirty)
		*dirty = 0;
	char *m = slurp("/proc/device-tree/model", NULL);
	if (m) {
		snprintf(si.model, sizeof si.model, "%s", m);
		free(m);
	}
	FILE *f = fopen("/proc/uptime", "r");
	if (f) {
		if (fscanf(f, "%lf", &si.uptime) != 1)
			si.uptime = 0;
		fclose(f);
	}
	f = fopen("/proc/meminfo", "r");
	if (f) {
		char k[64];
		long v;
		while (fscanf(f, "%63s %ld kB\n", k, &v) == 2) {
			if (!strcmp(k, "MemTotal:")) si.mem_total_kb = v;
			if (!strcmp(k, "MemAvailable:")) si.mem_avail_kb = v;
		}
		fclose(f);
	}
	f = fopen("/proc/loadavg", "r");
	if (f) {
		if (fscanf(f, "%f", &si.load1) != 1)
			si.load1 = 0;
		fclose(f);
	}
	f = fopen("/proc/stat", "r");
	if (f) {
		unsigned long long v[8] = {0};
		if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
			   &v[7]) >= 4) {
			unsigned long long idle = v[3] + v[4], tot = 0;
			for (int i = 0; i < 8; i++)
				tot += v[i];
			unsigned long long busy = tot - idle;
			if (si.last_total && tot > si.last_total) {
				si.cpu_pct = 100.0f * (busy - si.last_busy) / (tot - si.last_total);
				if (si.nhist == 60)
					memmove(si.hist, si.hist + 1, 59 * sizeof(float)), si.nhist--;
				si.hist[si.nhist++] = si.cpu_pct;
			}
			si.last_busy = busy;
			si.last_total = tot;
		}
		fclose(f);
	}
}

/* ------------------------------------------------------------ children --- */
static pid_t child = -1;
static int child_pipe = -1;
static double child_t0;
static char child_out[256];
static int child_outn;
static int child_kind; /* 1 gpu, 2 sound */

static pid_t spawn(char *const argv[], int capture)
{
	if (preview)
		return -1;
	int pfd[2] = {-1, -1};
	if (capture && pipe(pfd))
		capture = 0;
	pid_t p = fork();
	if (p < 0)
		return -1;
	if (p == 0) {
		setpgid(0, 0);
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0);
		if (capture) {
			dup2(pfd[1], 1);
			dup2(pfd[1], 2);
			close(pfd[0]);
		} else {
			dup2(nul, 1);
		}
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execv(argv[0], argv);
		execvp(argv[0], argv);
		_exit(127);
	}
	setpgid(p, p);
	child_outn = 0;
	child_out[0] = 0;
	if (capture) {
		close(pfd[1]);
		child_pipe = pfd[0];
		fcntl(child_pipe, F_SETFL, O_NONBLOCK);
	}
	child = p;
	child_t0 = now_s();
	return p;
}

static void child_drain(void)
{
	if (child_pipe < 0)
		return;
	char b[256];
	ssize_t n;
	while ((n = read(child_pipe, b, sizeof b)) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (b[i] == '\n') {
				child_out[child_outn] = 0;
				if (child_outn)
					fprintf(stderr, "child: %s\n", child_out);
				/* keep the last non-empty line: sprites ends with its fps */
				if (child_outn)
					child_outn = -child_outn;
				else
					child_outn = 0;
				continue;
			}
			if (child_outn < 0)
				child_outn = 0;
			if (child_outn < (int)sizeof child_out - 1)
				child_out[child_outn++] = b[i];
		}
	}
}

/* returns 1 when the child has gone (status in *st) */
static int child_poll(int *st)
{
	if (child < 0)
		return 1;
	child_drain();
	pid_t r = waitpid(child, st, WNOHANG);
	if (r == child || (r < 0 && errno == ECHILD)) {
		child_drain();
		if (child_pipe >= 0)
			close(child_pipe);
		child_pipe = -1;
		child = -1;
		return 1;
	}
	return 0;
}

static void child_stop(void)
{
	if (child < 0)
		return;
	kill(-child, SIGTERM);
	kill(child, SIGTERM);
	int st;
	for (int i = 0; i < 30; i++) {
		if (child_poll(&st))
			return;
		usleep(50000);
	}
	kill(-child, SIGKILL);
	kill(child, SIGKILL);
	waitpid(child, &st, 0);
	child_poll(&st);
	child = -1;
}

/* ------------------------------------------------------------ display --- */
static uint8_t *fbmem;
static int fb_fd = -1, fb_stride, fb_w = W, fb_h = H, tty_fd = -1;

/* ATV_PERF=1: frame timings to stderr (render = drawing into the back buffer, fb = the copy out) */
static int perf;
static double perf_fb, perf_all;
static int perf_n;
static double perf_clock(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void present_copy(int x0, int y0, int x1, int y1);
static void present(int x0, int y0, int x1, int y1)
{
	double t0 = perf ? perf_clock() : 0;
	present_copy(x0, y0, x1, y1);
	if (perf)
		perf_fb += perf_clock() - t0;
}
static void present_copy(int x0, int y0, int x1, int y1)
{
	if (!fbmem)
		return;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > W) x1 = W;
	if (y1 > H) y1 = H;
	if (fb_w == W && fb_h == H) {
		for (int y = y0; y < y1; y++)
			memcpy(fbmem + (size_t)y * fb_stride + x0 * 4, canvas.px + (size_t)y * W + x0, (size_t)(x1 - x0) * 4);
		return;
	}
	/* another mode (atv_hdmi=720): nearest-neighbour scale of the region */
	int dx0 = x0 * fb_w / W, dx1 = (x1 * fb_w + W - 1) / W, dy0 = y0 * fb_h / H, dy1 = (y1 * fb_h + H - 1) / H;
	for (int y = dy0; y < dy1 && y < fb_h; y++) {
		uint32_t *d = (uint32_t *)(fbmem + (size_t)y * fb_stride);
		const uint32_t *s = canvas.px + (size_t)(y * H / fb_h) * W;
		for (int x = dx0; x < dx1 && x < fb_w; x++)
			d[x] = s[x * W / fb_w];
	}
}
static void present_all(void) { present(0, 0, W, H); }

static void display_restore(void)
{
#ifdef __linux__
	if (tty_fd >= 0) {
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
		close(tty_fd);
		tty_fd = -1;
	}
#endif
}

static void display_open(void)
{
#ifdef __linux__
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0)
		die("/dev/fb0: %s", strerror(errno));
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo fx;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fx))
		die("fb ioctl: %s", strerror(errno));
	if (v.bits_per_pixel != 32)
		die("fb is %d bpp, want 32", v.bits_per_pixel);
	fb_w = v.xres;
	fb_h = v.yres;
	fb_stride = fx.line_length;
	fbmem = mmap(NULL, (size_t)fb_stride * fb_h, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fbmem == MAP_FAILED)
		die("fb mmap: %s", strerror(errno));
	si.fbw = fb_w;
	si.fbh = fb_h;
	/* fbcon must not paint over us (cursor, kernel messages, blanking) */
	tty_fd = open("/dev/tty0", O_RDWR);
	if (tty_fd >= 0) {
		const char *noblank = "\033[9;0]\033[?25l";
		if (write(tty_fd, noblank, strlen(noblank)) < 0) {
		}
		if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS))
			fprintf(stderr, "atv-demo: KDSETMODE: %s\n", strerror(errno));
	}
	atexit(display_restore);
#endif
}

/* -------------------------------------------------------------- input --- */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_BACK, K_OK, K_PLAY };
#define MAXIN 8
static int in_fd[MAXIN], n_in;
static double in_scan_t;

#ifdef __linux__
static int has_key(int fd, int code)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1];
	memset(bits, 0, sizeof bits);
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0)
		return 0;
	return (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1;
}
#endif

static void input_scan(void)
{
#ifdef __linux__
	for (int i = 0; i < n_in; i++)
		close(in_fd[i]);
	n_in = 0;
	DIR *d = opendir("/dev/input");
	if (!d)
		return;
	struct dirent *e;
	static char seen[512];
	char now[512] = "";
	while ((e = readdir(d)) && n_in < MAXIN) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		char p[300], name[128] = "";
		snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
		int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGNAME(sizeof name), name);
		/* the remote by name; any other device with Enter and arrows (a keyboard) too */
		if (!strcmp(name, "Apple TV IR remote") || (has_key(fd, KEY_ENTER) && has_key(fd, KEY_LEFT))) {
			snprintf(now + strlen(now), sizeof now - strlen(now), "%s \"%s\" ", p, name);
			in_fd[n_in++] = fd;
		} else
			close(fd);
	}
	closedir(d);
	/* rescanned every 30 s: only a change is worth a log line */
	if (strcmp(now, seen)) {
		fprintf(stderr, "atv-demo: input %s\n", now[0] ? now : "none");
		snprintf(seen, sizeof seen, "%s", now);
	}
#endif
	in_scan_t = now_s();
}

static int map_key(int code)
{
#ifdef __linux__
	switch (code) {
	case KEY_UP: return K_UP;
	case KEY_DOWN: return K_DOWN;
	case KEY_LEFT: return K_LEFT;
	case KEY_RIGHT: return K_RIGHT;
	case KEY_BACK: case KEY_MENU: case KEY_ESC: case KEY_BACKSPACE: return K_BACK;
	case KEY_ENTER: case KEY_OK: case KEY_SELECT: case KEY_KPENTER: case KEY_SPACE: return K_OK;
	case KEY_PLAYPAUSE: case KEY_PLAY: case KEY_PAUSE: case KEY_P: return K_PLAY;
	}
	fprintf(stderr, "atv-demo: unmapped key code %d\n", code);
#else
	(void)code;
#endif
	return K_NONE;
}

/* --------------------------------------------------------------- state --- */
enum { S_HOME, S_GPU, S_SOUND, S_BOX, S_TRY, S_CONFIRM, S_REBOOT, S_GPURUN };
static int screen = S_HOME, dirty_full = 1;
static float focus = 0, focus_from = 0;
static int focus_to = 0;
static double focus_t0 = -10;
static int gpu_sel;
static float gpu_sel_f;
static double gpu_sel_t0 = -10;
static int gpu_from;
static char gpu_last[160];
static char sound_msg[160];
static double sound_t0;
static int sound_playing, sound_retry;
static double last_key, stats_t;
static int attract, attract_paused, attract_step;
static double attract_t0;
static int gpu_have;
static double reboot_t0;
static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

#define SGX "/usr/local/lib/sgx2d"

static const struct {
	const char *title, *sub, *desc;
} cards[5] = {
	{"gpu", "Sprites and blending on the GPU",
	 "The PowerVR SGX543MP1 runs Apple's own GPU microkernel, driven from Linux by an open DRM and Mesa driver."},
	{"sound", "A chord over HDMI audio",
	 "A synthesized arpeggio played through the HDMI audio driver: ALSA card \"HDMI\", 48 kHz, 16-bit stereo."},
	{"this box", "Live stats, what works", "Uptime, memory and CPU load straight from /proc, refreshed every second."},
	{"try it", "The story, the code", "How a 3rd-gen Apple TV ends up booting Linux, and where to find the source."},
	{"back to ios", "Reboot into iOS", "Reboots the box into iOS, the way it shipped. It takes about two minutes."},
};

/* --------------------------------------------------------- background --- */
#define GRID 48
static void make_bgbase(void)
{
	/* flat near-black with a faint cyan grid, aligned to the safe margin (96 = 2 cells) */
	bgbase = surf_new(W, H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t *p = &bgbase.px[(size_t)y * W + x];
			*p = C_BG;
			if (x % GRID == 0 || y % GRID == 0)
				over(p, C_ACCENT, 16);
		}
}

static void footer_keys(const char *const *k, int n)
{
	float x = SAFE, y = H - SAFE + 6;
	fill_rect(0, H - SAFE - 62, W, 2, C_ACCENT, 255);
	for (int i = 0; i < n; i += 2) {
		x = keycap(x, y, k[i]) + 14;
		x = text(F[F_FOOT], x, y, k[i + 1], C_DIM, 255, 0) + 44;
	}
}

static void title_bar(void)
{
	/* a cyan square for a logo, the site-style name, a full-bleed 2 px rule */
	fill_rect(SAFE, SAFE - 2, 30, 30, C_ACCENT, 255);
	text(F[F_TITLE], SAFE + 48, SAFE + 22, "appletv3.linux", C_TEXT, 255, 0);
	if (attract) {
		const char *s = "DEMO MODE \xc2\xb7 PRESS ANY BUTTON";
		float w = text_w(F[F_LABEL], s, 2.0f) + 24;
		tag(W - SAFE - w, SAFE + 22, s, C_ACCENT, C_INK);
	} else if (attract_paused) {
		const char *s = "AUTO DEMO PAUSED";
		float w = text_w(F[F_LABEL], s, 2.0f) + 28;
		box(W - SAFE - w, SAFE - 9, w, 42, 2, C_WARN, 255);
		text(F[F_LABEL], W - SAFE - w + 14, SAFE + 19, s, C_WARN, 255, 2.0f);
	} else {
		const char *s = si.kernel[0] ? si.kernel : "Linux";
		float w = text_w(F[F_SMALL], s, 0) + 32;
		box(W - SAFE - w, SAFE - 10, w, 46, 2, C_TEXT, 255);
		text(F[F_SMALL], W - SAFE - w + 16, SAFE + 21, s, C_TEXT, 255, 0);
	}
	fill_rect(0, SAFE + 70, W, 2, C_TEXT, 255);
}

/* a detail screen's head: the card's number in a cyan block, then its name */
static void section_head(int i)
{
	char n[4];
	snprintf(n, sizeof n, "%02d", i + 1);
	tilted(SAFE + 46, 222, 92, 92, -4, C_ACCENT, 255);
	text_c(F[F_BIG], SAFE + 46, 244, n, C_INK, 255);
	text(F[F_HERO], SAFE + 128, 254, cards[i].title, C_TEXT, 255, 0);
}

/* --------------------------------------------------------------- icons --- */
static void icon(int which, float x, float y, float s, uint32_t c)
{
	/* drawn in a s x s box at (x,y) */
	float m = s / 2, cx = x + m, cy = y + m;
	switch (which) {
	case 0: /* chip */
		stroke_rrect(x + s * 0.2f, y + s * 0.2f, s * 0.6f, s * 0.6f, s * 0.08f, s * 0.06f, c, 255);
		fill_rrect(x + s * 0.36f, y + s * 0.36f, s * 0.28f, s * 0.28f, s * 0.04f, c, 255);
		for (int i = 0; i < 3; i++) {
			float o = s * (0.33f + 0.17f * i);
			line(x + o, y + s * 0.06f, x + o, y + s * 0.18f, s * 0.05f, c, 255);
			line(x + o, y + s * 0.82f, x + o, y + s * 0.94f, s * 0.05f, c, 255);
			line(x + s * 0.06f, y + o, x + s * 0.18f, y + o, s * 0.05f, c, 255);
			line(x + s * 0.82f, y + o, x + s * 0.94f, y + o, s * 0.05f, c, 255);
		}
		break;
	case 1: /* speaker + waves */
		fill_rrect(x + s * 0.12f, y + s * 0.38f, s * 0.16f, s * 0.24f, s * 0.03f, c, 255);
		triangle(x + s * 0.24f, y + s * 0.40f, x + s * 0.46f, y + s * 0.18f, x + s * 0.46f, y + s * 0.82f, c, 255);
		triangle(x + s * 0.24f, y + s * 0.40f, x + s * 0.46f, y + s * 0.82f, x + s * 0.24f, y + s * 0.60f, c, 255);
		shape_circle(x + s * 0.44f, cy, s * 0.2f, s * 0.055f, 5.5f, 6.283f, c, 255);
		shape_circle(x + s * 0.44f, cy, s * 0.2f, s * 0.055f, 0, 0.78f, c, 255);
		shape_circle(x + s * 0.44f, cy, s * 0.36f, s * 0.055f, 5.5f, 6.283f, c, 255);
		shape_circle(x + s * 0.44f, cy, s * 0.36f, s * 0.055f, 0, 0.78f, c, 255);
		break;
	case 2: /* the box itself, a puck with a status light */
		fill_rrect(x + s * 0.08f, y + s * 0.36f, s * 0.84f, s * 0.3f, s * 0.08f, c, 255);
		fill_rrect(x + s * 0.12f, y + s * 0.38f, s * 0.76f, s * 0.14f, s * 0.06f, C_CARD2, 120);
		shape_circle(x + s * 0.78f, y + s * 0.58f, s * 0.035f, 0, 0, 0, C_CARD, 255);
		fill_rrect(x + s * 0.2f, y + s * 0.7f, s * 0.6f, s * 0.05f, s * 0.025f, c, 110);
		break;
	case 3: /* a QR-ish mark */
		for (int k = 0; k < 3; k++) {
			float ox = x + (k == 1 ? s * 0.58f : s * 0.1f), oy = y + (k == 2 ? s * 0.58f : s * 0.1f);
			stroke_rrect(ox, oy, s * 0.32f, s * 0.32f, s * 0.05f, s * 0.06f, c, 255);
			fill_rrect(ox + s * 0.1f, oy + s * 0.1f, s * 0.12f, s * 0.12f, s * 0.02f, c, 255);
		}
		fill_rrect(x + s * 0.6f, y + s * 0.6f, s * 0.12f, s * 0.12f, s * 0.02f, c, 255);
		fill_rrect(x + s * 0.78f, y + s * 0.78f, s * 0.12f, s * 0.12f, s * 0.02f, c, 255);
		fill_rrect(x + s * 0.78f, y + s * 0.6f, s * 0.12f, s * 0.08f, s * 0.02f, c, 200);
		break;
	case 4: /* restart arrow */
		shape_circle(cx, cy, s * 0.34f, s * 0.08f, 0.9f, 6.0f, c, 255);
		triangle(x + s * 0.73f, y + s * 0.10f, x + s * 0.92f, y + s * 0.38f, x + s * 0.60f, y + s * 0.40f, c, 255);
		break;
	}
}

/* ---------------------------------------------------------------- cards --- */
#define CARD_W 320
#define CARD_H 380
#define CARD_GAP 32
#define CARD_Y 410
#define PAD 40 /* sprite margin for the hard shadow and the cursor bar */
#define SHADOW 12
static surf card_spr[5][2], ring_spr;

static float card_x(float i) { return SAFE + i * (CARD_W + CARD_GAP); }

static void make_card(int i, int foc)
{
	surf *s = &card_spr[i][foc];
	*s = surf_new(CARD_W + 2 * PAD, CARD_H + 2 * PAD);
	set_target(s);
	float x = PAD, y = PAD;
	uint32_t ink = foc ? C_INK : C_TEXT;
	if (foc)
		fill_rect((int)x + SHADOW, (int)y + SHADOW, CARD_W, CARD_H, C_TEXT, 255);
	fill_rect((int)x, (int)y, CARD_W, CARD_H, foc ? C_ACCENT : C_CARD, 255);
	box(x, y, CARD_W, CARD_H, 2, foc ? C_INK : RGB(0x5a, 0x5a, 0x5a), 255);
	char n[4];
	snprintf(n, sizeof n, "%02d", i + 1);
	text(F[F_LABEL], x + 30, y + 50, n, foc ? C_INK : C_DIM, 255, 2.0f);
	uint32_t ic = foc ? C_INK : (i == 4 ? C_WARN : C_ACCENT);
	icon(i, x + 26, y + 84, 84, ic);
	text(F[F_CARDT], x + 30, y + 240, cards[i].title, ink, 255, 0);
	para(F[F_CARDS], x + 30, y + 290, CARD_W - 60, 34, cards[i].sub, foc ? C_INK : C_SOFT);
	set_target(&canvas);
}

static void make_ring(void)
{
	/* the focus cursor: a cyan bar under the card, it slides between cards */
	ring_spr = surf_new(CARD_W + 2 * PAD, CARD_H + 2 * PAD);
	set_target(&ring_spr);
	fill_rect(PAD, PAD + CARD_H + 26, CARD_W + SHADOW, 6, C_ACCENT, 255);
	set_target(&canvas);
}

/* ------------------------------------------------------------- screens --- */
static void begin_static(void)
{
	/* the static layer of a screen goes to bgcache, then everything is shown */
	memcpy(bgcache.px, bgbase.px, (size_t)W * H * 4);
	clip_all();
	set_target(&bgcache);
	title_bar();
}

static float home_last;
static void home_static(void)
{
	home_last = -1; /* the next home_dynamic draws everything */
	begin_static();
	text(F[F_LABEL], SAFE, 214, "APPLETV3,2 \xc2\xb7 J33IAP \xc2\xb7 S5L8947X \xc2\xb7 PROJECT CASCADIA", C_DIM, 255, 3.0f);
	float x = text(F[F_HERO], SAFE, 300, "this apple tv runs", C_TEXT, 255, 0) + 30;
	float bw = text_w(F[F_HERO], "LINUX", 0) + 56;
	tilted(x + bw / 2, 274, bw, 104, -3, C_ACCENT, 255);
	text(F[F_HERO], x + 28, 302, "LINUX", C_INK, 255, 0);
	text(F[F_LEAD], SAFE, 362,
	     "mainline 6.12 on the A5: one Cortex-A9 at ~850 MHz, 512 MB, 1080p over HDMI.", C_SOFT, 255, 0);
	static const char *k[] = {"Left/Right", "choose", "Select", "open", "Play/Pause", "auto demo on/off"};
	footer_keys(k, 6);
	set_target(&canvas);
}

#define HOME_DYN_Y0 (CARD_Y - PAD - 20)
#define HOME_DYN_Y1 (CARD_Y + CARD_H + 150)

/*
 * Only what moves is redrawn: the cards between where the focus was last frame and where it
 * is now, and the description line when it changes.  One Cortex-A9 draws every pixel, so the
 * whole 1920-wide strip each frame ran at 16 fps (measured, ATV_PERF=1).
 */
#define HOME_CARDS_Y1 (CARD_Y + CARD_H + PAD)
static float home_last = -1; /* focus as last drawn; < 0: draw everything */
static int home_di = -1, home_x0, home_x1, home_desc;

static void home_dynamic(void)
{
	double t = now_s();
	float e = ease_out((float)((t - focus_t0) / 0.28));
	focus = focus_from + (focus_to - focus_from) * e;
	int full = home_last < 0;
	float lo = full ? 0 : fminf(focus, home_last), hi = full ? 4 : fmaxf(focus, home_last);
	home_x0 = full ? 0 : (int)card_x(floorf(lo)) - PAD;
	home_x1 = full ? W : (int)card_x(ceilf(hi)) + CARD_W + PAD;
	set_target(&canvas);
	set_clip(home_x0 < 0 ? 0 : home_x0, HOME_DYN_Y0, home_x1 > W ? W : home_x1, HOME_CARDS_Y1);
	copy_rect(&canvas, &bgcache, home_x0, HOME_DYN_Y0, home_x1, HOME_CARDS_Y1);
	for (int i = 0; i < 5; i++) {
		float lift = 1 - fminf(1, fabsf(focus - i));
		int x = (int)card_x(i) - PAD, y = CARD_Y - PAD - (int)(16 * lift);
		if (x + CARD_W + 2 * PAD <= home_x0 || x >= home_x1)
			continue;
		/* the colour snaps at the halfway point, no crossfade: two full-alpha blits were half the cost */
		blit(&card_spr[i][lift > 0.5f], x, y, 255);
	}
	blit(&ring_spr, (int)card_x(focus) - PAD, CARD_Y - PAD, 255);
	home_last = focus;
	/* the focused card's description, switched at the halfway point */
	int di = (int)roundf(focus);
	home_desc = full || di != home_di;
	if (home_desc) {
		home_di = di;
		set_clip(0, HOME_CARDS_Y1, W, HOME_DYN_Y1);
		copy_rect(&canvas, &bgcache, 0, HOME_CARDS_Y1, W, HOME_DYN_Y1);
		float x = text(F[F_BODY], SAFE, CARD_Y + CARD_H + 104, "\xe2\x86\x92", C_ACCENT, 255, 0) + 18;
		para(F[F_BODY], x, CARD_Y + CARD_H + 104, W - SAFE - x, 40, cards[di].desc, C_SOFT);
	}
	clip_all();
}

static int home_animating(void) { return now_s() - focus_t0 < 0.3; }

/* GPU detail */
static void gpu_static(void)
{
	begin_static();
	section_head(0);
	para(F[F_LEAD], SAFE, 340, 1400,
	     46, "A PowerVR SGX543MP1. Apple's own GPU microkernel runs on it; Linux drives it with an open DRM driver "
		 "and Mesa (OpenGL ES 2.0). Pick a demo, it takes over the whole screen.",
	     C_SOFT);
	/* measured facts, as framed chips */
	float y = 530;
	text(F[F_LABEL], SAFE, y - 24, "MEASURED ON THIS BOX", C_DIM, 255, 3.0f);
	const char *facts[] = {"sgx2d sprites: 80 fps, 200 sprites at 1080p", "Mesa glkms: 41 fps full screen at 1080p",
			       "Apple microkernel up in 74 ms"};
	float x = SAFE;
	for (int i = 0; i < 3; i++) {
		float w = text_w(F[F_SMALL], facts[i], 0) + 40;
		box(x, y, w, 52, 2, C_FAINT, 255);
		text(F[F_SMALL], x + 20, y + 34, facts[i], C_TEXT, 255, 0);
		x += w + 16;
	}
	static const char *k[] = {"Left/Right", "choose", "Select", "run", "Menu", "back / stop"};
	footer_keys(k, 6);
	set_target(&canvas);
}

#define GPU_TY 620
#define GPU_TW 820
#define GPU_TH 220
static void gpu_dynamic(void)
{
	double t = now_s();
	float e = ease_out((float)((t - gpu_sel_t0) / 0.25));
	gpu_sel_f = gpu_from + (gpu_sel - gpu_from) * e;
	set_clip(0, GPU_TY - 40, W, GPU_TY + GPU_TH + 70);
	copy_rect(&canvas, &bgcache, 0, GPU_TY - 40, W, GPU_TY + GPU_TH + 70);
	set_target(&canvas);
	static const char *tt[2] = {"sprites", "fill & blend"};
	static const char *ts[2] = {"200 bouncing sprites, drawn by the GPU at 1920\xc3\x97" "1080",
				    "Fills, blend modes and a streaming texture"};
	/* the hard shadow slides to the chosen tile */
	fill_rect((int)(SAFE + gpu_sel_f * (GPU_TW + 48)) + SHADOW, GPU_TY + SHADOW, GPU_TW, GPU_TH, C_TEXT, 255);
	for (int i = 0; i < 2; i++) {
		float x = SAFE + i * (GPU_TW + 48);
		float sel = 1 - fminf(1, fabsf(gpu_sel_f - i));
		int on = sel > 0.5f;
		uint32_t ink = on ? C_INK : C_TEXT;
		fill_rect((int)x, GPU_TY, GPU_TW, GPU_TH, on ? C_ACCENT : C_CARD, 255);
		box(x, GPU_TY, GPU_TW, GPU_TH, 2, on ? C_INK : RGB(0x5a, 0x5a, 0x5a), 255);
		/* a play glyph in a frame */
		box(x + 50, GPU_TY + GPU_TH / 2 - 60, 120, 120, 3, ink, 255);
		triangle(x + 94, GPU_TY + GPU_TH / 2 - 26, x + 94, GPU_TY + GPU_TH / 2 + 26, x + 136, GPU_TY + GPU_TH / 2,
			 on ? C_INK : C_ACCENT, 255);
		text(F[F_CARDT], x + 210, GPU_TY + 92, tt[i], ink, 255, 0);
		para(F[F_CARDS], x + 210, GPU_TY + 140, GPU_TW - 250, 34, ts[i], on ? C_INK : C_SOFT);
	}
	const char *msg = gpu_last[0] ? gpu_last : (gpu_have ? "" : "The GPU demos are not in this image (" SGX ").");
	if (msg[0])
		text(F[F_SMALL], SAFE, GPU_TY + GPU_TH + 54, msg, gpu_last[0] ? C_ACCENT : C_WARN, 255, 0);
	clip_all();
}

/* Sound */
static const char *pc_name[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void sound_static(void)
{
	begin_static();
	section_head(1);
	para(F[F_LEAD], SAFE, 340, 1600, 46,
	     "HDMI audio from Linux: the I2S and DMA driver for this chip, ALSA card \"HDMI\". The clip is synthesized "
	     "on the box itself: an arpeggio over four chords.",
	     C_SOFT);
	static const char *k[] = {"Select", "play / stop", "Menu", "back"};
	footer_keys(k, 4);
	set_target(&canvas);
}

#define SND_Y0 470
#define SND_Y1 (H - SAFE - 70)
static void sound_dynamic(void)
{
	set_clip(0, SND_Y0, W, SND_Y1);
	copy_rect(&canvas, &bgcache, 0, SND_Y0, W, SND_Y1);
	set_target(&canvas);
	double el = sound_playing ? now_s() - sound_t0 : 0;
	if (el > clip_secs)
		el = clip_secs;
	/* play button: a framed square, cyan while it plays */
	float bx = SAFE + 100, by = 640, bs = 200;
	fill_rect((int)(bx - bs / 2) + SHADOW, (int)(by - bs / 2) + SHADOW, (int)bs, (int)bs, C_TEXT, 255);
	fill_rect((int)(bx - bs / 2), (int)(by - bs / 2), (int)bs, (int)bs, sound_playing ? C_ACCENT : C_CARD, 255);
	box(bx - bs / 2, by - bs / 2, bs, bs, 3, sound_playing ? C_INK : C_TEXT, 255);
	if (sound_playing) {
		fill_rect((int)bx - 28, (int)by - 34, 20, 68, C_INK, 255);
		fill_rect((int)bx + 8, (int)by - 34, 20, 68, C_INK, 255);
	} else
		triangle(bx - 24, by - 38, bx - 24, by + 38, bx + 40, by, C_ACCENT, 255);
	/* chord name */
	const char *ch = "";
	for (int i = 0; i < viz_nc; i++)
		if (el >= viz_ch[i].t)
			ch = viz_ch[i].name;
	text(F[F_LABEL], SAFE, by + 176, "NOW PLAYING", C_DIM, 255, 3.0f);
	text(F[F_BIG], SAFE, by + 244, sound_playing ? ch : "\xe2\x80\x94", C_TEXT, 255, 0);
	/* chromagram: 12 pitch-class bars from viz.bin at the playback position */
	float x0 = SAFE + 420, bw = 88, gap = 20, base = SND_Y1 - 90, maxh = 250;
	int fi = (int)(el * viz_fps);
	for (int b = 0; b < 12; b++) {
		float v = 0;
		if (sound_playing && viz_v && fi < viz_nf) {
			/* a little smoothing over the neighbouring frames */
			float a = viz_v[fi * 12 + b], c0 = fi > 0 ? viz_v[(fi - 1) * 12 + b] : a;
			v = fminf(1, (a * 0.7f + c0 * 0.3f) / 170.0f);
		}
		float h = 8 + v * maxh;
		float x = x0 + b * (bw + gap);
		fill_rect((int)x, (int)(base - maxh), (int)bw, (int)maxh, C_CARD, 255);
		box(x, base - maxh, bw, maxh, 1, C_LINE, 255);
		uint32_t col = v > 0.55f ? C_ACCENT : (v > 0.25f ? RGB(0x00, 0xa6, 0xa6) : RGB(0x00, 0x5c, 0x5c));
		fill_rect((int)x, (int)(base - h), (int)bw, (int)h, col, 255);
		text_c(F[F_SMALL], x + bw / 2, base + 44, pc_name[b], v > 0.55f ? C_TEXT : C_DIM, 255);
	}
	/* progress */
	float px = x0, pw = 12 * (bw + gap) - gap, py = SND_Y0 + 26;
	box(px, py, pw, 16, 2, C_FAINT, 255);
	fill_rect((int)px + 4, (int)py + 4, (int)((pw - 8) * (float)(el / clip_secs)), 8, C_ACCENT, 255);
	char tb[64];
	snprintf(tb, sizeof tb, "%.1f s / %.1f s", el, clip_secs);
	text_r(F[F_SMALL], px + pw, py + 56, tb, C_DIM, 255);
	text(F[F_SMALL], px, py + 56, sound_msg[0] ? sound_msg : "Pitch classes, analysed from the same samples",
	     sound_msg[0] ? C_WARN : C_DIM, 255, 0);
	clip_all();
}

/* This box */
static const struct { const char *t; int live; } works[] = {
	{"HDMI video, 1920\xc3\x97" "1080 at 60 Hz", 0},
	{"HDMI audio (ALSA)", 1},
	{"GPU: SGX543MP1, open Mesa driver", 1},
	{"IR remote via the MSP430 MCU", 1},
	{"Ethernet (s5l_fe)", 0},
	{"Wi-Fi chip up, scans networks (BCM4334)", 0},
	{"Reboot back into iOS", 0},
	{"Cable-free boot from iOS: kloader \xe2\x86\x92 iBEC \xe2\x86\x92 iBoot", 0},
};

static void box_static(void)
{
	begin_static();
	section_head(2);
	text(F[F_LEAD], SAFE, 340, "Apple TV 3rd gen (A1469) \xc2\xb7 A5 S5L8947X \xc2\xb7 one Cortex-A9 at ~850 MHz \xc2\xb7 512 MB",
	     C_SOFT, 255, 0);
	/* what works panel */
	float x = 1080, y = 400, w = W - SAFE - 1080, h = 500;
	fill_rect((int)x, (int)y, (int)w, (int)h, C_CARD, 255);
	box(x, y, w, h, 2, C_TEXT, 255);
	text(F[F_LABEL], x + 36, y + 54, "WHAT WORKS", C_DIM, 255, 3.0f);
	int n = sizeof works / sizeof works[0];
	for (int i = 0; i < n; i++) {
		float ly = y + 112 + i * 52;
		/* a ticked square: cyan when this demo uses it */
		if (works[i].live)
			fill_rect((int)x + 38, (int)ly - 26, 30, 30, C_ACCENT, 255);
		else
			box(x + 38, ly - 26, 30, 30, 2, C_TEXT, 255);
		uint32_t tc = works[i].live ? C_INK : C_TEXT;
		line(x + 45, ly - 11, x + 51, ly - 5, 3.2f, tc, 255);
		line(x + 51, ly - 5, x + 61, ly - 18, 3.2f, tc, 255);
		float ex = text(F[F_SMALL], x + 86, ly, works[i].t, C_TEXT, 255, 0);
		if (works[i].live) {
			const char *tg = "IN THIS DEMO";
			if (ex + 16 + text_w(F[F_LABEL], tg, 2.0f) + 24 < x + w - 24)
				tag(ex + 16, ly, tg, C_ACCENT, C_INK);
		}
	}
	static const char *k[] = {"Menu", "back"};
	footer_keys(k, 2);
	set_target(&canvas);
}

#define BOX_Y0 400
#define BOX_Y1 920
static void stat_tile(float x, float y, float w, float h, const char *lab, const char *val, const char *sub)
{
	fill_rect((int)x, (int)y, (int)w, (int)h, C_CARD, 255);
	box(x, y, w, h, 2, RGB(0x5a, 0x5a, 0x5a), 255);
	text(F[F_LABEL], x + 30, y + 48, lab, C_DIM, 255, 3.0f);
	text(F[F_BIG], x + 28, y + 122, val, C_TEXT, 255, 0);
	if (sub)
		text(F[F_SMALL], x + 30, y + 170, sub, C_SOFT, 255, 0);
}

static void box_dynamic(void)
{
	set_clip(SAFE, BOX_Y0, 1060, BOX_Y1 + 2);
	copy_rect(&canvas, &bgcache, SAFE, BOX_Y0, 1060, BOX_Y1 + 2);
	set_target(&canvas);
	char v[64], s[96];
	float tw = 440, th = 196, gx = SAFE, gy = BOX_Y0;
	long up = (long)si.uptime;
	if (up >= 3600)
		snprintf(v, sizeof v, "%ldh %02ldm", up / 3600, up / 60 % 60);
	else
		snprintf(v, sizeof v, "%ldm %02lds", up / 60, up % 60);
	stat_tile(gx, gy, tw, th, "UPTIME", v, "since the kernel started");
	long used = (si.mem_total_kb - si.mem_avail_kb) / 1024, tot = si.mem_total_kb / 1024;
	snprintf(v, sizeof v, "%ld MB", used);
	snprintf(s, sizeof s, "used of %ld MB (MemTotal)", tot);
	stat_tile(gx + tw + 24, gy, tw, th, "MEMORY", v, s);
	/* memory bar */
	float mx = gx + tw + 24 + 30, mw = tw - 60;
	fill_rect((int)mx, (int)gy + 136, (int)mw, 8, C_CARD2, 255);
	if (tot)
		fill_rect((int)mx, (int)gy + 136, (int)(mw * used / (float)tot), 8, C_ACCENT, 255);
	snprintf(v, sizeof v, "%.0f%%", si.cpu_pct);
	snprintf(s, sizeof s, "load average %.2f", si.load1);
	stat_tile(gx, gy + th + 24, tw, th, "CPU LOAD", v, s);
	/* sparkline of the last minute */
	float sx = gx + 200, sw = tw - 232, sy = gy + th + 24 + 40, sh = 80;
	for (int i = 1; i < si.nhist; i++) {
		float x0 = sx + sw * (i - 1) / 59.0f, x1 = sx + sw * i / 59.0f;
		float y0 = sy + sh - sh * clampf(si.hist[i - 1] / 100.0f, 0, 1);
		float y1 = sy + sh - sh * clampf(si.hist[i] / 100.0f, 0, 1);
		line(x0, y0, x1, y1, 3, C_ACCENT, 255);
	}
	stat_tile(gx + tw + 24, gy + th + 24, tw, th, "KERNEL", "", si.model[0] ? si.model : "");
	text(F[F_MED], gx + tw + 24 + 30, gy + th + 24 + 116, si.kernel, C_TEXT, 255, 0);
	snprintf(v, sizeof v, "Display %d\xc3\x97%d, simplefb", si.fbw, si.fbh);
	text(F[F_SMALL], SAFE + 2, BOX_Y1 - 4, v, C_DIM, 255, 0);
	clip_all();
}

/* Try it */
static void try_static(void)
{
	begin_static();
	section_head(3);
	static const char *story[] = {
		"Project Cascadia brought mainline Linux 6.12 to Apple's A5, starting with the iPad mini 1.",
		"This port takes it to the Apple TV 3: HDMI video and audio, Ethernet, the IR remote, the GPU.",
		"It boots from iOS with no cable: kloader \xe2\x86\x92 iBEC \xe2\x86\x92 iBoot, which loads /linux.kc from flash.",
		"Everything you see here is open source. Scan the code to start reading.",
	};
	float y = 370;
	for (int i = 0; i < 4; i++) {
		text(F[F_LEAD], SAFE, y, "\xe2\x86\x92", C_ACCENT, 255, 0);
		y = para(F[F_LEAD], SAFE + 44, y, 1000, 48, story[i], i == 3 ? C_TEXT : C_SOFT) + 26;
	}
	/* QR on a white square with a hard cyan shadow, quiet zone 4 modules */
	if (qr_n) {
		int mod = 12, qs = (qr_n + 8) * mod;
		float qx = W - SAFE - qs - SHADOW, qy = 230;
		fill_rect((int)qx + SHADOW, (int)qy + SHADOW, qs, qs, C_ACCENT, 255);
		fill_rect((int)qx, (int)qy, qs, qs, RGB(255, 255, 255), 255);
		for (int yy = 0; yy < qr_n; yy++)
			for (int xx = 0; xx < qr_n; xx++)
				if (qr_mod[yy][xx])
					fill_rect((int)qx + (xx + 4) * mod, (int)qy + (yy + 4) * mod, mod, mod, C_INK, 255);
		/* the address under it, split after the owner so it stays inside the square's width */
		const char *u = strncmp(qr_url, "https://", 8) ? qr_url : qr_url + 8;
		const char *cut = strrchr(u, '/');
		char a[128];
		snprintf(a, sizeof a, "%.*s", cut ? (int)(cut - u + 1) : (int)strlen(u), u);
		float ty = qy + qs + SHADOW + 56;
		text(F[F_BODY], qx, ty, a, C_TEXT, 255, 0);
		if (cut)
			text(F[F_BODY], qx, ty + 38, cut + 1, C_ACCENT, 255, 0);
		text(F[F_LABEL], qx, ty + 88, "FORK \xc2\xb7 BRANCH J33I \xc2\xb7 THIS PORT", C_DIM, 255, 3.0f);
	}
	static const char *k[] = {"Menu", "back"};
	footer_keys(k, 2);
	set_target(&canvas);
}

/* Back to iOS: a dialog over the dimmed home screen */
static void confirm_static(int rebooting)
{
	home_static();
	set_target(&bgcache);
	/* the cards, as they are now */
	set_clip(0, HOME_DYN_Y0, W, HOME_DYN_Y1);
	for (int i = 0; i < 5; i++)
		blit(&card_spr[i][i == 4], (int)card_x(i) - PAD, CARD_Y - PAD - (i == 4 ? 16 : 0), 255);
	clip_all();
	fill_rect(0, 0, W, H, C_BG, 215);
	float dw = 1000, dh = 440, dx = (W - dw) / 2, dy = (H - dh) / 2 - 20;
	fill_rect((int)dx + 16, (int)dy + 16, (int)dw, (int)dh, C_WARN, 255);
	fill_rect((int)dx, (int)dy, (int)dw, (int)dh, C_BG, 255);
	box(dx, dy, dw, dh, 3, C_TEXT, 255);
	fill_rect((int)(dx + dw / 2 - 56), (int)dy + 50, 112, 112, C_WARN, 255);
	icon(4, dx + dw / 2 - 40, dy + 66, 80, C_INK);
	if (!rebooting) {
		text_c(F[F_CARDT], W / 2, dy + 236, "restart into iOS?", C_TEXT, 255);
		text_c(F[F_BODY], W / 2, dy + 292, "Select to reboot into iOS, Menu to cancel.", C_SOFT, 255);
		float kw = text_w(F[F_LABEL], "SELECT", 2) + 28 + 14 + text_w(F[F_FOOT], "reboot", 0) + 44 +
			   text_w(F[F_LABEL], "MENU", 2) + 28 + 14 + text_w(F[F_FOOT], "cancel", 0);
		float x = W / 2 - kw / 2, y = dy + 382;
		x = keycap(x, y, "Select") + 14;
		x = text(F[F_FOOT], x, y, "reboot", C_DIM, 255, 0) + 44;
		x = keycap(x, y, "Menu") + 14;
		text(F[F_FOOT], x, y, "cancel", C_DIM, 255, 0);
	} else {
		text_c(F[F_CARDT], W / 2, dy + 236, "restarting into iOS\xe2\x80\xa6", C_TEXT, 255);
		text_c(F[F_BODY], W / 2, dy + 292, "This takes about two minutes. Thanks for watching!", C_SOFT, 255);
	}
	set_target(&canvas);
}

/* the GPU demo owns the screen: a hint is drawn first, and stays if it fails to start */
static void gpurun_static(void)
{
	begin_static();
	text_c(F[F_CARDT], W / 2, H / 2 - 10, "starting the GPU demo\xe2\x80\xa6", C_TEXT, 255);
	text_c(F[F_BODY], W / 2, H / 2 + 50, "Menu stops it.", C_SOFT, 255);
	set_target(&canvas);
}

static void render_full(void)
{
	clip_all();
	switch (screen) {
	case S_HOME: home_static(); break;
	case S_GPU: gpu_static(); break;
	case S_SOUND: sound_static(); break;
	case S_BOX: box_static(); break;
	case S_TRY: try_static(); break;
	case S_CONFIRM: confirm_static(0); break;
	case S_REBOOT: confirm_static(1); break;
	case S_GPURUN: gpurun_static(); break;
	}
	memcpy(canvas.px, bgcache.px, (size_t)W * H * 4);
	set_target(&canvas);
	switch (screen) {
	case S_HOME: home_dynamic(); break;
	case S_GPU: gpu_dynamic(); break;
	case S_SOUND: sound_dynamic(); break;
	case S_BOX: box_dynamic(); break;
	}
	present_all();
}

/* redraw what moves; returns 1 if anything is still animating */
static int render_dynamic(void)
{
	switch (screen) {
	case S_HOME:
		home_dynamic();
		present(home_x0, HOME_DYN_Y0, home_x1, HOME_CARDS_Y1);
		if (home_desc)
			present(0, HOME_CARDS_Y1, W, HOME_DYN_Y1);
		return home_animating();
	case S_GPU:
		gpu_dynamic();
		present(0, GPU_TY - 40, W, GPU_TY + GPU_TH + 70);
		return now_s() - gpu_sel_t0 < 0.27;
	case S_SOUND:
		sound_dynamic();
		present(0, SND_Y0, W, SND_Y1);
		return sound_playing;
	case S_BOX:
		box_dynamic();
		present(SAFE, BOX_Y0, 1060, BOX_Y1 + 2);
		return 0;
	}
	return 0;
}

/* --------------------------------------------------------------- logic --- */
static void go(int s)
{
	screen = s;
	dirty_full = 1;
}

/* The GPU owns the screen during its demo, so what it is showing is said by the demo itself:
 * this draws a caption with the app's fonts into a file sgx2d puts on top of every frame. */
#define CAPTION_PATH "/tmp/atv-gpu-caption.rgba"
static void write_caption(int which)
{
	static const char *l1[2] = {"200 sprites, blended by the SGX543MP1 GPU at 1920\xc3\x97" "1080.",
				    "Fills, blend modes and a texture the CPU streams every frame."};
	static const char *l2[2] = {"The CPU only moves them; the GPU draws every pixel.",
				    "Drawn by the GPU, through Apple's microkernel."};
	surf c = surf_new(1024, 112);
	set_target(&c);
	set_clip(0, 0, c.w, c.h);
	fill_rect(0, 0, c.w, c.h, C_BG, 235);
	box(0, 0, c.w, c.h, 2, C_ACCENT, 255);
	float x = 22 + tag(22, 48, "GPU DEMO", C_ACCENT, C_INK) + 18;
	text(F[F_SMALL], x, 47, l1[which], C_TEXT, 255, 0);
	text(F[F_SMALL], x, 83, l2[which], C_SOFT, 255, 0);
	text_r(F[F_LABEL], c.w - 22, 83, "MENU STOPS IT", C_DIM, 255);
	set_target(&canvas);
	clip_all();
	FILE *f = fopen(CAPTION_PATH, "wb");
	if (f) {
		uint32_t hd[2] = {(uint32_t)c.w, (uint32_t)c.h};
		fwrite(hd, 4, 2, f);
		for (int i = 0; i < c.w * c.h; i++) {
			/* premultiplied a8r8g8b8 -> straight RGBA8 */
			uint32_t v = c.px[i];
			unsigned a = v >> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
			if (a && a < 255)
				r = r * 255 / a, g = g * 255 / a, b = b * 255 / a;
			uint8_t o[4] = {(uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g), (uint8_t)(b > 255 ? 255 : b), (uint8_t)a};
			fwrite(o, 1, 4, f);
		}
		fclose(f);
		setenv("SGX2D_CAPTION", CAPTION_PATH, 1);
	}
	free(c.px);
}

static void gpu_start(int which, int frames)
{
	write_caption(which);
	char n[16], fr[16];
	snprintf(n, sizeof n, "%d", 200);
	snprintf(fr, sizeof fr, "%d", frames);
	char *spr[] = {SGX "/sprites", SGX, n, fr, NULL};
	char *d2[] = {SGX "/demo2", SGX, NULL};
	go(S_GPURUN);
	render_full();
	if (spawn(which ? d2 : spr, 1) > 0)
		child_kind = 1;
	else {
		snprintf(gpu_last, sizeof gpu_last, "Could not start the GPU demo.");
		if (!attract)
			go(S_GPU);
	}
}

static void gpu_finished(int st)
{
	(void)st;
	/* sprites prints "600 frames, 200 sprites: 80.0 fps" last */
	char *l = child_out;
	if (child_outn < 0)
		child_outn = -child_outn;
	child_out[child_outn] = 0;
	if (strstr(l, "fps"))
		snprintf(gpu_last, sizeof gpu_last, "Last run on this box: %.100s", l);
	else if (WIFEXITED(st) && WEXITSTATUS(st) && WEXITSTATUS(st) != 127)
		snprintf(gpu_last, sizeof gpu_last, "The demo stopped: %.100s", l[0] ? l : "exit status");
	else
		gpu_last[0] = 0;
	child_kind = 0;
}

static void sound_start(int fallback)
{
	char *wav = clip_path;
	char *a1[] = {"/usr/bin/aplay", "-q", "-D", "hw:HDMI,0", wav, NULL};
	char *a2[] = {"/usr/bin/aplay", "-q", wav, NULL};
	sound_msg[0] = 0;
	if (preview) {
		sound_playing = 1;
		sound_t0 = now_s();
		return;
	}
	if (spawn(fallback ? a2 : a1, 1) > 0) {
		child_kind = 2;
		sound_playing = 1;
		sound_t0 = now_s();
		sound_retry = fallback;
	} else
		snprintf(sound_msg, sizeof sound_msg, "Could not start aplay.");
}

static void attract_go(int step)
{
	attract_step = step % 3;
	attract_t0 = now_s();
	if (attract_step == 0) {
		if (gpu_have && fb_w == W && fb_h == H)
			gpu_start(0, 1600);
		else
			go(S_BOX);
	} else
		go(attract_step == 1 ? S_BOX : S_TRY);
}

static void attract_stop(void)
{
	if (child >= 0)
		child_stop();
	child_kind = 0;
	sound_playing = 0;
	attract = 0;
	go(S_HOME);
}

static void handle_key(int k)
{
	last_key = now_s();
	if (attract) {
		/* any button ends the show; Play/Pause also keeps it from coming back */
		attract_stop();
		if (k == K_PLAY)
			attract_paused = 1;
		return;
	}
	if (k == K_PLAY && screen != S_GPURUN) {
		attract_paused = !attract_paused;
		dirty_full = 1;
		return;
	}
	switch (screen) {
	case S_HOME:
		if (k == K_LEFT || k == K_RIGHT) {
			int to = focus_to + (k == K_RIGHT ? 1 : -1);
			if (to < 0 || to > 4)
				break;
			focus_from = focus;
			focus_to = to;
			focus_t0 = now_s();
		} else if (k == K_OK) {
			focus = focus_from = focus_to;
			static const int dest[5] = {S_GPU, S_SOUND, S_BOX, S_TRY, S_CONFIRM};
			if (dest[focus_to] == S_SOUND)
				clip_prepare();
			go(dest[focus_to]);
			if (dest[focus_to] == S_BOX)
				read_stats(), stats_t = now_s();
		}
		break;
	case S_GPU:
		if (k == K_LEFT || k == K_RIGHT) {
			int to = k == K_RIGHT ? 1 : 0;
			if (to != gpu_sel) {
				gpu_from = gpu_sel;
				gpu_sel = to;
				gpu_sel_t0 = now_s();
			}
		} else if (k == K_OK && gpu_have) {
			gpu_start(gpu_sel, 100000);
		} else if (k == K_BACK)
			go(S_HOME);
		break;
	case S_GPURUN:
		if (k == K_BACK) {
			int st = 0;
			child_stop();
			gpu_finished(st);
			go(S_GPU);
		}
		break;
	case S_SOUND:
		if (k == K_OK) {
			if (sound_playing) {
				child_stop();
				sound_playing = 0;
				child_kind = 0;
			} else
				sound_start(0);
		} else if (k == K_BACK) {
			child_stop();
			sound_playing = 0;
			child_kind = 0;
			go(S_HOME);
		}
		break;
	case S_BOX:
	case S_TRY:
		if (k == K_BACK)
			go(S_HOME);
		break;
	case S_CONFIRM:
		if (k == K_OK) {
			go(S_REBOOT);
			render_full();
			if (!preview) {
				sync();
				/* keep this screen up through the way down: stage 2 TERMs everyone, KILLs 2 s later */
				signal(SIGTERM, SIG_IGN);
				signal(SIGHUP, SIG_IGN);
				/* what busybox reboot does: TERM to PID 1, whose trap (stage 2) reboots
				 * through iBoot's reset line */
				if (kill(1, SIGTERM))
					fprintf(stderr, "atv-demo: kill(1, TERM): %s\n", strerror(errno));
				reboot_t0 = now_s();
			}
		} else if (k == K_BACK)
			go(S_HOME);
		break;
	case S_REBOOT:
		/* still alive long after asking: let Menu bring the menu back */
		if (k == K_BACK && now_s() - reboot_t0 > 15) {
			signal(SIGTERM, on_sig);
			signal(SIGHUP, on_sig);
			go(S_HOME);
		}
		break;
	}
}

static void tick(void)
{
	double t = now_s();
	int st;
	if (child >= 0 && child_poll(&st)) {
		if (child_kind == 1) {
			gpu_finished(st);
			if (!attract)
				go(S_GPU);
		} else if (child_kind == 2) {
			child_kind = 0;
			if (WIFEXITED(st) && WEXITSTATUS(st) != 0 && t - sound_t0 < 1.5 && !sound_retry) {
				fprintf(stderr, "atv-demo: aplay on hw:HDMI,0 failed (%d): %s; trying the default device\n",
					WEXITSTATUS(st), child_out);
				sound_start(1);
			} else {
				if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
					snprintf(sound_msg, sizeof sound_msg, "aplay failed (%d)", WEXITSTATUS(st));
				sound_playing = 0;
				if (screen == S_SOUND)
					dirty_full = 1;
			}
		}
	}
	if (preview)
		return;
	if (sound_playing && t - sound_t0 > clip_secs + 3 && child < 0)
		sound_playing = 0;
	if (screen == S_BOX && t - stats_t >= 1.0) {
		read_stats();
		stats_t = t;
		render_dynamic();
	}
	if (!attract && !attract_paused && screen != S_CONFIRM && screen != S_REBOOT && screen != S_GPURUN &&
	    !sound_playing && t - last_key > 120) {
		attract = 1;
		attract_go(0);
	}
	if (attract) {
		double dur = attract_step == 0 ? 20 : 10;
		if (attract_step == 0 && child < 0 && screen == S_GPURUN)
			attract_go(1); /* the demo ended early */
		else if (t - attract_t0 > dur) {
			if (child >= 0)
				child_stop();
			child_kind = 0;
			attract_go(attract_step + 1);
		}
	}
	if (t - in_scan_t > (n_in ? 30 : 3))
		input_scan();
}

/* ------------------------------------------------------------- preview --- */
static int frame_no;
static char outdir[512];
static void write_ppm(const char *name)
{
	char p[700];
	snprintf(p, sizeof p, "%s/%02d-%s.ppm", outdir, ++frame_no, name);
	FILE *f = fopen(p, "wb");
	if (!f)
		die("%s: %s", p, strerror(errno));
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	uint8_t *row = malloc(W * 3);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t v = canvas.px[(size_t)y * W + x];
			row[x * 3] = v >> 16;
			row[x * 3 + 1] = v >> 8;
			row[x * 3 + 2] = v;
		}
		fwrite(row, 1, W * 3, f);
	}
	free(row);
	fclose(f);
	printf("%s\n", p);
}

static void shot(const char *name)
{
	render_full();
	write_ppm(name);
}

static void render_screens(void)
{
	sim_now = 100;
	shot("home");
	handle_key(K_RIGHT);
	sim_now += 0.09;
	shot("home-moving");
	sim_now += 1;
	handle_key(K_RIGHT);
	sim_now += 1;
	handle_key(K_RIGHT);
	sim_now += 1;
	shot("home-focus-tryit");
	handle_key(K_LEFT), sim_now += 1, handle_key(K_LEFT), sim_now += 1, handle_key(K_LEFT), sim_now += 1;
	handle_key(K_OK);
	gpu_have = 1;
	shot("gpu");
	handle_key(K_RIGHT);
	sim_now += 1;
	snprintf(gpu_last, sizeof gpu_last, "Last run on this box: 600 frames, 200 sprites: 80.0 fps (sample)");
	shot("gpu-demo2-lastrun");
	gpu_last[0] = 0;
	handle_key(K_BACK);
	handle_key(K_RIGHT), sim_now += 1;
	handle_key(K_OK);
	shot("sound");
	handle_key(K_OK);
	sim_now += 3.3;
	shot("sound-playing");
	sim_now += 3.6;
	shot("sound-playing-later");
	handle_key(K_BACK);
	handle_key(K_RIGHT), sim_now += 1;
	handle_key(K_OK);
	shot("thisbox");
	handle_key(K_BACK);
	handle_key(K_RIGHT), sim_now += 1;
	handle_key(K_OK);
	shot("tryit");
	handle_key(K_BACK);
	handle_key(K_RIGHT), sim_now += 1;
	handle_key(K_OK);
	shot("confirm");
	handle_key(K_OK);
	shot("rebooting");
	go(S_HOME);
	attract_paused = 1;
	shot("home-attract-paused");
	attract_paused = 0;
	attract = 1;
	go(S_BOX);
	shot("attract-thisbox");
	attract = 0;
	go(S_GPURUN);
	shot("gpu-starting");	write_caption(0); /* the sprites caption, to look at: CAPTION_PATH */
}

/* ---------------------------------------------------------------- main --- */
int main(int argc, char **argv)
{
	const char *render = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-a") && i + 1 < argc)
			snprintf(assetdir, sizeof assetdir, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--render-screens") && i + 1 < argc)
			render = argv[++i];
		else if (!strcmp(argv[i], "--write-clip") && i + 1 < argc) {
			/* the Sound card's clip as a WAV, to listen to it anywhere */
			preview = 1;
			make_clip();
			return write_clip(argv[++i]) ? 1 : 0;
		} else if (!strcmp(argv[i], "--restore-tty")) {
			/* for stage 2, after a demo that died without putting the console back */
#ifdef __linux__
			int fd = open("/dev/tty0", O_RDWR);
			if (fd >= 0) {
				ioctl(fd, KDSETMODE, KD_TEXT);
				if (write(fd, "\033[?25h", 6) < 0) {
				}
				close(fd);
			}
#endif
			return 0;
		}
		else {
			fprintf(stderr, "usage: atv-demo [-a ASSETDIR] [--render-screens OUTDIR]\n");
			return 2;
		}
	}
	setvbuf(stderr, NULL, _IOLBF, 0);
	perf = getenv("ATV_PERF") != NULL;
	load_fonts(assetdir);
	load_assets();
	canvas = surf_new(W, H);
	bgcache = surf_new(W, H);
	make_bgbase();
	for (int i = 0; i < 5; i++)
		make_card(i, 0), make_card(i, 1);
	make_ring();
	set_target(&canvas);
	if (render) {
		preview = 1;
		snprintf(outdir, sizeof outdir, "%s", render);
		mkdir(outdir, 0755);
		read_stats();
		render_screens();
		return 0;
	}
#ifdef __linux__
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	signal(SIGHUP, on_sig);
	signal(SIGPIPE, SIG_IGN);
	gpu_have = access(SGX "/sprites", X_OK) == 0;
	display_open();
	read_stats();
	input_scan();
	last_key = now_s();
	int anim = 0;
	while (!quit) {
		if (dirty_full) {
			dirty_full = 0;
			if (screen != S_GPURUN || child < 0)
				render_full();
			anim = 1;
		}
		struct pollfd pf[MAXIN];
		for (int i = 0; i < n_in; i++)
			pf[i] = (struct pollfd){in_fd[i], POLLIN, 0};
		int r = poll(pf, n_in, anim ? 16 : 200);
		if (r > 0)
			for (int i = 0; i < n_in; i++) {
				if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
					in_scan_t = 0;
					continue;
				}
				if (!(pf[i].revents & POLLIN))
					continue;
				struct input_event ev[16];
				ssize_t n = read(in_fd[i], ev, sizeof ev);
				for (int j = 0; j < (int)(n / (ssize_t)sizeof ev[0]); j++)
					if (ev[j].type == EV_KEY && ev[j].value == 1) {
						int k = map_key(ev[j].code);
						if (k != K_NONE) {
							handle_key(k);
							anim = 1;
						}
					}
			}
		tick();
		if (!clip_ready && screen == S_HOME && !anim)
			clip_prepare(); /* ~1 s of maths, once, while nothing moves */
		if (!dirty_full && anim && screen != S_GPURUN && screen != S_REBOOT && screen != S_CONFIRM) {
			double t0 = perf ? perf_clock() : 0;
			anim = render_dynamic();
			if (perf) {
				perf_all += perf_clock() - t0;
				if (++perf_n == 30) {
					fprintf(stderr, "atv-demo: perf screen %d: %.1f ms/frame (%.1f fps), fb copy %.1f ms\n", screen,
						perf_all / perf_n * 1e3, perf_n / perf_all, perf_fb / perf_n * 1e3);
					perf_n = 0, perf_all = perf_fb = 0;
				}
			}
		}
		else if (!dirty_full)
			anim = 0;
	}
	child_stop();
	display_restore();
#else
	fprintf(stderr, "atv-demo: this build only renders previews (--render-screens)\n");
	(void)on_sig;
	(void)present_all;
	(void)input_scan;
	(void)map_key;
#endif
	return 0;
}
