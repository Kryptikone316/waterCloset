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
#include "draw.h"

static void initColor(SDL_Color *c, int r, int g, int b);

static int prevFilled, curFilled;

/* The device's display driver takes a 640x480 RGB565 frame. app.hwSurface
 * (init.c) is exactly that, and the renderer draws straight into it, so the
 * finished frame is handed over as it is with no conversion or copy. The 640x360
 * game picture sits in the middle of it (SDL_RenderSetLogicalSize letterboxes
 * it) and the black bars are never drawn over: the surface starts black and
 * SDL clips every draw to the picture. */
/* Frame timing, read by main.c's once-per-two-seconds log line. */
unsigned perfDrawCalls = 0;
long long perfConvertNs = 0, perfHwNs = 0, perfRectNs = 0, perfBlitNs = 0, perfTextNs = 0;

static long long perfNowNs(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void presentToHardware(void)
{
	long long t0 = perfNowNs();

	hwdisp_present(app.hwSurface->pixels, HW_PHYS_W, HW_PHYS_H, app.hwSurface->pitch);

	prevFilled = curFilled;
	curFilled = 0;

	perfHwNs += perfNowNs() - t0;
}

void initGraphics(void)
{
	initColor(&app.colors.red, 255, 0, 0);
	initColor(&app.colors.orange, 255, 128, 0);
	initColor(&app.colors.yellow, 255, 255, 0);
	initColor(&app.colors.green, 0, 255, 0);
	initColor(&app.colors.blue, 0, 0, 255);
	initColor(&app.colors.cyan, 0, 255, 255);
	initColor(&app.colors.purple, 255, 0, 255);
	initColor(&app.colors.white, 255, 255, 255);
	initColor(&app.colors.black, 0, 0, 0);
	initColor(&app.colors.lightGrey, 192, 192, 192);
	initColor(&app.colors.darkGrey, 128, 128, 128);

}

/* No app.backBuffer render-target texture on this device on purpose -
 * SDL_RenderSetLogicalSize (init.c) only scales draw calls made against the
 * renderer's DEFAULT target, not a custom SDL_TEXTUREACCESS_TARGET texture.
 * Drawing straight to the default target (our owned, physically-640x360
 * app.hwSurface) is what actually makes every individual tile/entity blit
 * cheaper, not just the one final frame copy the stock backBuffer design
 * would otherwise still do at full logical resolution. */
/* The stage fills the whole picture with its own colour as the first thing it
 * draws (stage.c, drawGame), so clearing the frame to black first just writes
 * the same pixels twice. The stage says so with markFullFill() and the next
 * frame's clear is skipped; any other screen does not, so the clear comes back
 * on the first frame after leaving the stage. */
void markFullFill(void)
{
	curFilled = 1;
}

void prepareScene(void)
{
	if (prevFilled)
	{
		return;
	}

	/* Fill just the game picture (logical coordinates), not the whole surface:
	 * the black bars above and below it stay as they are. */
	SDL_Rect picture = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };

	SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
	SDL_RenderFillRect(app.renderer, &picture);
}


void presentScene(void)
{
	if (app.dev.debug)
	{
		drawText(SCREEN_WIDTH - 5, SCREEN_HEIGHT - 30, 32, TEXT_RIGHT, app.colors.white, "%dfps | Ents: %d | Cols: %d | Draw: %d", app.dev.fps, app.dev.ents, app.dev.collisions, app.dev.drawing);
	}

	SDL_RenderPresent(app.renderer);

	presentToHardware();
}

void blit(SDL_Texture *texture, int x, int y, int center, SDL_RendererFlip flip)
{
	SDL_Rect dest;

	dest.x = x;
	dest.y = y;
	SDL_QueryTexture(texture, NULL, NULL, &dest.w, &dest.h);
	perfDrawCalls++;

	if (center)
	{
		dest.x -= dest.w / 2;
		dest.y -= dest.h / 2;
	}

	/* RenderCopyEx is by far the heaviest call on the software renderer (it
	 * makes several temporary surfaces and runs a rotation pass even at angle 0),
	 * so use the plain copy whenever there is no flip. */
	if (flip == SDL_FLIP_NONE)
	{
		SDL_RenderCopy(app.renderer, texture, NULL, &dest);
	}
	else
	{
		SDL_RenderCopyEx(app.renderer, texture, NULL, &dest, 0, NULL, flip);
	}
}

void blitAtlasImage(AtlasImage *atlasImage, int x, int y, int center, SDL_RendererFlip flip)
{
	SDL_Rect dest;

	dest.x = x;
	dest.y = y;

	if (center)
	{
		dest.x -= (atlasImage->rect.w / 2);
		dest.y -= (atlasImage->rect.h / 2);
	}

	/* Draw the half-size copy of the image (atlas.c) at twice its size in game
	 * coordinates, which the renderer's 0.5 scale turns into exactly its own
	 * size on the screen: a plain 1:1 copy with no scaling. */
	dest.w = atlasImage->half.w * 2;
	dest.h = atlasImage->half.h * 2;

	/* The renderer halves game coordinates. At an odd one the copy would land on a
	 * half pixel and be stretched by a pixel, so land it on the pixel grid. */
	dest.x &= ~1;
	dest.y &= ~1;

	perfDrawCalls++;
	long long pt0 = perfNowNs();

	if (atlasImage->directSurface && flip == SDL_FLIP_NONE)
	{
		/* Opaque map tile: copy it straight onto the screen surface. The picture
		 * sits at the surface's top-left x and is centred vertically; clip to it
		 * so nothing is drawn into the black bars. */
		SDL_Rect picture = { 0, (HW_PHYS_H - SCREEN_HEIGHT / 2) / 2, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 };
		SDL_Rect clip, to;

		SDL_RenderFlush(app.renderer);

		SDL_GetClipRect(app.hwSurface, &clip);

		if (clip.x != picture.x || clip.y != picture.y || clip.w != picture.w || clip.h != picture.h)
		{
			SDL_SetClipRect(app.hwSurface, &picture);
		}

		/* floor(x / 2) for negative x too */
		to.x = dest.x >= 0 ? dest.x / 2 : -((1 - dest.x) / 2);
		to.y = (dest.y >= 0 ? dest.y / 2 : -((1 - dest.y) / 2)) + picture.y;

		SDL_BlitSurface(atlasImage->directSurface, &atlasImage->half, app.hwSurface, &to);
	}
	else if (flip == SDL_FLIP_NONE)
	{
		SDL_RenderCopy(app.renderer, atlasImage->texture, &atlasImage->half, &dest);
	}
	else if (flip == SDL_FLIP_HORIZONTAL && atlasImage->halfFlipH.w > 0)
	{
		SDL_RenderCopy(app.renderer, atlasImage->texture, &atlasImage->halfFlipH, &dest);
	}
	else if (flip == SDL_FLIP_VERTICAL && atlasImage->halfFlipV.w > 0)
	{
		SDL_RenderCopy(app.renderer, atlasImage->texture, &atlasImage->halfFlipV, &dest);
	}
	else
	{
		SDL_RenderCopyEx(app.renderer, atlasImage->texture, &atlasImage->half, &dest, 0, NULL, flip);
	}

	perfBlitNs += perfNowNs() - pt0;
}

/* Lights, glows and sparks are tinted and faded copies of a soft white blob.
 * Asking SDL for that (colour and alpha modulation plus blending) sends every
 * pixel through its slowest generic path, and a stage with many coins and lights
 * on screen could not hold its frame rate. This does the same sum straight into
 * the RGB565 screen surface: dst += (tint * src - dst) * alpha. Position,
 * rounding and clipping follow blitAtlasImage(). */
void blitGlow(AtlasImage *atlasImage, int x, int y, int center, int r, int g, int b, int a)
{
	SDL_Surface *src = atlasImage->glowSurface;
	int top = (HW_PHYS_H - SCREEN_HEIGHT / 2) / 2;
	int dx, dy, x0, y0, i, j, i0, i1, j0, j1;

	if (src == NULL)
	{
		return;
	}

	dx = x;
	dy = y;

	if (center)
	{
		dx -= atlasImage->rect.w / 2;
		dy -= atlasImage->rect.h / 2;
	}

	dx &= ~1;
	dy &= ~1;

	x0 = dx >= 0 ? dx / 2 : -((1 - dx) / 2);
	y0 = (dy >= 0 ? dy / 2 : -((1 - dy) / 2)) + top;

	i0 = x0 < 0 ? -x0 : 0;
	i1 = x0 + src->w > SCREEN_WIDTH / 2 ? SCREEN_WIDTH / 2 - x0 : src->w;
	j0 = y0 < top ? top - y0 : 0;
	j1 = y0 + src->h > top + SCREEN_HEIGHT / 2 ? top + SCREEN_HEIGHT / 2 - y0 : src->h;

	if (i0 >= i1 || j0 >= j1)
	{
		return;
	}

	perfDrawCalls++;
	long long pt0 = perfNowNs();

	SDL_RenderFlush(app.renderer);

	for (j = j0 ; j < j1 ; j++)
	{
		const Uint32 *sp = (const Uint32 *)((Uint8 *)src->pixels + j * src->pitch);
		unsigned short *dp = (unsigned short *)((Uint8 *)app.hwSurface->pixels + (y0 + j) * app.hwSurface->pitch) + x0;

		for (i = i0 ; i < i1 ; i++)
		{
			Uint32 s = sp[i];
			int sa = ((s >> 24) * (a + 1)) >> 8;

			if (sa > 0)
			{
				unsigned short p = dp[i];
				int dr = p >> 11, dg = (p >> 5) & 63, db = p & 31;
				int tr = ((((s >> 16) & 0xFF) * (r + 1)) >> 8) >> 3;
				int tg = ((((s >> 8) & 0xFF) * (g + 1)) >> 8) >> 2;
				int tb = (((s & 0xFF) * (b + 1)) >> 8) >> 3;

				dr += ((tr - dr) * sa) >> 8;
				dg += ((tg - dg) * sa) >> 8;
				db += ((tb - db) * sa) >> 8;

				dp[i] = (dr << 11) | (dg << 5) | db;
			}
		}
	}

	perfBlitNs += perfNowNs() - pt0;
}

/* A translucent black rectangle (menu and tips overlays, HUD bars, fades) just
 * darkens what is already there. SDL's generic blend of a rectangle onto 16-bit
 * pixels is very slow, so darken the RGB565 pixels directly, with a small lookup
 * table per colour channel. The game picture is drawn at half size and centred
 * in the 640x480 surface, which is what the coordinates below undo. */
static int darkenRect(int x, int y, int w, int h, int alpha)
{
	int x0, y0, x1, y1, px, py, i;
	int f = ((255 - alpha) * 256 + 127) / 255;
	unsigned short rb[32], gg[64];

	x0 = x / 2;
	y0 = y / 2 + (HW_PHYS_H - SCREEN_HEIGHT / 2) / 2;
	x1 = (x + w + 1) / 2;
	y1 = (y + h + 1) / 2 + (HW_PHYS_H - SCREEN_HEIGHT / 2) / 2;

	if (x0 < 0) x0 = 0;
	if (x1 > HW_PHYS_W) x1 = HW_PHYS_W;
	if (y0 < (HW_PHYS_H - SCREEN_HEIGHT / 2) / 2) y0 = (HW_PHYS_H - SCREEN_HEIGHT / 2) / 2;
	if (y1 > (HW_PHYS_H + SCREEN_HEIGHT / 2) / 2) y1 = (HW_PHYS_H + SCREEN_HEIGHT / 2) / 2;

	if (x0 >= x1 || y0 >= y1)
	{
		return 1;
	}

	for (i = 0 ; i < 32 ; i++)
	{
		rb[i] = (i * f) >> 8;
	}

	for (i = 0 ; i < 64 ; i++)
	{
		gg[i] = (i * f) >> 8;
	}

	SDL_RenderFlush(app.renderer);

	for (py = y0 ; py < y1 ; py++)
	{
		unsigned short *row = (unsigned short *)((Uint8 *)app.hwSurface->pixels + py * app.hwSurface->pitch);

		for (px = x0 ; px < x1 ; px++)
		{
			unsigned short p = row[px];

			row[px] = (rb[p >> 11] << 11) | (gg[(p >> 5) & 63] << 5) | rb[p & 31];
		}
	}

	return 1;
}

void drawRect(int x, int y, int w, int h, int r, int g, int b, int a)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;

	long long pt0 = perfNowNs();

	if (r == 0 && g == 0 && b == 0 && a > 0 && a < 255 && darkenRect(x, y, w, h, a))
	{
		perfRectNs += perfNowNs() - pt0;
		return;
	}

	SDL_SetRenderDrawBlendMode(app.renderer, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(app.renderer, r, g, b, a);
	SDL_RenderFillRect(app.renderer, &rect);

	perfRectNs += perfNowNs() - pt0;
}

void drawOutlineRect(int x, int y, int w, int h, int r, int g, int b, int a)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;

	SDL_SetRenderDrawBlendMode(app.renderer, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(app.renderer, r, g, b, a);
	SDL_RenderDrawRect(app.renderer, &rect);
}

static void initColor(SDL_Color *c, int r, int g, int b)
{
	memset(c, 0, sizeof(SDL_Color));
	c->r = r;
	c->g = g;
	c->b = b;
	c->a = 255;
}
