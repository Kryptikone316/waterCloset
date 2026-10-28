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

#include <stdbool.h>

#include "sound.h"

#include "../../sf3500/sf3500_mixer.hpp"
#define STB_VORBIS_HEADER_ONLY
#include "../../sf3500/stb_vorbis.h"

static void loadSounds(void);

static Sf3500Voice *sounds[SND_MAX];
static char *musicFilenames[] = {
	"music/contemplation.ogg", "music/puzzle-1-a.ogg", "music/puzzle-1-b.ogg"
};
static int lastRandomMusic;
static int channelVolumes[CH_MAX];
static float sfxVolume = 1.0f;
static float musicVolumeGain = 1.0f;

/* Music: one streaming voice, re-pointed at a new file by loadMusic(). Only
 * ever called with loop=1 in this codebase (grepped all call sites), so the
 * fill callback always self-loops - no need to honor a non-looping case. */
static stb_vorbis *musicVorbis = NULL;
static int musicChannels = 2;
static Sf3500Voice *musicVoice = NULL;

void setSoundVolume(int vol)
{
	sfxVolume = vol / 100.0f;
}

void setMusicVolume(int vol)
{
	musicVolumeGain = vol / 100.0f;

	if (musicVoice)
	{
		sf3500_voice_set_gain(musicVoice, musicVolumeGain);
	}
}

static int musicFill(void *userdata, short *dst, int frames_wanted)
{
	int got;

	(void)userdata;

	if (!musicVorbis)
	{
		memset(dst, 0, frames_wanted * 2 * sizeof(short));
		return frames_wanted;
	}

	got = stb_vorbis_get_samples_short_interleaved(musicVorbis, musicChannels, dst, frames_wanted * musicChannels);

	if (got < frames_wanted)
	{
		stb_vorbis_seek_start(musicVorbis);
		got += stb_vorbis_get_samples_short_interleaved(musicVorbis, musicChannels, dst + got * musicChannels, (frames_wanted - got) * musicChannels);
	}

	return got;
}

/* One-shot decode-to-memory + resample to 48kHz stereo S16, same pattern as
 * every other static SFX voice on this device - stb_vorbis_decode_filename
 * does the decode, SDL_BuildAudioCVT/SDL_ConvertAudio does the resample
 * (works on any PCM buffer regardless of where it came from, not just
 * SDL_LoadWAV output). The resulting buffer is never freed - it belongs to
 * the voice for the program's lifetime, same as every other port. */
static Sf3500Voice *loadOggVoice(const char *filename)
{
	const char *path;
	short *decoded;
	int channels, rate, decodedSamples;
	SDL_AudioCVT cvt;
	int frames;

	path = getFileLocation((char *)filename);

	decodedSamples = stb_vorbis_decode_filename(path, &channels, &rate, &decoded);

	if (decodedSamples <= 0 || !decoded)
	{
		SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN, "stb_vorbis_decode_filename failed for '%s'", path);
		return NULL;
	}

	if (SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, (Uint8)channels, rate, AUDIO_S16SYS, 2, 48000) < 0)
	{
		free(decoded);
		return NULL;
	}

	cvt.len = decodedSamples * channels * (int)sizeof(short);
	cvt.buf = (Uint8 *)malloc((size_t)cvt.len * cvt.len_mult);
	memcpy(cvt.buf, decoded, (size_t)cvt.len);
	free(decoded);

	SDL_ConvertAudio(&cvt);

	frames = cvt.len_cvt / (2 * (int)sizeof(short));

	return sf3500_voice_create_static((const short *)cvt.buf, frames);
}

void initSounds(void)
{
	int i;

	memset(sounds, 0, sizeof(Sf3500Voice *) * SND_MAX);

	lastRandomMusic = -1;

	loadSounds();

	musicVoice = sf3500_voice_create_stream(musicFill, NULL, 48000, 2);

	setSoundVolume(app.config.soundVolume);
	setMusicVolume(app.config.musicVolume);

	for (i = 0 ; i < CH_MAX ; i++)
	{
		channelVolumes[i] = 0;
	}
}

void loadMusic(char *filename)
{
	const char *path;

	if (musicVoice)
	{
		sf3500_voice_stop(musicVoice);
	}

	if (musicVorbis)
	{
		stb_vorbis_close(musicVorbis);
		musicVorbis = NULL;
	}

	path = getFileLocation(filename);

	musicVorbis = stb_vorbis_open_filename(path, NULL, NULL);

	if (musicVorbis)
	{
		stb_vorbis_info info = stb_vorbis_get_info(musicVorbis);
		musicChannels = info.channels;
	}
}

void playMusic(int loop)
{
	(void)loop; /* always looping - see musicFill's own comment */

	if (musicVoice)
	{
		sf3500_voice_play(musicVoice);
	}
}

void playSound(int id, int channel)
{
	(void)channel;

	if (sounds[id])
	{
		sf3500_voice_set_relative(sounds[id], 1);
		sf3500_voice_set_gain(sounds[id], sfxVolume);
		sf3500_voice_play(sounds[id]);
	}
}

/* Distance-based volume gating is unchanged from the original (same
 * channelVolumes[] "don't steal a louder channel" heuristic). Panning is
 * simplified from the original's angle/bearing math to the mixer's own
 * existing listener-x model (sf3500_mixer's documented "simple, deliberately
 * non-physical left/right split" - good enough here too, matches the
 * project's own established tradeoff rather than reimplementing bearing
 * math against a pan model that doesn't have a concept of angle). */
void playPositionalSound(int id, int channel, int srcX, int srcY, int destX, int destY)
{
	float distance, vol;

	(void)srcY; (void)destY;

	distance = getDistance(destX, destY, srcX, srcY);

	if (distance <= SCREEN_WIDTH)
	{
		vol = 255;
		vol /= SCREEN_WIDTH;
		vol *= distance;

		if (vol >= channelVolumes[channel] && sounds[id])
		{
			channelVolumes[channel] = (int)vol;

			sf3500_mixer_set_listener_x((float)destX);
			sf3500_voice_set_relative(sounds[id], 0);
			sf3500_voice_set_x(sounds[id], (float)srcX);
			sf3500_voice_set_gain(sounds[id], ((255.0f - vol) / 255.0f) * sfxVolume);
			sf3500_voice_play(sounds[id]);
		}
	}
}

void pauseSound(void)
{
	/* ponytail: no pause-all in sf3500_mixer's voice API (play/stop only) -
	 * add per-voice pause tracking if this is ever actually missed; nothing
	 * in this codebase's own flow calls resumeSound() without the game
	 * already being in a state where sounds restart naturally. */
}

void resumeSound(void)
{
}

static void loadSounds(void)
{
	sounds[SND_JUMP] = loadOggVoice("sound/331381__qubodup__public-domain-jump-sound.ogg");
	sounds[SND_COIN] = loadOggVoice("sound/135936__bradwesson__collectcoin.ogg");
	sounds[SND_FLUSH] = loadOggVoice("sound/108413__kyle1katarn__toilet.ogg");
	sounds[SND_PLUNGER] = loadOggVoice("sound/plunger.ogg");
	sounds[SND_KEY] = loadOggVoice("sound/mortice_key_drop_on_concrete_floor.ogg");
	sounds[SND_DEATH] = loadOggVoice("sound/death.ogg");
	sounds[SND_CLONE] = loadOggVoice("sound/clone.ogg");
	sounds[SND_NUDGE] = loadOggVoice("sound/nudge.ogg");
	sounds[SND_TELEPORT] = loadOggVoice("sound/teleport.ogg");
	sounds[SND_WIPE] = loadOggVoice("sound/wipe.ogg");
	sounds[SND_SPIT] = loadOggVoice("sound/434479__dersuperanton__splatter.ogg");
	sounds[SND_SPIT_HIT] = loadOggVoice("sound/446115__justinvoke__wet-splat.ogg");
	sounds[SND_MANHOLE_COVER] = loadOggVoice("sound/429167__aropson__heavy-clang-1.ogg");
	sounds[SND_CLOCK] = loadOggVoice("sound/clock.ogg");
	sounds[SND_EXPIRED] = loadOggVoice("sound/expired.ogg");
	sounds[SND_NEGATIVE] = loadOggVoice("sound/negative.ogg");
	sounds[SND_FANFARE] = loadOggVoice("sound/449069__ricniclas__fanfare.ogg");
	sounds[SND_DOOR] = loadOggVoice("sound/426770__cmilan__drawer-close.ogg");
	sounds[SND_TRAFFIC_LIGHT] = loadOggVoice("sound/264446__kickhat__open-button-1.ogg");
	sounds[SND_FAIL] = loadOggVoice("sound/fail.ogg");
	sounds[SND_ITEM] = loadOggVoice("sound/item.ogg");
	sounds[SND_TIP] = loadOggVoice("sound/tip.ogg");
	sounds[SND_PLUNGE] = loadOggVoice("sound/plunge.ogg");
	sounds[SND_PRESSURE_PLATE] = loadOggVoice("sound/245242__noirenex__beepping.ogg");
	sounds[SND_SPLASH] = loadOggVoice("sound/398032__swordofkings128__splash.ogg");
	sounds[SND_DRIP] = loadOggVoice("sound/25879__acclivity__drip1.ogg");
	sounds[SND_SQUIRT] = loadOggVoice("sound/258047__jagadamba__water-spraying-from-a-bottle-02.ogg");
	sounds[SND_INFLATE] = loadOggVoice("sound/110043__sandyrb__fart-005.ogg");
	sounds[SND_DEFLATE] = loadOggVoice("sound/110051__sandyrb__fart-013.ogg");
}

void loadRandomStageMusic(void)
{
	int r;

	r = rand() % (sizeof(musicFilenames) / sizeof(char*));

	if (r != lastRandomMusic)
	{
		lastRandomMusic = r;

		loadMusic(musicFilenames[r]);

		playMusic(1);
	}
}

void destroySounds(void)
{
	int i;

	for (i = 0 ; i < SND_MAX ; i++)
	{
		if (sounds[i])
		{
			sf3500_voice_destroy(sounds[i]);
		}
	}

	if (musicVoice)
	{
		sf3500_voice_destroy(musicVoice);
	}

	if (musicVorbis)
	{
		stb_vorbis_close(musicVorbis);
	}
}
