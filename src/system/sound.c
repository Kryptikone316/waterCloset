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
/* The voice last started on each channel. Upstream clears channelVolumes[c] in a
 * Mix_ChannelFinished callback when the channel's sound ends; this mixer has no
 * such callback, so the record is cleared instead when that voice is found to
 * have stopped. Without it the first positional sound on a channel left its
 * value behind for good, and nearer sounds on that channel were silenced from
 * then on. */
static Sf3500Voice *channelVoice[CH_MAX];
static int channelSound[CH_MAX];

/* Each sound has a few voices sharing one sample, so a sound that is triggered
 * again while it is still playing (coins picked up in a row) plays again on top
 * instead of cutting itself off or being skipped. */
#define SND_INSTANCES 3
static Sf3500Voice *soundInstances[SND_MAX][SND_INSTANCES];
static int nextInstance[SND_MAX];

static Sf3500Voice *pickInstance(int id)
{
	int i;

	for (i = 0 ; i < SND_INSTANCES ; i++)
	{
		if (soundInstances[id][i] && !sf3500_voice_playing(soundInstances[id][i]))
		{
			return soundInstances[id][i];
		}
	}

	/* all busy: restart the oldest */
	nextInstance[id] = (nextInstance[id] + 1) % SND_INSTANCES;

	return soundInstances[id][nextInstance[id]];
}
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
	int frames, attempt;

	path = getFileLocation((char *)filename);

	decodedSamples = stb_vorbis_decode_filename(path, &channels, &rate, &decoded);

	/* Now and then a sound fails to load at start-up for no lasting reason (a different
	 * one each time, the file itself is fine), which left that sound silent for the
	 * whole session. Try again a few times before giving up. */
	for (attempt = 1 ; (decodedSamples <= 0 || !decoded) && attempt < 4 ; attempt++)
	{
		SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN, "retrying '%s' (attempt %d)", path, attempt + 1);

		SDL_Delay(50);

		decodedSamples = stb_vorbis_decode_filename(path, &channels, &rate, &decoded);
	}

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

	for (i = 0 ; i < SND_MAX ; i++)
	{
		int n;

		if (sounds[i])
		{
			soundInstances[i][0] = sounds[i];

			for (n = 1 ; n < SND_INSTANCES ; n++)
			{
				soundInstances[i][n] = sf3500_voice_clone_static(sounds[i]);
			}
		}
	}

	musicVoice = sf3500_voice_create_stream(musicFill, NULL, 48000, 2);

	setSoundVolume(app.config.soundVolume);
	setMusicVolume(app.config.musicVolume);

	fprintf(stderr, "volume: sound %d, music %d (in-game settings; 100 = full)\n", app.config.soundVolume, app.config.musicVolume);

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

	/* An optional music_pack folder next to the game holds replacement tracks with
	 * the same file names (see music_pack/README.txt). A track found there is used
	 * instead of the one that came with the game. */
	{
		static char packPath[MAX_FILENAME_LENGTH];
		const char *base = strrchr(filename, '/');
		FILE *f;

		snprintf(packPath, sizeof(packPath), "music_pack/%s", base ? base + 1 : filename);

		f = fopen(packPath, "rb");

		if (f)
		{
			fclose(f);
			path = packPath;
		}
	}

	fprintf(stderr, "music: loading %s\n", path);

	musicVorbis = stb_vorbis_open_filename(path, NULL, NULL);

	if (musicVorbis)
	{
		stb_vorbis_info info = stb_vorbis_get_info(musicVorbis);

		musicChannels = info.channels == 1 ? 1 : 2;

		fprintf(stderr, "music: %d Hz, %d channel(s)\n", info.sample_rate, info.channels);

		/* tell the mixer what the track really is so that it converts it to the
		 * output rate (the tracks are 44100 Hz; the output is 48000 Hz) */
		sf3500_voice_set_stream_format(musicVoice, info.sample_rate, musicChannels);
	}
}

void playMusic(int loop)
{
	(void)loop; /* always looping - see musicFill's own comment */

	/* music=0 in watercloset.txt leaves the music off (WC_MUSIC=0) */
	if (wcEnvInt("WC_MUSIC", 1) == 0)
	{
		return;
	}

	if (musicVoice)
	{
		sf3500_voice_play(musicVoice);
	}
}

void playSound(int id, int channel)
{
	Sf3500Voice *v;

	(void)channel;

	if (sounds[id])
	{
		v = pickInstance(id);

		sf3500_voice_set_relative(v, 1);
		sf3500_voice_set_gain(v, sfxVolume);
		sf3500_voice_play(v);
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
	Sf3500Voice *v;

	(void)srcY; (void)destY;

	distance = getDistance(destX, destY, srcX, srcY);

	if (distance <= SCREEN_WIDTH)
	{
		vol = 255;
		vol /= SCREEN_WIDTH;
		vol *= distance;

		if (channelVoice[channel] && !sf3500_voice_playing(channelVoice[channel]))
		{
			channelVolumes[channel] = 0;
			channelVoice[channel] = NULL;
		}

		/* The same sound again always plays (the next coin in a row); the
		 * "don't steal a louder channel" check is only for a different sound. */
		if (sounds[id] && (vol >= channelVolumes[channel] || (channelVoice[channel] && channelSound[channel] == id)))
		{
			v = pickInstance(id);

			channelVolumes[channel] = (int)vol;
			channelVoice[channel] = v;
			channelSound[channel] = id;

			sf3500_mixer_set_listener_x((float)destX);
			sf3500_voice_set_relative(v, 0);
			sf3500_voice_set_x(v, (float)srcX);
			sf3500_voice_set_gain(v, ((255.0f - vol) / 255.0f) * sfxVolume);
			sf3500_voice_play(v);
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
	int i, n;

	for (i = 0 ; i < SND_MAX ; i++)
	{
		for (n = 0 ; n < SND_INSTANCES ; n++)
		{
			if (soundInstances[i][n])
			{
				sf3500_voice_destroy(soundInstances[i][n]);
			}
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
