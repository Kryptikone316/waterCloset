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

static void initFont(char *filename);
static void drawWord(char *word, int *x, int *y, int startX);
static void drawTextLines(int x, int y, int size, int align);
static void drawTextLine(int x, int y, int size, int align, const char *line);
void calcTextDimensions(const char *text, int size, int *w, int *h);
static int toHex(char c);

extern void translateKeyNames(char *buf, int size);

static char drawTextBuffer[MAX_LINE_LENGTH];
static int cachedLinesMode;
static SDL_Color cachedLinesColor;
static float scale;
static int ignoreColors;
static SDL_Color prevColor;
static SDL_Rect glyphs[128];
static SDL_Texture *fontTexture;

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
} HalfFont;

static HalfFont halfFonts[MAX_HALF_FONTS];
static int numHalfFonts;
static HalfFont *currentHalf;
static char fontPath[512];

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

	font = TTF_OpenFont(fontPath, (size + 1) / 2);
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

	TTF_CloseFont(font);

	h->surface = surface;
	h->texture = toTexture(surface, 0);
	SDL_SetTextureBlendMode(h->texture, SDL_BLENDMODE_BLEND);

	return h;
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
	SDL_Surface *surface;
	SDL_Texture *texture;
	SDL_Rect to;
	float fscale = size / (FONT_SIZE * 1.0f), x;
	int i, c, pass, w = 1, h = 1, px;

	x = 0;

	for (i = 0 ; line[i] ; i++)
	{
		c = (unsigned char)line[i];

		if (c < 128)
		{
			px = (int)(x / 2);

			if (px + hf->glyphs[c].w > w) w = px + hf->glyphs[c].w;
			if (hf->glyphs[c].h > h) h = hf->glyphs[c].h;

			x += glyphs[c].w * fscale;
		}
	}

	w++;
	h++;

	surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(surface, NULL, 0);

	for (pass = 0 ; pass < 2 ; pass++)
	{
		/* the shadow first (black, one pixel down and right), then the text */
		if (pass == 0)
		{
			SDL_SetSurfaceColorMod(hf->surface, 0, 0, 0);
		}
		else
		{
			SDL_SetSurfaceColorMod(hf->surface, color.r, color.g, color.b);
		}

		x = 0;

		for (i = 0 ; line[i] ; i++)
		{
			c = (unsigned char)line[i];

			if (c < 128)
			{
				to.x = (int)(x / 2) + (pass == 0);
				to.y = (pass == 0);

				SDL_BlitSurface(hf->surface, &hf->glyphs[c], surface, &to);

				x += glyphs[c].w * fscale;
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

void initFonts(void)
{
	STRNCPY(fontPath, getFileLocation("fonts/EnterCommand.ttf"), sizeof(fontPath));

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

	font = TTF_OpenFont(filename, FONT_SIZE);

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

	TTF_CloseFont(font);

	fontTexture = toTexture(surface, 1);
}

extern long long perfTextNs;

void drawText(int x, int y, int size, int align, SDL_Color color, const char *format, ...)
{
	va_list args;
	struct timespec perfT0, perfT1;

	clock_gettime(CLOCK_MONOTONIC, &perfT0);

	memset(&drawTextBuffer, '\0', sizeof(drawTextBuffer));

	va_start(args, format);
	vsprintf(drawTextBuffer, format, args);
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
	int i, n, w, h, currentWidth, len;

	memset(&line, '\0', sizeof(line));
	memset(&token, '\0', sizeof(token));

	len = strlen(drawTextBuffer);

	n = currentWidth = 0;

	for (i = 0 ; i < len ; i++)
	{
		token[n++] = drawTextBuffer[i];

		if (drawTextBuffer[i] == ' ' || i == len - 1)
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

			strcat(line, token);

			n = 0;

			memset(&token, '\0', sizeof(token));

			currentWidth += w;
		}
	}

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

	character = word[i++];

	while (character)
	{
		g = &glyphs[character];

		dest.x = *x;
		dest.y = *y;
		dest.w = g->w * scale;
		dest.h = g->h * scale;

		SDL_RenderCopy(app.renderer, fontTexture, g, &dest);

		*x += g->w * scale;

		character = word[i++];
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
	int i, character;
	SDL_Rect *g;

	scale = size / (FONT_SIZE * 1.0f);

	*w = 0;
	*h = 0;

	i = 0;

	character = text[i++];

	while (character)
	{
		g = &glyphs[character];

		*w += g->w * scale;
		*h = MAX(g->h * scale, *h);

		character = text[i++];
	}
}
