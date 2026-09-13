#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "SDL.h"
#include <AL/al.h>
#include <AL/alc.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

#include "3dc.h"
#include "bink.h"
#include "files.h"
#include "avp_ffmpeg.h"

extern int SoundSys_IsOn(void);
extern float PlatVolumeToGain(int volume);
extern ALvoid *GetAvpSoundContext(void);
extern void DrawAvpMenuBink(unsigned char* buf, int width, int height, int pitch);
extern void FlipBuffers(void);
extern void ClearScreenToBlack(void);
extern void CheckForWindowsMessages(void);
extern void DirectReadKeyboard(void);
extern void db_logf_fired(const char *fmtStrP, ...);

extern unsigned char GotAnyKey;
extern int DebouncedGotAnyKey;
extern unsigned char KeyboardInput[];
extern unsigned char DebouncedKeyboardInput[];

#define NUM_AUDIO_BUFFERS 16

static BOOL binkInitialized = FALSE;

BOOL BinkSys_Init(void)
{
	binkInitialized = TRUE;
	return TRUE;
}

void BinkSys_Release(void)
{
	binkInitialized = FALSE;
}

static void ClearAllInputState(void)
{
	GotAnyKey = 0;
	DebouncedGotAnyKey = 0;
	KeyboardInput[KEY_ESCAPE] = 0;
	DebouncedKeyboardInput[KEY_ESCAPE] = 0;
	KeyboardInput[KEY_CR] = 0;
	DebouncedKeyboardInput[KEY_CR] = 0;
	KeyboardInput[KEY_SPACE] = 0;
	DebouncedKeyboardInput[KEY_SPACE] = 0;
	for (int i = 0; i < 16; i++)
	{
		KeyboardInput[KEY_JOYSTICK_BUTTON_1 + i] = 0;
		DebouncedKeyboardInput[KEY_JOYSTICK_BUTTON_1 + i] = 0;
	}
}

static int CheckForSkip(uint32_t start_ticks, int *button_released)
{
	CheckForWindowsMessages();
	DirectReadKeyboard();

	uint32_t elapsed = SDL_GetTicks() - start_ticks;

	int escape_down = KeyboardInput[KEY_ESCAPE];
	int cr_down = KeyboardInput[KEY_CR];
	int space_down = KeyboardInput[KEY_SPACE];
	int btn_down = (KeyboardInput[KEY_JOYSTICK_BUTTON_1] || KeyboardInput[KEY_JOYSTICK_BUTTON_2] ||
	                KeyboardInput[KEY_JOYSTICK_BUTTON_10] || KeyboardInput[KEY_JOYSTICK_BUTTON_11]);

	if (!escape_down && !cr_down && !space_down && !btn_down)
	{
		*button_released = 1;
	}

	// 800ms grace period to avoid instant skip from launch button press
	if (elapsed < 800)
	{
		return 0;
	}

	if (*button_released)
	{
		if (DebouncedKeyboardInput[KEY_ESCAPE] || DebouncedKeyboardInput[KEY_CR] ||
		    DebouncedKeyboardInput[KEY_SPACE] || DebouncedKeyboardInput[KEY_JOYSTICK_BUTTON_1] ||
		    DebouncedKeyboardInput[KEY_JOYSTICK_BUTTON_2] || DebouncedKeyboardInput[KEY_JOYSTICK_BUTTON_11] ||
		    escape_down)
		{
			return 1;
		}
	}

	return 0;
}

static int FindMovieFilePath(const char *filename, char *out_path, size_t max_len)
{
	struct stat st;

	if (!filename || !filename[0] || !out_path || max_len == 0)
		return 0;

	if (FindGameFilePath(filename, out_path, max_len))
	{
		if (stat(out_path, &st) == 0)
		{
			db_logf_fired("FindMovieFilePath: FindGameFilePath found '%s' -> '%s'\n", filename, out_path);
			return 1;
		}
	}

	const char *slash = strrchr(filename, '/');
	const char *bslash = strrchr(filename, '\\');
	const char *fname = filename;
	if (slash && slash + 1 > fname) fname = slash + 1;
	if (bslash && bslash + 1 > fname) fname = bslash + 1;

	char base_name[128];
	strncpy(base_name, fname, sizeof(base_name) - 1);
	base_name[sizeof(base_name) - 1] = 0;
	char *dot = strrchr(base_name, '.');
	if (dot) *dot = 0;

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
		"FMVs", "fmvs", "movies", "Movies", "FMV", "fmv", ""
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

				if (strcasecmp(entry_base, base_name) == 0)
				{
					if (strcasecmp(ext, ".bik") == 0 || strcasecmp(ext, ".smk") == 0 ||
					    strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".mkv") == 0)
					{
						snprintf(out_path, max_len, "%s/%s", dir_path, de->d_name);
						if (stat(out_path, &st) == 0)
						{
							closedir(d);
							db_logf_fired("FindMovieFilePath: found '%s' at '%s'\n", filename, out_path);
							return 1;
						}
					}
				}
			}
			closedir(d);
		}
	}

	db_logf_fired("FindMovieFilePath: could not locate '%s' (base '%s')\n", filename, base_name);
	return 0;
}

void PlayBinkedFMV(char *filenamePtr, int volume)
{
	if (!binkInitialized)
	{
		BinkSys_Init();
	}

	if (!filenamePtr || !filenamePtr[0])
		return;

	ALCcontext *alc_ctx = (ALCcontext *)GetAvpSoundContext();
	if (alc_ctx)
	{
		alcMakeContextCurrent(alc_ctx);
	}

	db_logf_fired("PlayBinkedFMV: requested '%s'\n", filenamePtr);

	char resolvedPath[PATH_MAX];
	if (!FindMovieFilePath(filenamePtr, resolvedPath, sizeof(resolvedPath)))
	{
		db_logf_fired("PlayBinkedFMV: Unable to locate movie file '%s'\n", filenamePtr);
		return;
	}

	db_logf_fired("PlayBinkedFMV: Playing '%s' (resolved path: '%s')\n", filenamePtr, resolvedPath);

	AvpAvioContext avio_state;
	AVFormatContext *fmt_ctx = AvpOpenMediaFile(resolvedPath, &avio_state);
	if (!fmt_ctx)
	{
		db_logf_fired("PlayBinkedFMV: AvpOpenMediaFile failed for '%s'\n", resolvedPath);
		return;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
	{
		db_logf_fired("PlayBinkedFMV: avformat_find_stream_info failed\n");
		AvpCloseMediaFile(&avio_state);
		return;
	}

	int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

	if (video_idx < 0 && audio_idx < 0)
	{
		db_logf_fired("PlayBinkedFMV: No valid video or audio stream in '%s'\n", resolvedPath);
		AvpCloseMediaFile(&avio_state);
		return;
	}


	AVCodecContext *v_ctx = NULL;
	AVCodecContext *a_ctx = NULL;
	struct SwsContext *sws_ctx = NULL;
	SwrContext *swr_ctx = NULL;
	AVFrame *scaled_frame = NULL;

	double fps = 30.0;
	double frame_duration_ms = 1000.0 / 30.0;
	int dst_w = 640;
	int dst_h = 480;

	if (video_idx >= 0)
	{
		AVStream *v_stream = fmt_ctx->streams[video_idx];
		const AVCodec *v_codec = avcodec_find_decoder(v_stream->codecpar->codec_id);
		if (v_codec)
		{
			v_ctx = avcodec_alloc_context3(v_codec);
			if (v_ctx && avcodec_parameters_to_context(v_ctx, v_stream->codecpar) >= 0 &&
			    avcodec_open2(v_ctx, v_codec, NULL) >= 0)
			{
				if (v_stream->avg_frame_rate.den > 0 && v_stream->avg_frame_rate.num > 0)
					fps = av_q2d(v_stream->avg_frame_rate);
				else if (v_stream->r_frame_rate.den > 0 && v_stream->r_frame_rate.num > 0)
					fps = av_q2d(v_stream->r_frame_rate);

				if (fps < 1.0 || fps > 120.0) fps = 30.0;
				frame_duration_ms = 1000.0 / fps;

				dst_w = v_ctx->width;
				dst_h = v_ctx->height;

				// Scale to fit inside 640x480 virtual frame maintaining aspect ratio
				if (dst_w > 640 || dst_h > 480)
				{
					float aspect = (float)v_ctx->width / (float)v_ctx->height;
					if (dst_w > 640)
					{
						dst_w = 640;
						dst_h = (int)(640.0f / aspect);
					}
					if (dst_h > 480)
					{
						dst_h = 480;
						dst_w = (int)(480.0f * aspect);
					}
				}

				sws_ctx = sws_getContext(
					v_ctx->width, v_ctx->height, v_ctx->pix_fmt,
					dst_w, dst_h, AV_PIX_FMT_RGB565LE,
					SWS_FAST_BILINEAR, NULL, NULL, NULL);

				if (sws_ctx)
				{
					scaled_frame = av_frame_alloc();
					scaled_frame->format = AV_PIX_FMT_RGB565LE;
					scaled_frame->width = dst_w;
					scaled_frame->height = dst_h;
					av_frame_get_buffer(scaled_frame, 0);
				}
			}
		}
	}

	// Audio setup with OpenAL
	ALuint alSource = 0;
	ALuint alBuffers[NUM_AUDIO_BUFFERS];
	ALuint allBuffers[NUM_AUDIO_BUFFERS];
	uint8_t *resample_buf = NULL;
	size_t resample_buf_size = 0;
	bool has_audio = false;

	if (audio_idx >= 0 && SoundSys_IsOn())
	{
		AVStream *a_stream = fmt_ctx->streams[audio_idx];
		const AVCodec *a_codec = avcodec_find_decoder(a_stream->codecpar->codec_id);
		if (a_codec)
		{
			a_ctx = avcodec_alloc_context3(a_codec);
			if (a_ctx && avcodec_parameters_to_context(a_ctx, a_stream->codecpar) >= 0 &&
			    avcodec_open2(a_ctx, a_codec, NULL) >= 0)
			{
				AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
				int swr_ret = swr_alloc_set_opts2(&swr_ctx, &out_layout, AV_SAMPLE_FMT_S16, 44100,
				                                  &a_ctx->ch_layout, a_ctx->sample_fmt, a_ctx->sample_rate,
				                                  0, NULL);
				if (swr_ret >= 0 && swr_ctx && swr_init(swr_ctx) >= 0)
				{
					alGenSources(1, &alSource);
					alGenBuffers(NUM_AUDIO_BUFFERS, alBuffers);
					memcpy(allBuffers, alBuffers, sizeof(alBuffers));

					alSource3f(alSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
					alSource3f(alSource, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
					alSource3f(alSource, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
					alSourcef(alSource, AL_ROLLOFF_FACTOR, 0.0f);
					alSourcei(alSource, AL_SOURCE_RELATIVE, AL_TRUE);
					alSourcef(alSource, AL_PITCH, 1.0f);
					alSourcef(alSource, AL_GAIN, PlatVolumeToGain(volume));

					resample_buf_size = 4096 * 2 * sizeof(int16_t);
					resample_buf = (uint8_t *)malloc(resample_buf_size);
					has_audio = true;
				}
			}
		}
	}

	// Prepare black screen for letterboxed video
	ClearScreenToBlack();
	FlipBuffers();
	ClearScreenToBlack();
	FlipBuffers();

	ClearAllInputState();

	AVPacket *pkt = av_packet_alloc();
	AVFrame *v_frame = av_frame_alloc();
	AVFrame *a_frame = av_frame_alloc();

	int skip_requested = 0;
	int frame_count = 0;
	uint32_t start_ticks = SDL_GetTicks();
	int button_released = 0;

	while (!skip_requested && av_read_frame(fmt_ctx, pkt) >= 0)
	{
		if (CheckForSkip(start_ticks, &button_released))
		{
			db_logf_fired("PlayBinkedFMV: skip requested by user\n");
			skip_requested = 1;
			av_packet_unref(pkt);
			break;
		}


		if (pkt->stream_index == audio_idx && has_audio && a_ctx && swr_ctx)
		{
			if (avcodec_send_packet(a_ctx, pkt) >= 0)
			{
				while (avcodec_receive_frame(a_ctx, a_frame) == 0)
				{
					// Reclaim finished OpenAL buffers
					ALint processed = 0;
					alGetSourcei(alSource, AL_BUFFERS_PROCESSED, &processed);
					while (processed > 0)
					{
						ALuint unq = 0;
						alSourceUnqueueBuffers(alSource, 1, &unq);
						for (int b = 0; b < NUM_AUDIO_BUFFERS; b++)
						{
							if (alBuffers[b] == 0)
							{
								alBuffers[b] = unq;
								break;
							}
						}
						processed--;
					}

					// Find a free buffer slot
					int free_slot = -1;
					for (int b = 0; b < NUM_AUDIO_BUFFERS; b++)
					{
						if (alBuffers[b] != 0)
						{
							free_slot = b;
							break;
						}
					}

					if (free_slot < 0)
					{
						for (int r = 0; r < 20 && free_slot < 0; r++)
						{
							SDL_Delay(5);
							alGetSourcei(alSource, AL_BUFFERS_PROCESSED, &processed);
							if (processed > 0)
							{
								ALuint unq = 0;
								alSourceUnqueueBuffers(alSource, 1, &unq);
								alBuffers[0] = unq;
								free_slot = 0;
								break;
							}
						}
					}

					if (free_slot >= 0)
					{
						int out_samples = swr_get_out_samples(swr_ctx, a_frame->nb_samples);
						if (out_samples > 0)
						{
							size_t needed = (size_t)out_samples * 2 * sizeof(int16_t);
							if (needed > resample_buf_size)
							{
								resample_buf = (uint8_t *)realloc(resample_buf, needed);
								resample_buf_size = needed;
							}

							int converted = swr_convert(swr_ctx, &resample_buf, out_samples,
							                            (const uint8_t **)a_frame->extended_data,
							                            a_frame->nb_samples);
							if (converted > 0)
							{
								ALsizei bytes = converted * 2 * (ALsizei)sizeof(int16_t);
								ALuint buf_id = alBuffers[free_slot];
								alBuffers[free_slot] = 0;
								alBufferData(buf_id, AL_FORMAT_STEREO16, resample_buf, bytes, 44100);
								alSourceQueueBuffers(alSource, 1, &buf_id);

								ALint state = 0;
								alGetSourcei(alSource, AL_SOURCE_STATE, &state);
								if (state != AL_PLAYING)
								{
									alSourcePlay(alSource);
								}
							}
						}
					}
					av_frame_unref(a_frame);
				}
			}
		}
		else if (pkt->stream_index == video_idx && v_ctx && sws_ctx && scaled_frame)
		{
			if (avcodec_send_packet(v_ctx, pkt) >= 0)
			{
				while (avcodec_receive_frame(v_ctx, v_frame) == 0)
				{
					if (CheckForSkip(start_ticks, &button_released))
					{
						db_logf_fired("PlayBinkedFMV: skip requested by user\n");
						skip_requested = 1;
						av_frame_unref(v_frame);
						break;
					}

					sws_scale(sws_ctx, (const uint8_t * const *)v_frame->data, v_frame->linesize,
					          0, v_ctx->height, scaled_frame->data, scaled_frame->linesize);

					uint32_t target_ms = (uint32_t)(frame_count * frame_duration_ms);
					uint32_t now_ms = SDL_GetTicks() - start_ticks;
					if (target_ms > now_ms)
					{
						uint32_t wait_ms = target_ms - now_ms;
						if (wait_ms > 0 && wait_ms <= 100)
						{
							SDL_Delay(wait_ms);
						}
					}

					DrawAvpMenuBink(scaled_frame->data[0], dst_w, dst_h, scaled_frame->linesize[0]);
					FlipBuffers();
					frame_count++;

					av_frame_unref(v_frame);
				}
			}
		}

		av_packet_unref(pkt);
	}

	// Flush video decoder if video finished naturally
	if (!skip_requested && v_ctx && sws_ctx && scaled_frame)
	{
		avcodec_send_packet(v_ctx, NULL);
		while (avcodec_receive_frame(v_ctx, v_frame) == 0 && !skip_requested)
		{
			if (CheckForSkip(start_ticks, &button_released))
			{
				db_logf_fired("PlayBinkedFMV: skip requested by user\n");
				skip_requested = 1;
				av_frame_unref(v_frame);
				break;
			}

			sws_scale(sws_ctx, (const uint8_t * const *)v_frame->data, v_frame->linesize,
			          0, v_ctx->height, scaled_frame->data, scaled_frame->linesize);

			uint32_t target_ms = (uint32_t)(frame_count * frame_duration_ms);
			uint32_t now_ms = SDL_GetTicks() - start_ticks;
			if (target_ms > now_ms)
			{
				uint32_t wait_ms = target_ms - now_ms;
				if (wait_ms > 0 && wait_ms <= 100)
				{
					SDL_Delay(wait_ms);
				}
			}

			DrawAvpMenuBink(scaled_frame->data[0], dst_w, dst_h, scaled_frame->linesize[0]);
			FlipBuffers();
			frame_count++;

			av_frame_unref(v_frame);
		}
	}

	// Drain audio if not skipped
	if (!skip_requested && has_audio && alSource)
	{
		ALint state = 0;
		uint32_t drain_start = SDL_GetTicks();
		alGetSourcei(alSource, AL_SOURCE_STATE, &state);
		while (state == AL_PLAYING && (SDL_GetTicks() - drain_start < 1500))
		{
			if (CheckForSkip(start_ticks, &button_released)) break;
			SDL_Delay(20);
			alGetSourcei(alSource, AL_SOURCE_STATE, &state);
		}
	}

	// Cleanup Audio
	if (has_audio && alSource)
	{
		alSourceStop(alSource);
		ALint q = 0;
		alGetSourcei(alSource, AL_BUFFERS_QUEUED, &q);
		while (q > 0)
		{
			ALuint unq = 0;
			alSourceUnqueueBuffers(alSource, 1, &unq);
			q--;
		}
		alDeleteSources(1, &alSource);
		alDeleteBuffers(NUM_AUDIO_BUFFERS, allBuffers);
	}
	if (resample_buf)
	{
		free(resample_buf);
	}
	if (swr_ctx)
	{
		swr_free(&swr_ctx);
	}
	if (a_ctx)
	{
		avcodec_free_context(&a_ctx);
	}

	// Cleanup Video
	if (scaled_frame)
	{
		av_frame_free(&scaled_frame);
	}
	if (sws_ctx)
	{
		sws_freeContext(sws_ctx);
	}
	if (v_ctx)
	{
		avcodec_free_context(&v_ctx);
	}

	av_frame_free(&v_frame);
	av_frame_free(&a_frame);
	av_packet_free(&pkt);

	AvpCloseMediaFile(&avio_state);

	// Clear screen to black and clear input state
	ClearScreenToBlack();
	FlipBuffers();
	ClearScreenToBlack();
	FlipBuffers();

	ClearAllInputState();
	CheckForWindowsMessages();
	db_logf_fired("PlayBinkedFMV: completed '%s' (frames=%d, skipped=%d)\n", filenamePtr, frame_count, skip_requested);

}

void StartMenuBackgroundBink(void)
{
}

int PlayMenuBackgroundBink(void)
{
	return 0;
}

void EndMenuBackgroundBink(void)
{
}

int StartMusicBink(char* filenamePtr, BOOL looping)
{
	(void)filenamePtr;
	(void)looping;
	return 0;
}

int PlayMusicBink(int volume)
{
	(void)volume;
	return 0;
}

void EndMusicBink(void)
{
}

FMVHandle CreateBinkFMV(char* filenamePtr)
{
	(void)filenamePtr;
	return 0;
}

int UpdateBinkFMV(FMVHandle aFmvHandle, int volume)
{
	(void)aFmvHandle;
	(void)volume;
	return 0;
}

void CloseBinkFMV(FMVHandle aFmvHandle)
{
	(void)aFmvHandle;
}

char* GetBinkFMVImage(FMVHandle aFmvHandle)
{
	(void)aFmvHandle;
	return NULL;
}

