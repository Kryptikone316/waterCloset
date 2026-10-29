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

#include "main.h"
#include <sys/resource.h>
#include <time.h>

extern unsigned perfDrawCalls;
extern void hwaudio_get_stats(unsigned *underruns, unsigned *dropped, unsigned *fill);
extern long long perfConvertNs, perfHwNs, perfRectNs, perfBlitNs, perfTextNs;

static long long perfNowNs(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void handleCommandLine(int argc, char *argv[]);
static void capFrameRate(long *then, float *remainder);

int main(int argc, char *argv[])
{
	long then, nextSecond;
	float remainder;
	int frames;
	long long t0, t1, t2, t3, lastT0 = 0, perfLogicNs = 0, perfDrawNs = 0, perfPresentNs = 0, perfMaxIntervalNs = 0, perfIntervalNs = 0, perfMaxWorkNs = 0, perfTargetNs;
	int perfLate = 0;
	int perfFrames = 0, drawEvery, perfLog, frameNo = 0, drawThisFrame;

	memset(&app, 0, sizeof(App));

	/* nice: a lower number makes the scheduler favour this game over the small
	 * background jobs the system runs now and then (the launcher script wakes
	 * every fifth of a second to see whether the game has quit). */
	if (getenv("WC_NICE") && atoi(getenv("WC_NICE")) != 0)
	{
		int nv = atoi(getenv("WC_NICE"));

		fprintf(stderr, "nice: asked for %d, setpriority returned %d\n", nv, setpriority(PRIO_PROCESS, 0, nv));
	}

	app.texturesTail = &app.texturesHead;

	initSDL();

	atexit(cleanup);

	initGame();

	handleCommandLine(argc, argv);

	/* draw_every: the game logic runs on every frame (that is the game speed), but a
	 * picture is only drawn and shown on every Nth one. */
	drawEvery = wcEnvInt("WC_DRAW_EVERY", 1);
	perfLog = wcEnvInt("WC_PERF_LOG", 1);
	perfTargetNs = 1000000000LL / wcEnvInt("WC_FPS", 60);

	if (drawEvery < 1 || drawEvery > 4)
	{
		drawEvery = 1;
	}

	then = SDL_GetTicks();

	frames = remainder = 0;

	nextSecond = SDL_GetTicks() + 1000;

	while (1)
	{
		t0 = perfNowNs();

		if (lastT0)
		{
			perfIntervalNs += t0 - lastT0;
			if (t0 - lastT0 > perfMaxIntervalNs) perfMaxIntervalNs = t0 - lastT0;
			if (t0 - lastT0 > perfTargetNs * 5 / 4) perfLate++;
		}
		lastT0 = t0;

		drawThisFrame = (frameNo++ % drawEvery) == 0;

		if (drawThisFrame)
		{
			prepareScene();
		}

		doInput();

		app.delegate.logic();

		t1 = perfNowNs();

		if (drawThisFrame)
		{
			app.delegate.draw();
		}

		t2 = perfNowNs();

		if (drawThisFrame)
		{
			presentScene();
		}

		t3 = perfNowNs();

		if (t3 - t0 > perfMaxWorkNs) perfMaxWorkNs = t3 - t0;

		perfLogicNs += t1 - t0;
		perfDrawNs += t2 - t1;
		perfPresentNs += t3 - t2;

		if (++perfFrames == 120)
		{
			if (perfLog)
			{
				static unsigned lastUnder, lastDropped;
				unsigned under, dropped, fill;

				hwaudio_get_stats(&under, &dropped, &fill);
				fprintf(stderr, "audio: %u underruns, %u dropped frames this window; %u ms buffered\n", under - lastUnder, dropped - lastDropped, fill / 48);
				lastUnder = under;
				lastDropped = dropped;
			}

			if (perfLog) fprintf(stderr, "perf: %d frames, interval avg %.1f max %.1f ms | logic %.1f draw %.1f present %.1f (convert %.1f hw %.1f) ms | draw calls/frame %u | in draw: blits %.1f rects %.1f text %.1f ms | late frames %d, longest frame work %.1f ms\n",
				perfFrames, perfIntervalNs / 1e6 / (perfFrames - 1), perfMaxIntervalNs / 1e6, perfLogicNs / 1e6 / perfFrames, perfDrawNs / 1e6 / perfFrames,
				perfPresentNs / 1e6 / perfFrames, perfConvertNs / 1e6 / perfFrames, perfHwNs / 1e6 / perfFrames, perfDrawCalls / perfFrames, perfBlitNs / 1e6 / perfFrames, perfRectNs / 1e6 / perfFrames, perfTextNs / 1e6 / perfFrames, perfLate, perfMaxWorkNs / 1e6);
			if (perfLog) fflush(stderr);
			perfLate = 0; perfMaxWorkNs = 0; perfFrames = 0; perfLogicNs = perfDrawNs = perfPresentNs = perfIntervalNs = perfMaxIntervalNs = 0;
			perfConvertNs = perfHwNs = perfRectNs = perfBlitNs = perfTextNs = 0; perfDrawCalls = 0;
		}

		frames++;

		capFrameRate(&then, &remainder);

		if (SDL_GetTicks() > nextSecond)
		{
			game.stats[STAT_TIME]++;

			app.dev.fps = frames;

			nextSecond = SDL_GetTicks() + 1000;

			frames = 0;
		}
	}

	return 0;
}

static void handleCommandLine(int argc, char *argv[])
{
	int i;

	for (i = 1 ; i < argc ; i++)
	{
		if (strcmp(argv[i], "-stage") == 0)
		{
			initStage();

			stage.num = atoi(argv[i + 1]);

			loadStage(1);

			loadRandomStageMusic();
		}
		else if (strcmp(argv[i], "-ending") == 0)
		{
			memset(&stage, 0, sizeof(Stage));
			stage.entityTail = &stage.entityHead;

			initEnding();

			stage.num = -1;
		}

		if (strcmp(argv[i], "-debug") == 0)
		{
			app.dev.debug = 1;

			SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
		}
	}

	if (stage.num == 0)
	{
		loadGame();

		initTitle();
	}
}

/* Frame limiter. The game does one logic step per frame, so the frame rate IS the
 * game speed: 60 is the speed it was designed for (and its on-screen clock counts
 * 60 frames to the second); a lower limit makes everything slower, not just less
 * smooth. The target comes from WC_FPS (the fps= line in watercloset.txt). Each
 * frame is held to an absolute deadline, so the rate stays exactly on target
 * however long a frame took; if the game ever falls behind it starts again from
 * now instead of rushing to catch up. */
static long long limiterNextNs;
static long long limiterPeriodNs;

static void capFrameRate(long *then, float *remainder)
{
	long long now = perfNowNs();
	struct timespec t;

	if (limiterPeriodNs == 0)
	{
		const char *e = getenv("WC_FPS");
		int fps = e ? atoi(e) : 60;

		if (fps < 15 || fps > 240)
		{
			fps = 60;
		}

		limiterPeriodNs = 1000000000LL / fps;
		limiterNextNs = now;
	}

	limiterNextNs += limiterPeriodNs;

	if (limiterNextNs < now - 2 * limiterPeriodNs)
	{
		limiterNextNs = now;
	}

	if (limiterNextNs > now)
	{
		t.tv_sec = limiterNextNs / 1000000000LL;
		t.tv_nsec = limiterNextNs % 1000000000LL;

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
	}

	*then = SDL_GetTicks();
}
