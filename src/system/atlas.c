/*
Copyright (C) 2018 Parallel Realities

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

#include <SDL2/SDL_image.h>
#include "atlas.h"

static void loadAtlasData(void);
extern SDL_Texture *toTexture(SDL_Surface *surface, int destroySurface);

static AtlasImage atlases[NUM_ATLAS_BUCKETS];
static SDL_Texture *atlasTexture;

void initAtlas(void)
{
	memset(&atlases, 0, sizeof(AtlasImage) * NUM_ATLAS_BUCKETS);

	loadAtlasData();
}

AtlasImage *getAtlasImage(char *filename, int required)
{
	AtlasImage *a;
	unsigned long i;

	i = hashcode(filename) % NUM_ATLAS_BUCKETS;

	for (a = atlases[i].next ; a != NULL ; a = a->next)
	{
		if (strcmp(a->filename, filename) == 0)
		{
			return a;
		}
	}

	if (required)
	{
		SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_CRITICAL, "No such atlas image '%s'", filename);
		exit(1);
	}

	return NULL;
}

/* SF3500: the screen is drawn at half the game's coordinates (640x360 for the
 * 1280x720 game space), and the software renderer is far slower scaling every
 * sprite down as it draws it than copying it unscaled. So a half-size copy of
 * every atlas image is built once here, and drawing uses those. Each image is
 * reduced by averaging 2x2 blocks (weighted by alpha, so the edges of cut-out
 * shapes do not darken) and packed into its own small atlas, which also keeps
 * neighbouring images from bleeding into each other. */
#define HALF_ATLAS_SIZE 512

/* Picture tuning, set from watercloset.txt by run.sh (environment): how far the
 * reduced image leans towards a single sharp sample instead of the soft 2x2
 * average (0 to 1), and a colour boost applied to the reduced images (not to
 * the glow and particle sprites, which are tinted and faded by the game). */
static float tuneSharpness = 0.5f, tuneSaturation = 1.0f, tuneContrast = 1.0f;

static float tuneFromEnv(const char *name, float def)
{
	const char *v = getenv(name);

	if (v && *v)
	{
		return (float)atof(v);
	}

	return def;
}

static int clamp255(float v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : (int)(v + 0.5f));
}

static SDL_Surface *halfAtlasSurface, *halfDirectSurface;
static int halfCursorX, halfCursorY, halfRowHeight;

/* Copies a finished half-size image, mirrored, into the next free space in the
 * half-size atlas. SDL's flipped-copy call (SDL_RenderCopyEx) puts the picture in
 * the wrong place when it is letterboxed inside the screen surface, so a mirrored
 * copy is drawn with the plain call instead. */
static SDL_Rect placeMirrored(const SDL_Rect *from, int mirrorH, int mirrorV)
{
	Uint32 *dp = (Uint32 *)halfAtlasSurface->pixels;
	int dpitch = halfAtlasSurface->pitch / 4, i, j;
	SDL_Rect r;

	r.w = from->w;
	r.h = from->h;

	if (halfCursorX + r.w > HALF_ATLAS_SIZE)
	{
		halfCursorX = 0;
		halfCursorY += halfRowHeight;
		halfRowHeight = 0;
	}

	if (halfCursorY + r.h > HALF_ATLAS_SIZE)
	{
		SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_CRITICAL, "Half-size atlas is full (mirrored copies)");
		exit(1);
	}

	r.x = halfCursorX;
	r.y = halfCursorY;

	for (j = 0 ; j < r.h ; j++)
	{
		for (i = 0 ; i < r.w ; i++)
		{
			dp[(r.y + j) * dpitch + r.x + i] = dp[(from->y + (mirrorV ? r.h - 1 - j : j)) * dpitch + from->x + (mirrorH ? r.w - 1 - i : i)];
		}
	}

	halfCursorX += r.w;

	if (r.h > halfRowHeight)
	{
		halfRowHeight = r.h;
	}

	return r;
}

static void addHalfImage(SDL_Surface *src, AtlasImage *atlas)
{
	int hw, hh, i, j, di, dj, sx, sy;
	const Uint32 *sp = (const Uint32 *)src->pixels;
	Uint32 *dp = (Uint32 *)halfAtlasSurface->pixels;
	int spitch = src->pitch / 4, dpitch = halfAtlasSurface->pitch / 4;

	int isGlow = strncmp(atlas->filename, "gfx/particles/", 14) == 0;

	int direct = 0;

	hw = (atlas->rect.w + 1) / 2;
	hh = (atlas->rect.h + 1) / 2;

	/* Map tiles with no transparent pixel can be copied without blending. */
	if (strncmp(atlas->filename, "gfx/tilesets/", 13) == 0)
	{
		direct = 1;

		for (j = 0 ; j < atlas->rect.h && direct ; j++)
		{
			for (i = 0 ; i < atlas->rect.w ; i++)
			{
				if (((sp[(atlas->rect.y + j) * spitch + atlas->rect.x + i] >> 24) & 0xFF) != 255)
				{
					direct = 0;
					break;
				}
			}
		}
	}

	if (halfCursorX + hw > HALF_ATLAS_SIZE)
	{
		halfCursorX = 0;
		halfCursorY += halfRowHeight;
		halfRowHeight = 0;
	}

	if (halfCursorY + hh > HALF_ATLAS_SIZE)
	{
		SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_CRITICAL, "Half-size atlas is full at '%s'", atlas->filename);
		exit(1);
	}

	for (j = 0 ; j < hh ; j++)
	{
		for (i = 0 ; i < hw ; i++)
		{
			int n = 0, a = 0, r = 0, g = 0, b = 0;

			for (dj = 0 ; dj < 2 ; dj++)
			{
				for (di = 0 ; di < 2 ; di++)
				{
					sx = i * 2 + di;
					sy = j * 2 + dj;

					if (sx < atlas->rect.w && sy < atlas->rect.h)
					{
						Uint32 px = sp[(atlas->rect.y + sy) * spitch + atlas->rect.x + sx];
						int pa = (px >> 24) & 0xFF;

						n++;
						a += pa;
						r += ((px >> 16) & 0xFF) * pa;
						g += ((px >> 8) & 0xFF) * pa;
						b += (px & 0xFF) * pa;
					}
				}
			}

			if (a > 0)
			{
				float fr = (float)r / a, fg = (float)g / a, fb = (float)b / a, fa = (float)a / n;

				/* Lean towards the most opaque of the four source pixels (a
				 * crisp, aliased pick, like the old scaled draw) as far as
				 * tuneSharpness says. */
				/* Soft glows and particles stay plain averages: pulling them towards
				 * their most opaque pixel would thicken their gradients. */
				if (tuneSharpness > 0.0f && !isGlow)
				{
					Uint32 best = 0;
					int ba = -1;

					for (dj = 0 ; dj < 2 ; dj++)
					{
						for (di = 0 ; di < 2 ; di++)
						{
							sx = i * 2 + di;
							sy = j * 2 + dj;

							if (sx < atlas->rect.w && sy < atlas->rect.h)
							{
								Uint32 px = sp[(atlas->rect.y + sy) * spitch + atlas->rect.x + sx];

								if ((int)((px >> 24) & 0xFF) > ba)
								{
									ba = (px >> 24) & 0xFF;
									best = px;
								}
							}
						}
					}

					fr += (((best >> 16) & 0xFF) - fr) * tuneSharpness;
					fg += (((best >> 8) & 0xFF) - fg) * tuneSharpness;
					fb += ((best & 0xFF) - fb) * tuneSharpness;
					fa += (ba - fa) * tuneSharpness;
				}

				if (!isGlow)
				{
					float lum = 0.299f * fr + 0.587f * fg + 0.114f * fb;

					fr = (lum + (fr - lum) * tuneSaturation - 128.0f) * tuneContrast + 128.0f;
					fg = (lum + (fg - lum) * tuneSaturation - 128.0f) * tuneContrast + 128.0f;
					fb = (lum + (fb - lum) * tuneSaturation - 128.0f) * tuneContrast + 128.0f;
				}

				dp[(halfCursorY + j) * dpitch + halfCursorX + i] = ((Uint32)clamp255(fa) << 24) | ((Uint32)clamp255(fr) << 16) | ((Uint32)clamp255(fg) << 8) | (Uint32)clamp255(fb);

				if (direct)
				{
					Uint16 *d16 = (Uint16 *)((Uint8 *)halfDirectSurface->pixels + (halfCursorY + j) * halfDirectSurface->pitch);

					d16[halfCursorX + i] = (Uint16)(((clamp255(fr) >> 3) << 11) | ((clamp255(fg) >> 2) << 5) | (clamp255(fb) >> 3));
				}
			}
			else
			{
				dp[(halfCursorY + j) * dpitch + halfCursorX + i] = 0;
			}
		}
	}

	atlas->directSurface = direct ? halfDirectSurface : NULL;

	atlas->half.x = halfCursorX;
	atlas->half.y = halfCursorY;
	atlas->half.w = hw;
	atlas->half.h = hh;

	halfCursorX += hw;

	if (hh > halfRowHeight)
	{
		halfRowHeight = hh;
	}

	if (isGlow)
	{
		SDL_Surface *g = SDL_CreateRGBSurfaceWithFormat(0, hw, hh, 32, SDL_PIXELFORMAT_ARGB8888);

		for (j = 0 ; j < hh ; j++)
		{
			memcpy((Uint8 *)g->pixels + j * g->pitch, &dp[(atlas->half.y + j) * dpitch + atlas->half.x], hw * 4);
		}

		atlas->glowSurface = g;
	}

	/* Mirrored copies: sideways for everything the game can draw facing left
	 * (not map tiles or glows), upside down for the menu arrows. */
	if (strncmp(atlas->filename, "gfx/tilesets/", 13) != 0 && !isGlow)
	{
		atlas->halfFlipH = placeMirrored(&atlas->half, 1, 0);
	}

	if (strstr(atlas->filename, "arrow") != NULL)
	{
		atlas->halfFlipV = placeMirrored(&atlas->half, 0, 1);
	}
}

static void loadAtlasData(void)
{
	AtlasImage *atlas, *a;
	cJSON *root, *node;
	char *text;
	unsigned long i;
	SDL_Surface *loaded, *atlasSurface;
	SDL_Texture *halfTexture;

	tuneSharpness = tuneFromEnv("WC_SHARPNESS", tuneSharpness);
	tuneSaturation = tuneFromEnv("WC_SATURATION", tuneSaturation);
	tuneContrast = tuneFromEnv("WC_CONTRAST", tuneContrast);

	loaded = IMG_Load(getFileLocation("gfx/atlas/atlas.png"));
	atlasSurface = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(loaded);

	halfAtlasSurface = SDL_CreateRGBSurfaceWithFormat(0, HALF_ATLAS_SIZE, HALF_ATLAS_SIZE, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_FillRect(halfAtlasSurface, NULL, 0);
	halfDirectSurface = SDL_CreateRGBSurfaceWithFormat(0, HALF_ATLAS_SIZE, HALF_ATLAS_SIZE, 16, SDL_PIXELFORMAT_RGB565);
	SDL_FillRect(halfDirectSurface, NULL, 0);

	text = readFile(getFileLocation("data/atlas/atlas.json"));

	root = cJSON_Parse(text);

	for (node = root->child ; node != NULL ; node = node->next)
	{
		atlas = malloc(sizeof(AtlasImage));
		memset(atlas, 0, sizeof(AtlasImage));

		STRNCPY(atlas->filename, cJSON_GetObjectItem(node, "filename")->valuestring, MAX_DESCRIPTION_LENGTH);
		atlas->rect.x = cJSON_GetObjectItem(node, "x")->valueint;
		atlas->rect.y = cJSON_GetObjectItem(node, "y")->valueint;
		atlas->rect.w = cJSON_GetObjectItem(node, "w")->valueint;
		atlas->rect.h = cJSON_GetObjectItem(node, "h")->valueint;
		addHalfImage(atlasSurface, atlas);

		i = hashcode(atlas->filename) % NUM_ATLAS_BUCKETS;

		a = &atlases[i];

		/* horrible bit to look for the tail */
		while (a->next)
		{
			a = a->next;
		}

		a->next = atlas;
	}

	cJSON_Delete(root);

	free(text);

	halfTexture = toTexture(halfAtlasSurface, 0);
	SDL_SetTextureBlendMode(halfTexture, SDL_BLENDMODE_BLEND);

	for (i = 0 ; i < NUM_ATLAS_BUCKETS ; i++)
	{
		for (a = atlases[i].next ; a != NULL ; a = a->next)
		{
			/* The game tints and fades atlas images through ->texture (glows,
			 * particles, menu arrows), so that must be the texture that is drawn. */
			a->texture = halfTexture;
			a->halfTexture = halfTexture;
		}
	}

	SDL_FreeSurface(atlasSurface);
	SDL_FreeSurface(halfAtlasSurface);
}
