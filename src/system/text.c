/*
Copyright (C) 2019 Parallel Realities

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include <time.h>
#include "text.h"
#include "lang.h"

static void initFont(char *filename);
static void drawWord(char *word, int *x, int *y, int startX);
static void drawTextLines(int x, int y, int size, int align);
static void drawTextLine(int x, int y, int size, int align, const char *line);
void calcTextDimensions(const char *text, int size, int *w, int *h);
static int toHex(char c);

extern void translateKeyNames(char *buf, int size);
extern int fileExists(const char *filename);

static char drawTextBuffer[MAX_LINE_LENGTH];
static int cachedLinesMode;
static SDL_Color cachedLinesColor;
static float scale;
static int ignoreColors;
static SDL_Color prevColor;
static SDL_Rect glyphs[128];
static SDL_Texture *fontTexture;

/* Text is UTF-8. The original glyph table covers ASCII only, and stays the fast path
 * for it; every other character (accents, Cyrillic, Chinese, Japanese ...) is drawn
 * on first use into a small bitmap of its own and kept. The layout size of such a
 * character (its width at FONT_SIZE) comes from SDL_ttf, so text is laid out the way
 * ASCII text is: widths at the game's scale, drawing at half size. */
static unsigned nextCodepoint(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	unsigned c = *s++;
	int n = 0;

	if (c >= 0xF0)
	{
		c &= 0x07;
		n = 3;
	}
	else if (c >= 0xE0)
	{
		c &= 0x0F;
		n = 2;
	}
	else if (c >= 0xC0)
	{
		c &= 0x1F;
		n = 1;
	}

	while (n-- && (*s & 0xC0) == 0x80)
	{
		c = (c << 6) | (*s++ & 0x3F);
	}

	*p = (const char *)s;

	return c;
}

static int encodeCodepoint(unsigned c, char *out)
{
	if (c < 0x80)
	{
		out[0] = c;
		out[1] = 0;
		return 1;
	}

	if (c < 0x800)
	{
		out[0] = 0xC0 | (c >> 6);
		out[1] = 0x80 | (c & 0x3F);
		out[2] = 0;
		return 2;
	}

	if (c < 0x10000)
	{
		out[0] = 0xE0 | (c >> 12);
		out[1] = 0x80 | ((c >> 6) & 0x3F);
		out[2] = 0x80 | (c & 0x3F);
		out[3] = 0;
		return 3;
	}

	out[0] = 0xF0 | (c >> 18);
	out[1] = 0x80 | ((c >> 12) & 0x3F);
	out[2] = 0x80 | ((c >> 6) & 0x3F);
	out[3] = 0x80 | (c & 0x3F);
	out[4] = 0;
	return 4;
}

/* a character a line may be broken after even without a space (Chinese, Japanese) */
static int canBreakAfter(unsigned c)
{
	return (c >= 0x2E80 && c < 0xA000) || (c >= 0xF900 && c < 0xFB00) || (c >= 0xFF00 && c < 0xFFF0);
}

#define XMETRIC_SIZE 4096
#define XGLYPH_SIZE 2048

typedef struct
{
	unsigned cp;
	int w, h;
} XMetric;

static XMetric xmetrics[XMETRIC_SIZE];
static TTF_Font *layoutFont;

/* the size of a non-ASCII character at the game's own scale */
static const XMetric *getXMetric(unsigned cp)
{
	unsigned h = (cp * 2654435761u) >> 20;
	char utf8[8];
	int i, w = 0, ht = 0;

	for (i = 0 ; i < XMETRIC_SIZE ; i++)
	{
		XMetric *m = &xmetrics[(h + i) & (XMETRIC_SIZE - 1)];

		if (m->cp == cp)
		{
			return m;
		}

		if (m->cp == 0)
		{
			encodeCodepoint(cp, utf8);

			if (layoutFont)
			{
				TTF_SizeUTF8(layoutFont, utf8, &w, &ht);
			}

			m->cp = cp;
			m->w = w;
			m->h = ht;

			return m;
		}
	}

	return &xmetrics[0];
}

/* SF3500: the screen is drawn at half the game's coordinates, and scaling a big
 * glyph bitmap down by that much (and by size/FONT_SIZE on top) is soft and slow.
 * So each text size gets its own glyph atlas, rendered by SDL_ttf at half the
 * size in points (the size it appears at on the screen), and drawing is a plain
 * 1:1 copy. The original glyph atlas above is kept for the layout (widths and
 * alignment), so text sits exactly where it did. */
#define MAX_HALF_FONTS 8
#define HALF_FONT_TEXTURE_SIZE 512

typedef struct
{
	int size;
	SDL_Texture *texture;
	SDL_Surface *surface;
	SDL_Rect glyphs[128];
	TTF_Font *ttf;                       /* kept open to draw other characters on first use */
	unsigned xcp[XGLYPH_SIZE];
	SDL_Surface *xsurf[XGLYPH_SIZE];
} HalfFont;

static HalfFont halfFonts[MAX_HALF_FONTS];
static int numHalfFonts;
static HalfFont *currentHalf;
static char fontPath[512];
static int fontBold;                     /* the fallback font is thin: drawn emboldened */
static int fontScale = 100;              /* percent: the fallback font is scaled to the height of the game's own */

static int pointSize(int n)
{
	n = n * fontScale / 100;

	return n < 6 ? 6 : n;
}

static HalfFont *getHalfFont(int size)
{
	TTF_Font *font;
	SDL_Surface *surface, *text;
	SDL_Rect dest;
	SDL_Color white = {255, 255, 255, 255};
	HalfFont *h;
	char c[2];
	int i;

	for (i = 0 ; i < numHalfFonts ; i++)
	{
		if (halfFonts[i].size == size)
		{
			return &halfFonts[i];
		}
	}

	if (numHalfFonts == MAX_HALF_FONTS)
	{
		return &halfFonts[0];
	}

	h = &halfFonts[numHalfFonts++];
	memset(h, 0, sizeof(HalfFont));
	h->size = size;

	font = TTF_OpenFont(fontPath, pointSize((size + 1) / 2));

	if (font && fontBold)
	{
		TTF_SetFontStyle(font, TTF_STYLE_BOLD);
	}
	/* A plain ARGB surface, cleared to transparent. The glyphs are copied in as
	 * they are (no blending), so their soft anti-aliased edges survive: at this
	 * size nearly every stroke is made of them. */
	surface = SDL_CreateRGBSurfaceWithFormat(0, HALF_FONT_TEXTURE_SIZE, HALF_FONT_TEXTURE_SIZE, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(surface, NULL, 0);

	dest.x = dest.y = 0;

	for (i = ' ' ; i <= 'z' ; i++)
	{
		c[0] = i;
		c[1] = 0;

		text = TTF_RenderUTF8_Blended(font, c, white);
		SDL_SetSurfaceBlendMode(text, SDL_BLENDMODE_NONE);

		TTF_SizeText(font, c, &dest.w, &dest.h);

		if (dest.x + dest.w >= HALF_FONT_TEXTURE_SIZE)
		{
			dest.x = 0;
			dest.y += dest.h + 1;
		}

		SDL_BlitSurface(text, NULL, surface, &dest);

		h->glyphs[i] = dest;

		SDL_FreeSurface(text);

		dest.x += dest.w + 1;
	}

	h->ttf = font;

	h->surface = surface;
	h->texture = toTexture(surface, 0);
	SDL_SetTextureBlendMode(h->texture, SDL_BLENDMODE_BLEND);

	return h;
}


/* a non-ASCII character as a small bitmap at this font's size */
static SDL_Surface *getXGlyph(HalfFont *hf, unsigned cp)
{
	unsigned h = (cp * 2654435761u) >> 21;
	SDL_Color white = {255, 255, 255, 255};
	char utf8[8];
	int i;

	for (i = 0 ; i < XGLYPH_SIZE ; i++)
	{
		int k = (h + i) & (XGLYPH_SIZE - 1);

		if (hf->xcp[k] == cp)
		{
			return hf->xsurf[k];
		}

		if (hf->xcp[k] == 0)
		{
			encodeCodepoint(cp, utf8);

			hf->xcp[k] = cp;
			hf->xsurf[k] = hf->ttf ? TTF_RenderUTF8_Blended(hf->ttf, utf8, white) : NULL;

			if (hf->xsurf[k] == NULL)
			{
				/* nothing to draw for it: an empty bitmap, so it is not tried again */
				hf->xsurf[k] = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_ARGB8888);
				SDL_FillRect(hf->xsurf[k], NULL, 0);
			}

			return hf->xsurf[k];
		}
	}

	return NULL;
}

/* Most text on screen is the same few lines in the same places every frame (the
 * HUD). Each plain line is drawn once into a small texture, shadow included, and
 * after that costs one copy per frame; the line is drawn again only when its
 * text, size or colour changes. Lines with colour codes ('#') and wrapped text
 * are not cached. */
#define MAX_CACHED_LINES 32

typedef struct
{
	char text[MAX_LINE_LENGTH];
	int size;
	Uint8 r, g, b;
	SDL_Texture *texture;
	int w, h;
} CachedLine;

static CachedLine cachedLines[MAX_CACHED_LINES];
static int nextCachedLine;

static SDL_Texture *buildLine(HalfFont *hf, const char *line, int size, SDL_Color color, int *outW, int *outH)
{
	SDL_Surface *surface, *xs;
	SDL_Texture *texture;
	SDL_Rect to;
	const char *p;
	float fscale = size / (FONT_SIZE * 1.0f), x;
	int c, pass, w = 1, h = 1, px;

	x = 0;

	for (p = line ; *p ; )
	{
		c = nextCodepoint(&p);

		if (c < 128)
		{
			px = (int)(x / 2);

			if (px + hf->glyphs[c].w > w) w = px + hf->glyphs[c].w;
			if (hf->glyphs[c].h > h) h = hf->glyphs[c].h;

			x += glyphs[c].w * fscale;
		}
		else
		{
			xs = getXGlyph(hf, c);

			if (xs)
			{
				px = (int)(x / 2);

				if (px + xs->w > w) w = px + xs->w;
				if (xs->h > h) h = xs->h;

				x += getXMetric(c)->w * fscale;
			}
		}
	}

	w++;
	h++;

	surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(surface, NULL, 0);

	for (pass = 0 ; pass < 2 ; pass++)
	{
		SDL_Color mod = pass == 0 ? (SDL_Color){0, 0, 0, 255} : color;

		/* the shadow first (black, one pixel down and right), then the text */
		SDL_SetSurfaceColorMod(hf->surface, mod.r, mod.g, mod.b);

		x = 0;

		for (p = line ; *p ; )
		{
			c = nextCodepoint(&p);

			to.x = (int)(x / 2) + (pass == 0);
			to.y = (pass == 0);

			if (c < 128)
			{
				SDL_BlitSurface(hf->surface, &hf->glyphs[c], surface, &to);

				x += glyphs[c].w * fscale;
			}
			else if ((xs = getXGlyph(hf, c)) != NULL)
			{
				SDL_SetSurfaceColorMod(xs, mod.r, mod.g, mod.b);
				SDL_BlitSurface(xs, NULL, surface, &to);

				x += getXMetric(c)->w * fscale;
			}
		}
	}

	SDL_SetSurfaceColorMod(hf->surface, 255, 255, 255);

	texture = toTexture(surface, 1);
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

	*outW = w;
	*outH = h;

	return texture;
}

static void drawCachedLine(const char *line, int x, int y, int size, int align, SDL_Color color)
{
	CachedLine *cl = NULL;
	SDL_Rect dest;
	int i, w, h;

	calcTextDimensions(line, size, &w, &h);

	if (align == TEXT_RIGHT)
	{
		x -= w;
	}
	else if (align == TEXT_CENTER)
	{
		x -= (w / 2);
	}

	for (i = 0 ; i < MAX_CACHED_LINES ; i++)
	{
		if (cachedLines[i].texture && cachedLines[i].size == size && cachedLines[i].r == color.r && cachedLines[i].g == color.g && cachedLines[i].b == color.b && strcmp(cachedLines[i].text, line) == 0)
		{
			cl = &cachedLines[i];
			break;
		}
	}

	if (!cl)
	{
		cl = &cachedLines[nextCachedLine];
		nextCachedLine = (nextCachedLine + 1) % MAX_CACHED_LINES;

		if (cl->texture)
		{
			SDL_DestroyTexture(cl->texture);
		}

		STRNCPY(cl->text, line, MAX_LINE_LENGTH);
		cl->size = size;
		cl->r = color.r;
		cl->g = color.g;
		cl->b = color.b;
		cl->texture = buildLine(currentHalf, line, size, color, &cl->w, &cl->h);
	}

	dest.x = x & ~1;
	dest.y = y & ~1;
	dest.w = cl->w * 2;
	dest.h = cl->h * 2;

	SDL_SetTextureAlphaMod(cl->texture, color.a);
	SDL_RenderCopy(app.renderer, cl->texture, NULL, &dest);
}

/* can this font draw every character of the loaded language? */
static int fontCoversLanguage(const char *path)
{
	TTF_Font *f = TTF_OpenFont(path, 16);
	const char *p;
	unsigned c;
	int i, ok = 1;

	if (!f)
	{
		return 0;
	}

	for (i = 0 ; i < langCount() && ok ; i++)
	{
		for (p = langString(i) ; *p && ok ; )
		{
			c = nextCodepoint(&p);

			if (c >= 128 && (c > 0xFFFF || !TTF_GlyphIsProvided(f, (Uint16)c)))
			{
				ok = 0;
			}
		}
	}

	TTF_CloseFont(f);

	return ok;
}

void initFonts(void)
{
	const char *fallback = getenv("WC_FALLBACK_FONT");

	STRNCPY(fontPath, getFileLocation("fonts/EnterCommand.ttf"), sizeof(fontPath));

	/* The game's own font has no Cyrillic, Chinese or Japanese. When a language needs
	 * more than it can draw, the whole game uses TreeFrogUI's Unicode font instead, so
	 * letters from two fonts are never mixed. */
	if (langCount() > 0 && !fontCoversLanguage(fontPath))
	{
		if (!fallback || !*fallback)
		{
			fallback = "/mnt/sdcard/frogui/fonts/TreeFrogUnicode.ttf";
		}

		if (fileExists(fallback))
		{
			TTF_Font *a = TTF_OpenFont(fontPath, FONT_SIZE), *b = TTF_OpenFont(fallback, FONT_SIZE);

			/* the other font is taller for the same point size, and every menu and
			 * panel is laid out for the game's own, so it is shrunk to match */
			if (a && b && TTF_FontHeight(b) > 0)
			{
				fontScale = TTF_FontHeight(a) * 100 / TTF_FontHeight(b);
			}

			if (a) TTF_CloseFont(a);
			if (b) TTF_CloseFont(b);

			STRNCPY(fontPath, fallback, sizeof(fontPath));

			fontBold = getenv("WC_FALLBACK_BOLD") ? atoi(getenv("WC_FALLBACK_BOLD")) : 1;
		}

		/* no font can draw this language: it is shown in English rather than as boxes */
		if (!fontCoversLanguage(fontPath))
		{
			SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN, "No font for language '%s' (looked for %s), using English", langLocale(), fallback);

			langDisable();
		}
	}

	initFont(fontPath);
}

static void initFont(char *filename)
{
	TTF_Font *font;
	SDL_Surface *surface, *text;
	SDL_Rect dest;
	int i;
	char c[2];
	SDL_Rect *g;
	SDL_Color white = {255, 255, 255, 255};

	memset(&glyphs, 0, sizeof(SDL_Rect) * 128);

	font = TTF_OpenFont(filename, pointSize(FONT_SIZE));

	if (font && fontBold)
	{
		TTF_SetFontStyle(font, TTF_STYLE_BOLD);
	}

	surface = SDL_CreateRGBSurface(0, FONT_TEXTURE_SIZE, FONT_TEXTURE_SIZE, 32, 0, 0, 0, 0xff);

	SDL_SetColorKey(surface, SDL_TRUE, SDL_MapRGBA(surface->format, 0, 0, 0, 0));

	dest.x = dest.y = 0;

	for (i = ' ' ; i <= 'z' ; i++)
	{
		memset(c, 0, 2);

		sprintf(c, "%c", i);

		text = TTF_RenderUTF8_Blended(font, c, white);

		TTF_SizeText(font, c, &dest.w, &dest.h);

		if (dest.x + dest.w >= FONT_TEXTURE_SIZE)
		{
			dest.x = 0;

			dest.y += dest.h + 1;

			if (dest.y + dest.h >= FONT_TEXTURE_SIZE)
			{
				SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_CRITICAL, "Out of glyph space in %dx%d font atlas texture map.", FONT_TEXTURE_SIZE, FONT_TEXTURE_SIZE);
				exit(1);
			}
		}

		SDL_BlitSurface(text, NULL, surface, &dest);

		g = &glyphs[i];

		g->x = dest.x;
		g->y = dest.y;
		g->w = dest.w;
		g->h = dest.h;

		SDL_FreeSurface(text);

		dest.x += dest.w;
	}

	layoutFont = font;                   /* kept: it measures the characters outside ASCII */

	fontTexture = toTexture(surface, 1);
}

/* Draws one line of finished text no wider than maxWidth: a longer translation is drawn
 * a little smaller instead of running into whatever is next to it. */
void drawTextFit(int x, int y, int size, int align, SDL_Color color, int maxWidth, const char *text)
{
	char buf[MAX_LINE_LENGTH];
	int w, h, originalSize = size;

	snprintf(buf, sizeof(buf), "%s", text);

	translateKeyNames(buf, sizeof(buf));

	for (;;)
	{
		calcTextDimensions(buf, size, &w, &h);

		if (w <= maxWidth || size <= 20)
		{
			break;
		}

		size -= 2;
	}

	/* text is drawn from its top, so a smaller line is moved down to keep its middle */
	drawText(x, y + (originalSize - size) / 2, size, align, color, "%s", buf);
}

extern long long perfTextNs;

void drawText(int x, int y, int size, int align, SDL_Color color, const char *format, ...)
{
	va_list args;
	struct timespec perfT0, perfT1;

	clock_gettime(CLOCK_MONOTONIC, &perfT0);

	memset(&drawTextBuffer, '\0', sizeof(drawTextBuffer));

	va_start(args, format);
	vsnprintf(drawTextBuffer, sizeof(drawTextBuffer), format, args);
	va_end(args);

	translateKeyNames(drawTextBuffer, sizeof(drawTextBuffer));

	currentHalf = getHalfFont(size);
	fontTexture = currentHalf->texture;

	if (app.text.wrap == 0 && strchr(drawTextBuffer, '#') == NULL)
	{
		drawCachedLine(drawTextBuffer, x, y, size, align, color);
	}
	else if (app.text.wrap == 0)
	{
		ignoreColors = 1;

		SDL_SetTextureColorMod(fontTexture, 0, 0, 0);
		SDL_SetTextureAlphaMod(fontTexture, 255);

		drawTextLine(x + 2, y + 2, size, align, drawTextBuffer);
		drawTextLine(x + 1, y + 1, size, align, drawTextBuffer);

		SDL_SetTextureColorMod(fontTexture, color.r, color.g, color.b);
		SDL_SetTextureAlphaMod(fontTexture, color.a);

		ignoreColors = 0;

		drawTextLine(x, y, size, align, drawTextBuffer);
	}
	else if (strchr(drawTextBuffer, '#') == NULL)
	{
		cachedLinesMode = 1;
		cachedLinesColor = color;

		drawTextLines(x, y, size, align);

		cachedLinesMode = 0;
	}
	else
	{
		ignoreColors = 1;

		SDL_SetTextureColorMod(fontTexture, 0, 0, 0);
		SDL_SetTextureAlphaMod(fontTexture, 255);

		drawTextLines(x + 2, y + 2, size, align);
		drawTextLines(x + 1, y + 1, size, align);

		SDL_SetTextureColorMod(fontTexture, color.r, color.g, color.b);
		SDL_SetTextureAlphaMod(fontTexture, color.a);

		ignoreColors = 0;

		drawTextLines(x, y, size, align);
	}

	clock_gettime(CLOCK_MONOTONIC, &perfT1);
	perfTextNs += (long long)(perfT1.tv_sec - perfT0.tv_sec) * 1000000000LL + (perfT1.tv_nsec - perfT0.tv_nsec);
}

static void drawTextLines(int x, int y, int size, int align)
{
	char line[MAX_LINE_LENGTH], token[MAX_WORD_LENGTH];
	const char *p;
	unsigned c;
	int n, w, h, currentWidth, len, step, last;

	memset(&line, '\0', sizeof(line));
	memset(&token, '\0', sizeof(token));

	len = strlen(drawTextBuffer);

	n = currentWidth = 0;

	for (p = drawTextBuffer ; *p ; )
	{
		const char *start = p;

		c = nextCodepoint(&p);
		step = p - start;
		last = *p == '\0';

		if (n + step >= MAX_WORD_LENGTH - 1)
		{
			/* a word too long for the buffer is cut where it stands */
			token[n] = '\0';
			n = 0;
			calcTextDimensions(token, size, &w, &h);
			strncat(line, token, sizeof(line) - strlen(line) - 1);
			currentWidth += w;
			memset(&token, '\0', sizeof(token));
		}

		memcpy(token + n, start, step);
		n += step;
		token[n] = '\0';

		if (c == ' ' || last || canBreakAfter(c))
		{
			calcTextDimensions(token, size, &w, &h);

			if (currentWidth + w > app.text.wrap)
			{
				if (cachedLinesMode)
				{
					drawCachedLine(line, x, y, size, align, cachedLinesColor);
				}
				else
				{
					drawTextLine(x, y, size, align, line);
				}

				currentWidth = 0;

				y += h * 1.2;

				memset(&line, '\0', sizeof(line));
			}

			strncat(line, token, sizeof(line) - strlen(line) - 1);

			n = 0;

			memset(&token, '\0', sizeof(token));

			currentWidth += w;
		}
	}

	(void)len;

	if (cachedLinesMode)
	{
		drawCachedLine(line, x, y, size, align, cachedLinesColor);
	}
	else
	{
		drawTextLine(x, y, size, align, line);
	}
}

static void drawTextLine(int x, int y, int size, int align, const char *line)
{
	int i, startX, n, w, h;
	char word[MAX_WORD_LENGTH];

	scale = size / (FONT_SIZE * 1.0f);

	startX = x;

	memset(word, 0, MAX_WORD_LENGTH);

	n = 0;

	calcTextDimensions(line, size, &w, &h);

	if (align == TEXT_RIGHT)
	{
		x -= w;
	}
	else if (align == TEXT_CENTER)
	{
		x -= (w / 2);
	}

	for (i = 0 ; i < strlen(line) ; i++)
	{
		word[n++] = line[i];

		if (line[i] == ' ')
		{
			drawWord(word, &x, &y, startX);

			memset(word, 0, MAX_WORD_LENGTH);

			n = 0;
		}
	}

	drawWord(word, &x, &y, startX);
}

static void drawWord(char *word, int *x, int *y, int startX)
{
	int i;
	int character;
	SDL_Rect dest, *g, *hg;
	SDL_Color c;

	i = 0;

	if (word[0] == '#')
	{
		if (!ignoreColors)
		{
			if (word[1] == '!')
			{
				SDL_SetTextureColorMod(fontTexture, prevColor.r, prevColor.g, prevColor.b);

				return;
			}

			c.r = toHex(word[1]);
			c.g = toHex(word[2]);
			c.b = toHex(word[3]);

			SDL_GetTextureColorMod(fontTexture, &prevColor.r, &prevColor.g, &prevColor.b);

			SDL_SetTextureColorMod(fontTexture, c.r, c.g, c.b);
		}

		return;
	}

	character = (unsigned char)word[i++];

	while (character)
	{
		if (character >= 128)
		{
			/* this older path draws ASCII only; the cached lines handle the rest */
			character = (unsigned char)word[i++];
			continue;
		}

		g = &glyphs[character];

		dest.x = *x;
		dest.y = *y;
		dest.w = g->w * scale;
		dest.h = g->h * scale;

		SDL_RenderCopy(app.renderer, fontTexture, g, &dest);

		*x += g->w * scale;

		character = (unsigned char)word[i++];
	}
}

static int toHex(char c)
{
	int result;

	if (c >= '0' && c <= '9')
	{
		result = c - '0';
	}
	else if (c >= 'a' && c <= 'f')
	{
		result = 11 + (c - 'a');
	}
	else
	{
		result = 0;
	}

	return MIN(result * 16, 255);
}

void calcTextDimensions(const char *text, int size, int *w, int *h)
{
	float scale;
	const char *p;
	int character;
	SDL_Rect *g;
	const XMetric *m;

	scale = size / (FONT_SIZE * 1.0f);

	*w = 0;
	*h = 0;

	for (p = text ; *p ; )
	{
		character = nextCodepoint(&p);

		if (character < 128)
		{
			g = &glyphs[character];

			*w += g->w * scale;
			*h = MAX(g->h * scale, *h);
		}
		else
		{
			m = getXMetric(character);

			*w += m->w * scale;
			*h = MAX(m->h * scale, *h);
		}
	}
}
