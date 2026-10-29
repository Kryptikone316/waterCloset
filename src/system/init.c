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

#include "init.h"

static void showLoadingStep(float step, float maxSteps);

void initSDL(void)
{
	if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) < 0)
	{
		printf("Couldn't initialize SDL: %s\n", SDL_GetError());
		exit(1);
	}

	if (hwdisp_init() != 0)
	{
		printf("hwdisp_init failed\n");
		exit(1);
	}

	joykey_init();

	if (!sf3500_mixer_init())
	{
		printf("sf3500_mixer_init failed, continuing without audio\n");
	}

	createSaveFolder();

	loadConfig();

	/* No real window/display on this device - app.renderer is a software
	 * renderer bound to an owned surface we read directly at present time
	 * (same "own the final pixel buffer, skip the accelerated path
	 * entirely" fix as every other SDL2 port on this hardware - there is
	 * no GPU here, SDL_RENDERER_ACCELERATED would have silently fallen
	 * back to software anyway, just via a much slower SDL_RenderReadPixels
	 * round-trip at present time instead of a direct pointer read).
	 *
	 * The owned surface is the device's whole 640x480 RGB565 canvas (HW_PHYS_W/H), not
	 * SCREEN_WIDTH x SCREEN_HEIGHT - SDL_RenderSetLogicalSize makes the
	 * renderer transparently scale every draw call from the game's 1280x720
	 * logical coordinate space down onto this smaller physical surface, the
	 * same mechanism the stock game's own "640 x 360" Window Size option
	 * relies on. This is what actually lowers the real rasterization cost
	 * on this GPU-less device - unlike a downscale applied only at the
	 * final present step, every individual tile/entity blit is now cheaper,
	 * not just the one full-frame copy at the end. */
	app.hwSurface = SDL_CreateRGBSurfaceWithFormat(0, HW_PHYS_W, HW_PHYS_H, 16, SDL_PIXELFORMAT_RGB565);
	app.renderer = SDL_CreateSoftwareRenderer(app.hwSurface);
	SDL_RenderSetLogicalSize(app.renderer, SCREEN_WIDTH, SCREEN_HEIGHT);

	IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

	if (TTF_Init() < 0)
	{
		printf("Couldn't initialize SDL TTF: %s\n", SDL_GetError());
		exit(1);
	}

	SDL_ShowCursor(0);
}

void initGame(void)
{
	int i, numInitFuns;
	void (*initFuncs[]) (void) = {
		initLookups,
		initAtlas,
		initFonts,
		initSounds,
		initWidgets,
		initEntityFactory,
		initParticles,
		initStageMetaData
	};

	srand(time(NULL));

	numInitFuns = sizeof(initFuncs) / sizeof(void*);

	initGraphics();

	for (i = 0 ; i < numInitFuns ; i++)
	{
		showLoadingStep(i + 1, numInitFuns);

		initFuncs[i]();
	}
}

static void showLoadingStep(float step, float maxSteps)
{
	SDL_Rect r;
	float percent;

	prepareScene();

	r.w = 600;
	r.h = 6;
	r.x = (SCREEN_WIDTH - r.w) / 2;
	r.y = (SCREEN_HEIGHT - r.h) / 2;

	percent = step;
	percent /= maxSteps;

	drawRect(r.x, r.y, r.w, r.h, 64, 96, 128, 255);
	drawRect(r.x, r.y, r.w * percent, r.h, 128, 192, 255, 255);

	presentScene();

	SDL_Delay(1);
}

void cleanup(void)
{
	destroyTextures();

	destroySounds();

	sf3500_mixer_deinit();

	hwdisp_deinit();

	SDL_DestroyRenderer(app.renderer);

	SDL_FreeSurface(app.hwSurface);

	TTF_Quit();

	SDL_Quit();
}
