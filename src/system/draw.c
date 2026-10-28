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

#include "draw.h"

static void initColor(SDL_Color *c, int r, int g, int b);

/* Real HCGE canvas on this device is 640x480 4:3 (confirmed on real
 * hardware via ccleste's run.log - see cceleste_handoff memory). app.hwSurface
 * is physically HW_PHYS_W x HW_PHYS_H = 640x360 (init.c; the game's logical
 * 1280x720 coordinate space is scaled onto it by SDL_RenderSetLogicalSize,
 * not here) - exactly the panel's width already, so this is a direct 1:1
 * pixel copy (format convert only, no scaling) plus a vertical letterbox
 * to pad 360 up to 480. */
#define HW_OUT_W 640
#define HW_OUT_H 480
static unsigned short hwPresentBuf[HW_OUT_W * HW_OUT_H];

static void presentToHardware(void)
{
	int x, y;
	const int yOff = (HW_OUT_H - HW_PHYS_H) / 2;

	memset(hwPresentBuf, 0, sizeof(hwPresentBuf));

	for (y = 0 ; y < HW_PHYS_H ; y++)
	{
		Uint32 *srow = (Uint32 *)((Uint8 *)app.hwSurface->pixels + y * app.hwSurface->pitch);
		unsigned short *drow = hwPresentBuf + (y + yOff) * HW_OUT_W;

		for (x = 0 ; x < HW_PHYS_W ; x++)
		{
			Uint32 px = srow[x];
			drow[x] = (unsigned short)(((px >> 8) & 0xF800) | ((px >> 5) & 0x07E0) | ((px >> 3) & 0x001F));
		}
	}

	hwdisp_present(hwPresentBuf, HW_OUT_W, HW_OUT_H, HW_OUT_W * sizeof(unsigned short));
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
void prepareScene(void)
{
	SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
	SDL_RenderClear(app.renderer);
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

	if (center)
	{
		dest.x -= dest.w / 2;
		dest.y -= dest.h / 2;
	}

	SDL_RenderCopyEx(app.renderer, texture, NULL, &dest, 0, NULL, flip);
}

void blitAtlasImage(AtlasImage *atlasImage, int x, int y, int center, SDL_RendererFlip flip)
{
	SDL_Rect dest;

	dest.x = x;
	dest.y = y;
	dest.w = atlasImage->rect.w;
	dest.h = atlasImage->rect.h;

	if (center)
	{
		dest.x -= (dest.w / 2);
		dest.y -= (dest.h / 2);
	}

	SDL_RenderCopyEx(app.renderer, atlasImage->texture, &atlasImage->rect, &dest, 0, NULL, flip);
}

void drawRect(int x, int y, int w, int h, int r, int g, int b, int a)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;

	SDL_SetRenderDrawBlendMode(app.renderer, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(app.renderer, r, g, b, a);
	SDL_RenderFillRect(app.renderer, &rect);
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
