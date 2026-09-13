#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>

#include "SDL.h"
#include "SDL_thread.h"
#include <AL/al.h>
#include <AL/alc.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include "fixer.h"
#include "win95/cd_player.h"
#include "cdplayer.h"
#include "files.h"
#include "avp_ffmpeg.h"

extern int SoundSys_IsOn(void);
extern float PlatVolumeToGain(int volume);
extern ALvoid *GetAvpSoundContext(void);
extern void db_logf_fired(const char *fmtStrP, ...);

int CDPlayerVolume = 127;

#define NUM_CDDA_BUFFERS 4
#define CDDA_BUFFER_SAMPLES 4096

static const char *known_gold_tracks[] = {
	"",
	"01 marine music 1",
	"02 colony",
	"03 invasion",
	"04 orbital",
	"05 tyrargo",
	"06 waterfall",
	"07 area 52",
	"08 vaults",
	"09 fury 161",
	"10 caverns",
	"11 ferarco",
	"12 temple",
	"13 gateway",
	"14 escape",
	"15 earthbound"
};

static int cdda_initialized = 0;
static int cdda_active = 1;
static SDL_Thread *cdda_thread = NULL;
static SDL_mutex *cdda_mutex = NULL;
static SDL_cond *cdda_cond = NULL;
static int cdda_thread_exit = 0;

static int cdda_req_track = -1;
static int cdda_req_loop = 0;
static int cdda_stop_flag = 0;
static int cdda_is_playing = 0;

static char s_track_paths[16][PATH_MAX];
static int s_tracks_scanned = 0;
static int s_tracks_available = 0;

static void ScanCDTracks(void)
{
	if (s_tracks_scanned) return;
	s_tracks_scanned = 1;
	s_tracks_available = 0;
	memset(s_track_paths, 0, sizeof(s_track_paths));

	const char *base_dirs[6];
	int num_bases = 0;
	const char *gdir = GetGameGlobalDir();
	const char *ldir = GetGameLocalDir();
	if (gdir && gdir[0]) base_dirs[num_bases++] = gdir;
	if (ldir && ldir[0] && (!gdir || strcmp(gdir, ldir) != 0)) base_dirs[num_bases++] = ldir;
	base_dirs[num_bases++] = "sdmc:/switch/avpgold";
	base_dirs[num_bases++] = "sdmc:/switch/avp_gold";
	base_dirs[num_bases++] = "romfs:";
	base_dirs[num_bases++] = ".";

	const char *subdirs[] = {
		"FMVs", "fmvs", "movies", "Movies", "music", "Music",
		"cdtracks", "CDTracks", "cdda", "sound", "sound/music", "sound/cdtracks", ""
	};
	int num_subdirs = sizeof(subdirs) / sizeof(subdirs[0]);

	for (int b = 0; b < num_bases; b++)
	{
		for (int s = 0; s < num_subdirs; s++)
		{
			char dir_path[PATH_MAX];
			if (subdirs[s][0] != 0)
				snprintf(dir_path, sizeof(dir_path), "%s/%s", base_dirs[b], subdirs[s]);
			else
				snprintf(dir_path, sizeof(dir_path), "%s", base_dirs[b]);

			DIR *d = opendir(dir_path);
			if (!d) continue;

			struct dirent *de;
			while ((de = readdir(d)) != NULL)
			{
				if (de->d_name[0] == '.') continue;

				char entry_base[128];
				strncpy(entry_base, de->d_name, sizeof(entry_base) - 1);
				entry_base[sizeof(entry_base) - 1] = 0;
				char *entry_dot = strrchr(entry_base, '.');
				const char *ext = entry_dot ? entry_dot : "";
				if (entry_dot) *entry_dot = 0;

				int valid_ext = (strcasecmp(ext, ".bik") == 0 || strcasecmp(ext, ".ogg") == 0 ||
				                 strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0 ||
				                 strcasecmp(ext, ".flac") == 0);
				if (!valid_ext) continue;

				for (int track = 1; track <= 15; track++)
				{
					if (s_track_paths[track][0] != 0) continue;

					char num_str2[16];
					char track_str1[32];
					char track_str2[32];
					char track_str3[32];
					snprintf(num_str2, sizeof(num_str2), "%02d", track);
					snprintf(track_str1, sizeof(track_str1), "track%02d", track);
					snprintf(track_str2, sizeof(track_str2), "track_%02d", track);
					snprintf(track_str3, sizeof(track_str3), "track%d", track);

					const char *known_title = (track >= 1 && track <= 15) ? known_gold_tracks[track] : NULL;
					int matched = 0;

					if (known_title && strcasecmp(entry_base, known_title) == 0)
						matched = 1;

					if (!matched && (strcasecmp(entry_base, track_str1) == 0 ||
					                 strcasecmp(entry_base, track_str2) == 0 ||
					                 strcasecmp(entry_base, track_str3) == 0))
						matched = 1;

					if (!matched && strncmp(entry_base, num_str2, 2) == 0)
					{
						char next_char = entry_base[2];
						if (next_char == 0 || next_char == ' ' || next_char == '_' ||
						    next_char == '-' || next_char == '.')
						{
							matched = 1;
						}
					}

					if (matched)
					{
						char full_file[PATH_MAX];
						snprintf(full_file, sizeof(full_file), "%s/%s", dir_path, de->d_name);
						struct stat st;
						if (stat(full_file, &st) == 0)
						{
							snprintf(s_track_paths[track], sizeof(s_track_paths[track]), "%s", full_file);
							s_tracks_available++;
							db_logf_fired("CDDA: found track %d at '%s'\n", track, full_file);
						}
					}
				}
			}
			closedir(d);
		}
	}

	db_logf_fired("CDDA: Scan complete, %d tracks found\n", s_tracks_available);
	if (s_tracks_available == 0)
	{
		cdda_active = 0;
	}
}

static int FindCDTrackFilePath(int track, char *out_path, size_t max_len)
{
	if (track < 1 || track > 15 || !out_path || max_len == 0)
		return 0;

	if (!s_tracks_scanned)
		ScanCDTracks();

	if (s_track_paths[track][0] != 0)
	{
		strncpy(out_path, s_track_paths[track], max_len - 1);
		out_path[max_len - 1] = 0;
		return 1;
	}

	return 0;
}

static int CDDA_ThreadFunc(void *data)
{
	(void)data;

	// Wait for sound system to be active
	while (!cdda_thread_exit && !SoundSys_IsOn())
	{
		SDL_Delay(50);
	}
	if (cdda_thread_exit) return 0;

	ALCcontext *ctx = (ALCcontext *)GetAvpSoundContext();
	if (ctx)
	{
		alcMakeContextCurrent(ctx);
	}

	ALuint alSource = 0;
	ALuint alBuffers[NUM_CDDA_BUFFERS];
	memset(alBuffers, 0, sizeof(alBuffers));

	alGenSources(1, &alSource);
	alGenBuffers(NUM_CDDA_BUFFERS, alBuffers);

	alSource3f(alSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSource3f(alSource, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
	alSource3f(alSource, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
	alSourcef(alSource, AL_ROLLOFF_FACTOR, 0.0f);
	alSourcei(alSource, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcef(alSource, AL_PITCH, 1.0f);
	alSourcef(alSource, AL_GAIN, PlatVolumeToGain(CDPlayerVolume));

	while (!cdda_thread_exit)
	{
		SDL_LockMutex(cdda_mutex);
		while (!cdda_thread_exit && cdda_req_track <= 0)
		{
			SDL_CondWait(cdda_cond, cdda_mutex);
		}

		if (cdda_thread_exit)
		{
			SDL_UnlockMutex(cdda_mutex);
			break;
		}

		int track = cdda_req_track;
		int loop = cdda_req_loop;
		cdda_req_track = -1;
		cdda_stop_flag = 0;
		cdda_is_playing = 1;
		SDL_UnlockMutex(cdda_mutex);

		char track_path[PATH_MAX];
		if (!FindCDTrackFilePath(track, track_path, sizeof(track_path)))
		{
			cdda_is_playing = 0;
			continue;
		}

		db_logf_fired("CDDA: Playing track %d from '%s' (loop=%d)\n", track, track_path, loop);

		AvpAvioContext avio_state;
		AVFormatContext *fmt_ctx = AvpOpenMediaFile(track_path, &avio_state);
		if (!fmt_ctx)
		{
			db_logf_fired("CDDA: Failed to open '%s'\n", track_path);
			cdda_is_playing = 0;
			continue;
		}

		if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
		{
			db_logf_fired("CDDA: avformat_find_stream_info failed for '%s'\n", track_path);
			AvpCloseMediaFile(&avio_state);
			cdda_is_playing = 0;
			continue;
		}

		int a_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		if (a_idx < 0)
		{
			db_logf_fired("CDDA: No audio stream in '%s'\n", track_path);
			AvpCloseMediaFile(&avio_state);
			cdda_is_playing = 0;
			continue;
		}

		AVStream *a_stream = fmt_ctx->streams[a_idx];
		const AVCodec *a_codec = avcodec_find_decoder(a_stream->codecpar->codec_id);
		if (!a_codec)
		{
			db_logf_fired("CDDA: Unsupported audio codec in '%s'\n", track_path);
			AvpCloseMediaFile(&avio_state);
			cdda_is_playing = 0;
			continue;
		}

		AVCodecContext *a_ctx = avcodec_alloc_context3(a_codec);
		if (!a_ctx || avcodec_parameters_to_context(a_ctx, a_stream->codecpar) < 0 ||
		    avcodec_open2(a_ctx, a_codec, NULL) < 0)
		{
			db_logf_fired("CDDA: Could not open audio decoder\n");
			if (a_ctx) avcodec_free_context(&a_ctx);
			avformat_close_input(&fmt_ctx);
			cdda_is_playing = 0;
			continue;
		}

		SwrContext *swr_ctx = NULL;
		AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
		swr_alloc_set_opts2(&swr_ctx, &out_layout, AV_SAMPLE_FMT_S16, 44100,
		                    &a_ctx->ch_layout, a_ctx->sample_fmt, a_ctx->sample_rate,
		                    0, NULL);
		if (!swr_ctx || swr_init(swr_ctx) < 0)
		{
			db_logf_fired("CDDA: Could not init audio resampler\n");
			if (swr_ctx) swr_free(&swr_ctx);
			avcodec_free_context(&a_ctx);
			avformat_close_input(&fmt_ctx);
			cdda_is_playing = 0;
			continue;
		}

		// Clear source buffers
		alSourceStop(alSource);
		ALint q = 0;
		alGetSourcei(alSource, AL_BUFFERS_QUEUED, &q);
		while (q > 0)
		{
			ALuint u = 0;
			alSourceUnqueueBuffers(alSource, 1, &u);
			q--;
		}

		ALuint free_slots[NUM_CDDA_BUFFERS];
		int num_free = NUM_CDDA_BUFFERS;
		for (int b = 0; b < NUM_CDDA_BUFFERS; b++)
		{
			free_slots[b] = alBuffers[b];
		}

		AVPacket *pkt = av_packet_alloc();
		AVFrame *a_frame = av_frame_alloc();
		size_t pcm_buf_capacity = 16384 * 2 * sizeof(int16_t);
		uint8_t *pcm_buf = (uint8_t *)malloc(pcm_buf_capacity);

		int eof_reached = 0;

		while (!cdda_thread_exit)
		{
			if (cdda_stop_flag || cdda_req_track > 0)
			{
				break;
			}

			// Update volume in real time
			alSourcef(alSource, AL_GAIN, PlatVolumeToGain(CDPlayerVolume));

			// Reclaim processed buffers
			ALint processed = 0;
			alGetSourcei(alSource, AL_BUFFERS_PROCESSED, &processed);
			while (processed > 0 && num_free < NUM_CDDA_BUFFERS)
			{
				ALuint u = 0;
				alSourceUnqueueBuffers(alSource, 1, &u);
				free_slots[num_free++] = u;
				processed--;
			}

			// Decode audio samples if we have free buffer slots and haven't hit EOF
			if (num_free > 0 && !eof_reached)
			{
				int target_samples = CDDA_BUFFER_SAMPLES;
				int total_samples_converted = 0;

				while (total_samples_converted < target_samples && !eof_reached)
				{
					int ret = av_read_frame(fmt_ctx, pkt);
					if (ret < 0)
					{
						if (loop)
						{
							av_seek_frame(fmt_ctx, a_idx, 0, AVSEEK_FLAG_BACKWARD);
							avcodec_flush_buffers(a_ctx);
						}
						else
						{
							eof_reached = 1;
						}
						break;
					}

					if (pkt->stream_index == a_idx)
					{
						if (avcodec_send_packet(a_ctx, pkt) >= 0)
						{
							while (avcodec_receive_frame(a_ctx, a_frame) == 0)
							{
								int out_samples = swr_get_out_samples(swr_ctx, a_frame->nb_samples);
								if (out_samples > 0)
								{
									size_t needed = ((size_t)total_samples_converted + out_samples) * 2 * sizeof(int16_t);
									if (needed > pcm_buf_capacity)
									{
										pcm_buf_capacity = needed * 2;
										pcm_buf = (uint8_t *)realloc(pcm_buf, pcm_buf_capacity);
									}

									uint8_t *out_ptr = pcm_buf + (total_samples_converted * 2 * sizeof(int16_t));
									int converted = swr_convert(swr_ctx, &out_ptr, out_samples,
									                            (const uint8_t **)a_frame->extended_data,
									                            a_frame->nb_samples);
									if (converted > 0)
									{
										total_samples_converted += converted;
									}
								}
								av_frame_unref(a_frame);
							}
						}
					}
					av_packet_unref(pkt);
				}

				if (total_samples_converted > 0 && num_free > 0)
				{
					ALuint bid = free_slots[--num_free];
					ALsizei bytes = total_samples_converted * 2 * (ALsizei)sizeof(int16_t);
					alBufferData(bid, AL_FORMAT_STEREO16, pcm_buf, bytes, 44100);
					alSourceQueueBuffers(alSource, 1, &bid);

					ALint st = 0;
					alGetSourcei(alSource, AL_SOURCE_STATE, &st);
					if (st != AL_PLAYING)
					{
						alSourcePlay(alSource);
					}
				}
			}

			// If EOF reached and non-looping, check if playback has drained
			if (eof_reached)
			{
				ALint queued = 0;
				alGetSourcei(alSource, AL_BUFFERS_QUEUED, &queued);
				ALint st = 0;
				alGetSourcei(alSource, AL_SOURCE_STATE, &st);
				if (queued == 0 || st != AL_PLAYING)
				{
					db_logf_fired("CDDA: Track %d finished playing\n", track);
					break;
				}
			}

			SDL_Delay(15);
		}

		// Stop source and clean up OpenAL queues
		alSourceStop(alSource);
		q = 0;
		alGetSourcei(alSource, AL_BUFFERS_QUEUED, &q);
		while (q > 0)
		{
			ALuint u = 0;
			alSourceUnqueueBuffers(alSource, 1, &u);
			q--;
		}

		free(pcm_buf);
		av_frame_free(&a_frame);
		av_packet_free(&pkt);
		swr_free(&swr_ctx);
		avcodec_free_context(&a_ctx);
		AvpCloseMediaFile(&avio_state);

		if (!cdda_stop_flag && cdda_req_track <= 0)
		{
			cdda_is_playing = 0;
		}
		else if (cdda_stop_flag)
		{
			cdda_is_playing = 0;
			cdda_stop_flag = 0;
		}
	}

	alSourceStop(alSource);
	alDeleteSources(1, &alSource);
	alDeleteBuffers(NUM_CDDA_BUFFERS, alBuffers);

	return 0;
}

void CDDA_Start(void)
{
	if (cdda_initialized) return;

	cdda_active = 1;
	cdda_thread_exit = 0;
	cdda_req_track = -1;
	cdda_stop_flag = 0;
	cdda_is_playing = 0;

	cdda_mutex = SDL_CreateMutex();
	cdda_cond = SDL_CreateCond();

	cdda_thread = SDL_CreateThread(CDDA_ThreadFunc, "CDDA_Audio", NULL);
	cdda_initialized = 1;

	db_logf_fired("CDDA_Start: initialized CD audio player thread\n");
}

void CDDA_End(void)
{
	if (!cdda_initialized) return;

	CDDA_Stop();

	if (cdda_thread)
	{
		SDL_LockMutex(cdda_mutex);
		cdda_thread_exit = 1;
		SDL_CondSignal(cdda_cond);
		SDL_UnlockMutex(cdda_mutex);

		SDL_WaitThread(cdda_thread, NULL);
		cdda_thread = NULL;
	}

	if (cdda_cond)
	{
		SDL_DestroyCond(cdda_cond);
		cdda_cond = NULL;
	}
	if (cdda_mutex)
	{
		SDL_DestroyMutex(cdda_mutex);
		cdda_mutex = NULL;
	}

	cdda_initialized = 0;
	db_logf_fired("CDDA_End: shut down CD audio player\n");
}

void CDDA_Play(int CDDATrack)
{
	if (!cdda_initialized) CDDA_Start();
	if (!cdda_active || CDDATrack <= 0) return;

	db_logf_fired("CDDA_Play: requested track %d\n", CDDATrack);

	SDL_LockMutex(cdda_mutex);
	cdda_req_track = CDDATrack;
	cdda_req_loop = 0;
	cdda_stop_flag = 0;
	cdda_is_playing = 1;
	SDL_CondSignal(cdda_cond);
	SDL_UnlockMutex(cdda_mutex);
}

void CDDA_PlayLoop(int CDDATrack)
{
	if (!cdda_initialized) CDDA_Start();
	if (!cdda_active || CDDATrack <= 0) return;

	db_logf_fired("CDDA_PlayLoop: requested loop track %d\n", CDDATrack);

	SDL_LockMutex(cdda_mutex);
	cdda_req_track = CDDATrack;
	cdda_req_loop = 1;
	cdda_stop_flag = 0;
	cdda_is_playing = 1;
	SDL_CondSignal(cdda_cond);
	SDL_UnlockMutex(cdda_mutex);
}

void CDDA_Stop(void)
{
	if (!cdda_initialized) return;

	SDL_LockMutex(cdda_mutex);
	cdda_stop_flag = 1;
	cdda_req_track = -1;
	cdda_is_playing = 0;
	SDL_CondSignal(cdda_cond);
	SDL_UnlockMutex(cdda_mutex);
}

int CDDA_IsPlaying(void)
{
	return (cdda_initialized && cdda_active && cdda_is_playing);
}

int CDDA_IsOn(void)
{
	if (!s_tracks_scanned) ScanCDTracks();
	return (cdda_initialized && cdda_active && s_tracks_available > 0);
}

int CDDA_CheckNumberOfTracks(void)
{
	if (!s_tracks_scanned) ScanCDTracks();
	return s_tracks_available;
}

void CDDA_ChangeVolume(int volume)
{
	if (volume < 0) volume = 0;
	if (volume > 127) volume = 127;
	CDPlayerVolume = volume;
}

int CDDA_GetCurrentVolumeSetting(void)
{
	return CDPlayerVolume;
}

void CDDA_SwitchOn(void)
{
	cdda_active = 1;
}

void CDDA_SwitchOff(void)
{
	cdda_active = 0;
	CDDA_Stop();
}

void CDDA_Management(void)
{
}

void CheckCDVolume(void)
{
}
