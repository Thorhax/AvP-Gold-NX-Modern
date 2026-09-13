/* KJL 15:25:20 8/16/97
 *
 * smacker.c - functions to handle FMV playback
 * Modernized for AvP Gold Nintendo Switch using FFmpeg and OpenAL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>

#include "SDL.h"
#include <AL/al.h>
#include <AL/alc.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#include "3dc.h"
#include "module.h"
#include "inline.h"
#include "stratdef.h"
#include "gamedef.h"
#include "fmv.h"
#include "avp_menus.h"
#include "avp_userprofile.h"
#include "oglfunc.h"
#include "bink.h"
#include "avp_ffmpeg.h"
#include "psnd.h"

#define UseLocalAssert 1
#include "ourasert.h"

int VolumeOfNearestVideoScreen;
int PanningOfNearestVideoScreen;

extern char *ScreenBuffer;
extern int GotAnyKey;
extern void DirectReadKeyboard(void);
extern IMAGEHEADER ImageHeaderArray[];
#if MaxImageGroups>1
extern int NumImagesArray[];
#else
extern int NumImages;
#endif

extern int SoundSys_IsOn(void);
extern ALvoid *GetAvpSoundContext(void);
extern void db_logf_fired(const char *fmtStrP, ...);

int SmackerSoundVolume = ONE_FIXED / 512;
int MoviesAreActive = 1;
int IntroOutroMoviesAreActive = 1;

int FmvColourRed = 0;
int FmvColourGreen = 0;
int FmvColourBlue = 0;

void ReleaseFMVTexture(FMVTEXTURE *ftPtr);
void SetupFMVTexture(FMVTEXTURE *ftPtr);
void UpdateFMVTexture(FMVTEXTURE *ftPtr);
void FindLightingValuesFromTriggeredFMV(unsigned char *bufferPtr, FMVTEXTURE *ftPtr);

#define MAX_NO_FMVTEXTURES 10
FMVTEXTURE FMVTexture[MAX_NO_FMVTEXTURES];
int NumberOfFMVTextures = 0;

#define PLOT_NUM_AUDIO_BUFFERS 32
#define PLOT_AUDIO_SAMPLE_RATE 22050

static int PlotFMVActive = 0;
static int PlotFMVNumber = 0;
static uint32_t PlotNextFrameTicks = 0;
static uint32_t PlotFrameDurationMs = 66;
static int PlotEofDrainFrames = 0;

static AvpAvioContext PlotAvioState;
static AVFormatContext *PlotFmtCtx = NULL;
static AVCodecContext *PlotVideoCodecCtx = NULL;
static AVCodecContext *PlotAudioCodecCtx = NULL;
static struct SwsContext *PlotSwsCtx = NULL;
static SwrContext *PlotSwrCtx = NULL;

static AVPacket *PlotPacket = NULL;
static AVFrame *PlotVideoFrame = NULL;
static AVFrame *PlotAudioFrame = NULL;

static int PlotVideoIdx = -1;
static int PlotAudioIdx = -1;

static ALuint PlotAlSource = 0;
static ALuint PlotAlBuffers[PLOT_NUM_AUDIO_BUFFERS];
static ALuint PlotAllBuffers[PLOT_NUM_AUDIO_BUFFERS];

static uint8_t PlotRGBBuf[128 * 128 * 4];

static void StopPlotFMV(void)
{
	if (!PlotFMVActive && !PlotFmtCtx)
		return;

	PlotFMVActive = 0;
	PlotFMVNumber = 0;
	PlotEofDrainFrames = 0;

	if (PlotAlSource)
	{
		ALCcontext *alc_ctx = (ALCcontext *)GetAvpSoundContext();
		if (alc_ctx && alcGetCurrentContext() != alc_ctx)
		{
			alcMakeContextCurrent(alc_ctx);
		}

		alSourceStop(PlotAlSource);
		ALint q = 0;
		alGetSourcei(PlotAlSource, AL_BUFFERS_QUEUED, &q);
		while (q > 0)
		{
			ALuint unq = 0;
			alSourceUnqueueBuffers(PlotAlSource, 1, &unq);
			q--;
		}
		alSourcei(PlotAlSource, AL_BUFFER, 0);
		alDeleteSources(1, &PlotAlSource);
		PlotAlSource = 0;
	}

	if (PlotAllBuffers[0] != 0)
	{
		alDeleteBuffers(PLOT_NUM_AUDIO_BUFFERS, PlotAllBuffers);
		memset(PlotAlBuffers, 0, sizeof(PlotAlBuffers));
		memset(PlotAllBuffers, 0, sizeof(PlotAllBuffers));
	}

	if (PlotSwrCtx)
	{
		swr_free(&PlotSwrCtx);
		PlotSwrCtx = NULL;
	}

	if (PlotAudioCodecCtx)
	{
		avcodec_free_context(&PlotAudioCodecCtx);
		PlotAudioCodecCtx = NULL;
	}

	if (PlotSwsCtx)
	{
		sws_freeContext(PlotSwsCtx);
		PlotSwsCtx = NULL;
	}

	if (PlotVideoCodecCtx)
	{
		avcodec_free_context(&PlotVideoCodecCtx);
		PlotVideoCodecCtx = NULL;
	}

	if (PlotVideoFrame)
	{
		av_frame_free(&PlotVideoFrame);
		PlotVideoFrame = NULL;
	}

	if (PlotAudioFrame)
	{
		av_frame_free(&PlotAudioFrame);
		PlotAudioFrame = NULL;
	}

	if (PlotPacket)
	{
		av_packet_free(&PlotPacket);
		PlotPacket = NULL;
	}

	if (PlotFmtCtx)
	{
		AvpCloseMediaFile(&PlotAvioState);
		PlotFmtCtx = NULL;
	}

	PlotVideoIdx = -1;
	PlotAudioIdx = -1;

	FmvColourRed = 0;
	FmvColourGreen = 0;
	FmvColourBlue = 0;
}

void ScanImagesForFMVs(void)
{
	int i;
	IMAGEHEADER *ihPtr;
	NumberOfFMVTextures = 0;

#if MaxImageGroups > 1
	for (int j = 0; j < MaxImageGroups; j++)
	{
		if (NumImagesArray[j])
		{
			ihPtr = &ImageHeaderArray[j * MaxImages];
			for (i = 0; i < NumImagesArray[j]; i++, ihPtr++)
			{
#else
	{
		if (NumImages)
		{
			ihPtr = &ImageHeaderArray[0];
			for (i = 0; i < NumImages; i++, ihPtr++)
			{
#endif
				if (strstr(ihPtr->ImageName, "FMVs"))
				{
					FMVTexture[NumberOfFMVTextures].IsTriggeredPlotFMV = 1;
					FMVTexture[NumberOfFMVTextures].ImagePtr = ihPtr;
					FMVTexture[NumberOfFMVTextures].StaticImageDrawn = 0;
					FMVTexture[NumberOfFMVTextures].MessageNumber = 0;
					SetupFMVTexture(&FMVTexture[NumberOfFMVTextures]);
					NumberOfFMVTextures++;

					if (NumberOfFMVTextures == MAX_NO_FMVTEXTURES)
					{
						break;
					}
				}
			}
		}
	}
}

void SetupFMVTexture(FMVTEXTURE *ftPtr)
{
	if (ftPtr->PalettedBuf == NULL)
	{
		ftPtr->PalettedBuf = (unsigned char *)calloc(1, 128 * 128 + 128 * 128 * 4);
	}

	if (ftPtr->RGBBuf == NULL)
	{
		if (ftPtr->PalettedBuf == NULL)
		{
			return;
		}
		ftPtr->RGBBuf = &ftPtr->PalettedBuf[128 * 128];
	}

	if (ftPtr->ImagePtr && ftPtr->ImagePtr->D3DTexture)
	{
		pglBindTexture(GL_TEXTURE_2D, ftPtr->ImagePtr->D3DTexture->id);
		pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, &ftPtr->RGBBuf[0]);
	}
}

void UpdateFMVTexture(FMVTEXTURE *ftPtr)
{
	if (!ftPtr || !ftPtr->ImagePtr || !ftPtr->ImagePtr->D3DTexture)
		return;

	pglBindTexture(GL_TEXTURE_2D, ftPtr->ImagePtr->D3DTexture->id);
	pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 96, GL_RGBA, GL_UNSIGNED_BYTE, PlotRGBBuf);
}

static void UpdatePlotFMV(void)
{
	if (!PlotFMVActive || !PlotFmtCtx)
		return;

	if (PlotAlSource)
	{
		ALCcontext *alc_ctx = (ALCcontext *)GetAvpSoundContext();
		if (alc_ctx && alcGetCurrentContext() != alc_ctx)
		{
			alcMakeContextCurrent(alc_ctx);
		}

		ALint processed = 0;
		alGetSourcei(PlotAlSource, AL_BUFFERS_PROCESSED, &processed);
		while (processed-- > 0)
		{
			ALuint unq = 0;
			alSourceUnqueueBuffers(PlotAlSource, 1, &unq);
			if (unq != 0)
			{
				for (int b = 0; b < PLOT_NUM_AUDIO_BUFFERS; b++)
				{
					if (PlotAlBuffers[b] == 0)
					{
						PlotAlBuffers[b] = unq;
						break;
					}
				}
			}
		}

		float gain = 1.0f;
		if (SmackerSoundVolume >= 0)
		{
			gain = (float)SmackerSoundVolume / (float)(ONE_FIXED / 512);
			if (gain > 1.0f) gain = 1.0f;
		}
		alSourcef(PlotAlSource, AL_GAIN, gain);
	}

	uint32_t now = SDL_GetTicks();
	if (now < PlotNextFrameTicks)
	{
		if (PlotAlSource)
		{
			ALint state = 0;
			alGetSourcei(PlotAlSource, AL_SOURCE_STATE, &state);
			if (state != AL_PLAYING)
			{
				ALint queued = 0;
				alGetSourcei(PlotAlSource, AL_BUFFERS_QUEUED, &queued);
				if (queued > 0) alSourcePlay(PlotAlSource);
			}
		}
		return;
	}

	bool got_video = false;
	while (!got_video && PlotFMVActive)
	{
		int ret = av_read_frame(PlotFmtCtx, PlotPacket);
		if (ret < 0)
		{
			// End of file reached; let any remaining queued audio finish playing
			if (PlotAlSource)
			{
				ALint state = 0;
				alGetSourcei(PlotAlSource, AL_SOURCE_STATE, &state);
				if (state == AL_PLAYING && PlotEofDrainFrames++ < 90)
				{
					PlotNextFrameTicks = now + PlotFrameDurationMs;
					return;
				}
			}
			StopPlotFMV();
			return;
		}

		if (PlotPacket->stream_index == PlotVideoIdx && PlotVideoCodecCtx)
		{
			if (avcodec_send_packet(PlotVideoCodecCtx, PlotPacket) >= 0)
			{
				if (avcodec_receive_frame(PlotVideoCodecCtx, PlotVideoFrame) >= 0)
				{
					got_video = true;
					uint8_t *dst_data[4] = { PlotRGBBuf, NULL, NULL, NULL };
					int dst_linesize[4] = { 128 * 4, 0, 0, 0 };
					sws_scale(PlotSwsCtx,
					          (const uint8_t * const *)PlotVideoFrame->data,
					          PlotVideoFrame->linesize,
					          0, PlotVideoCodecCtx->height,
					          dst_data, dst_linesize);
				}
			}
		}
		else if (PlotPacket->stream_index == PlotAudioIdx && PlotAudioCodecCtx && PlotSwrCtx && PlotAlSource)
		{
			if (avcodec_send_packet(PlotAudioCodecCtx, PlotPacket) >= 0)
			{
				while (avcodec_receive_frame(PlotAudioCodecCtx, PlotAudioFrame) >= 0)
				{
					int max_out = swr_get_out_samples(PlotSwrCtx, PlotAudioFrame->nb_samples);
					if (max_out > 0)
					{
						int16_t *pcm_out = (int16_t *)malloc(max_out * sizeof(int16_t));
						if (pcm_out)
						{
							uint8_t *out_arr[1] = { (uint8_t *)pcm_out };
							int out_samples = swr_convert(PlotSwrCtx, out_arr, max_out,
							                             (const uint8_t **)PlotAudioFrame->data, PlotAudioFrame->nb_samples);
							if (out_samples > 0)
							{
								ALint processed = 0;
								alGetSourcei(PlotAlSource, AL_BUFFERS_PROCESSED, &processed);
								while (processed-- > 0)
								{
									ALuint unq = 0;
									alSourceUnqueueBuffers(PlotAlSource, 1, &unq);
									if (unq != 0)
									{
										for (int b = 0; b < PLOT_NUM_AUDIO_BUFFERS; b++)
										{
											if (PlotAlBuffers[b] == 0)
											{
												PlotAlBuffers[b] = unq;
												break;
											}
										}
									}
								}

								int free_slot = -1;
								for (int b = 0; b < PLOT_NUM_AUDIO_BUFFERS; b++)
								{
									if (PlotAlBuffers[b] != 0)
									{
										free_slot = b;
										break;
									}
								}

								if (free_slot >= 0)
								{
									ALuint al_buf = PlotAlBuffers[free_slot];
									PlotAlBuffers[free_slot] = 0; // in-use

									alBufferData(al_buf, AL_FORMAT_MONO16, pcm_out, out_samples * sizeof(int16_t), PLOT_AUDIO_SAMPLE_RATE);
									alSourceQueueBuffers(PlotAlSource, 1, &al_buf);
									ALint state = 0;
									alGetSourcei(PlotAlSource, AL_SOURCE_STATE, &state);
									if (state != AL_PLAYING) alSourcePlay(PlotAlSource);
								}
							}
							free(pcm_out);
						}
					}
				}
			}
		}
		av_packet_unref(PlotPacket);
	}

	if (got_video)
	{
		PlotNextFrameTicks += PlotFrameDurationMs;
		if (PlotNextFrameTicks < now)
			PlotNextFrameTicks = now + PlotFrameDurationMs;

		// Calculate ambient screen lighting for 3D room
		unsigned int totalR = 0, totalG = 0, totalB = 0;
		int samples = (128 * 96) / 16;
		for (int i = 0; i < 128 * 96; i += 16)
		{
			totalR += PlotRGBBuf[i * 4 + 0];
			totalG += PlotRGBBuf[i * 4 + 1];
			totalB += PlotRGBBuf[i * 4 + 2];
		}
		FmvColourRed   = (totalR / samples) * (ONE_FIXED / 256);
		FmvColourGreen = (totalG / samples) * (ONE_FIXED / 256);
		FmvColourBlue  = (totalB / samples) * (ONE_FIXED / 256);

		// Update all in-game monitor textures in OpenGL
		for (int i = 0; i < NumberOfFMVTextures; i++)
		{
			if (FMVTexture[i].IsTriggeredPlotFMV && FMVTexture[i].ImagePtr && FMVTexture[i].ImagePtr->D3DTexture)
			{
				pglBindTexture(GL_TEXTURE_2D, FMVTexture[i].ImagePtr->D3DTexture->id);
				pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 96, GL_RGBA, GL_UNSIGNED_BYTE, PlotRGBBuf);
			}
		}
	}
}

static void UpdateIdleFMVTextures(void)
{
	static uint32_t last_static_ticks = 0;
	uint32_t now = SDL_GetTicks();
	if (now - last_static_ticks < 120) return;
	last_static_ticks = now;

	unsigned int seed = FastRandom();
	uint32_t *p = (uint32_t *)PlotRGBBuf;
	for (int i = 0; i < 128 * 96; i++)
	{
		seed = ((seed * 1664525) + 1013904223);
		uint8_t lum = (seed >> 24) & 0x3F;
		if ((i / 128) & 1) lum >>= 1;
		p[i] = 0xFF000000 | (lum << 16) | (lum << 8) | lum;
	}

	for (int i = 0; i < NumberOfFMVTextures; i++)
	{
		if (FMVTexture[i].ImagePtr && FMVTexture[i].ImagePtr->D3DTexture)
		{
			pglBindTexture(GL_TEXTURE_2D, FMVTexture[i].ImagePtr->D3DTexture->id);
			pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 96, GL_RGBA, GL_UNSIGNED_BYTE, PlotRGBBuf);
		}
	}
}

void UpdateAllFMVTextures(void)
{
	if (PlotFMVActive)
	{
		UpdatePlotFMV();
	}
	else
	{
		UpdateIdleFMVTextures();
	}
}

void ReleaseAllFMVTextures(void)
{
	StopPlotFMV();
	int i = NumberOfFMVTextures;
	while (i--)
	{
		ReleaseFMVTexture(&FMVTexture[i]);
	}
	NumberOfFMVTextures = 0;
}

int NextFMVTextureFrame(FMVTEXTURE *ftPtr, void *bufferPtr)
{
	(void)ftPtr;
	(void)bufferPtr;
	return 1;
}

void UpdateFMVTexturePalette(FMVTEXTURE *ftPtr)
{
	(void)ftPtr;
}

void StartTriggerPlotFMV(int number)
{
	if (number <= 0)
	{
		StopPlotFMV();
		return;
	}

	if (PlotFMVActive && PlotFMVNumber == number)
	{
		return;
	}

	StopPlotFMV();

	char filename[64];
	char resolvedPath[PATH_MAX];

	snprintf(filename, sizeof(filename), "message%d.smk", number);
	if (!FindMovieFilePath(filename, resolvedPath, sizeof(resolvedPath)))
	{
		snprintf(filename, sizeof(filename), "message%d.bik", number);
		if (!FindMovieFilePath(filename, resolvedPath, sizeof(resolvedPath)))
		{
			db_logf_fired("StartTriggerPlotFMV: Could not find movie file for message %d\n", number);
			return;
		}
	}

	db_logf_fired("StartTriggerPlotFMV: Playing message %d from '%s'\n", number, resolvedPath);

	PlotFmtCtx = AvpOpenMediaFile(resolvedPath, &PlotAvioState);
	if (!PlotFmtCtx)
	{
		db_logf_fired("StartTriggerPlotFMV: AvpOpenMediaFile failed for '%s'\n", resolvedPath);
		return;
	}

	if (avformat_find_stream_info(PlotFmtCtx, NULL) < 0)
	{
		db_logf_fired("StartTriggerPlotFMV: avformat_find_stream_info failed\n");
		AvpCloseMediaFile(&PlotAvioState);
		PlotFmtCtx = NULL;
		return;
	}

	PlotVideoIdx = av_find_best_stream(PlotFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	PlotAudioIdx = av_find_best_stream(PlotFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

	if (PlotVideoIdx < 0)
	{
		db_logf_fired("StartTriggerPlotFMV: No video stream in '%s'\n", resolvedPath);
		AvpCloseMediaFile(&PlotAvioState);
		PlotFmtCtx = NULL;
		return;
	}

	AVStream *v_stream = PlotFmtCtx->streams[PlotVideoIdx];
	const AVCodec *v_codec = avcodec_find_decoder(v_stream->codecpar->codec_id);
	if (!v_codec)
	{
		db_logf_fired("StartTriggerPlotFMV: Decoder not found for video codec %d\n", v_stream->codecpar->codec_id);
		AvpCloseMediaFile(&PlotAvioState);
		PlotFmtCtx = NULL;
		return;
	}

	PlotVideoCodecCtx = avcodec_alloc_context3(v_codec);
	if (!PlotVideoCodecCtx || avcodec_parameters_to_context(PlotVideoCodecCtx, v_stream->codecpar) < 0 ||
	    avcodec_open2(PlotVideoCodecCtx, v_codec, NULL) < 0)
	{
		db_logf_fired("StartTriggerPlotFMV: Failed to open video codec\n");
		StopPlotFMV();
		return;
	}

	double fps = 15.0;
	if (v_stream->avg_frame_rate.den > 0 && v_stream->avg_frame_rate.num > 0)
		fps = av_q2d(v_stream->avg_frame_rate);
	else if (v_stream->r_frame_rate.den > 0 && v_stream->r_frame_rate.num > 0)
		fps = av_q2d(v_stream->r_frame_rate);
	if (fps < 1.0 || fps > 120.0) fps = 15.0;
	PlotFrameDurationMs = (uint32_t)(1000.0 / fps);

	PlotSwsCtx = sws_getContext(
		PlotVideoCodecCtx->width, PlotVideoCodecCtx->height, PlotVideoCodecCtx->pix_fmt,
		128, 96, AV_PIX_FMT_RGBA,
		SWS_FAST_BILINEAR, NULL, NULL, NULL
	);
	if (!PlotSwsCtx)
	{
		db_logf_fired("StartTriggerPlotFMV: Failed to create SwsContext\n");
		StopPlotFMV();
		return;
	}

	if (PlotAudioIdx >= 0 && SoundSys_IsOn())
	{
		ALCcontext *alc_ctx = (ALCcontext *)GetAvpSoundContext();
		if (alc_ctx) alcMakeContextCurrent(alc_ctx);

		AVStream *a_stream = PlotFmtCtx->streams[PlotAudioIdx];
		const AVCodec *a_codec = avcodec_find_decoder(a_stream->codecpar->codec_id);
		if (a_codec)
		{
			PlotAudioCodecCtx = avcodec_alloc_context3(a_codec);
			if (PlotAudioCodecCtx && avcodec_parameters_to_context(PlotAudioCodecCtx, a_stream->codecpar) >= 0 &&
			    avcodec_open2(PlotAudioCodecCtx, a_codec, NULL) >= 0)
			{
				AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;
				swr_alloc_set_opts2(&PlotSwrCtx, &out_layout, AV_SAMPLE_FMT_S16, PLOT_AUDIO_SAMPLE_RATE,
				                    &PlotAudioCodecCtx->ch_layout, PlotAudioCodecCtx->sample_fmt, PlotAudioCodecCtx->sample_rate,
				                    0, NULL);
				if (PlotSwrCtx && swr_init(PlotSwrCtx) >= 0)
				{
					memset(PlotAlBuffers, 0, sizeof(PlotAlBuffers));
					memset(PlotAllBuffers, 0, sizeof(PlotAllBuffers));
					alGenSources(1, &PlotAlSource);
					alGenBuffers(PLOT_NUM_AUDIO_BUFFERS, PlotAlBuffers);
					memcpy(PlotAllBuffers, PlotAlBuffers, sizeof(PlotAlBuffers));
					alSourcei(PlotAlSource, AL_LOOPING, AL_FALSE);
					alSourcef(PlotAlSource, AL_GAIN, 1.0f);
				}
			}
		}
	}

	PlotPacket = av_packet_alloc();
	PlotVideoFrame = av_frame_alloc();
	PlotAudioFrame = av_frame_alloc();

	PlotFMVActive = 1;
	PlotFMVNumber = number;
	PlotNextFrameTicks = SDL_GetTicks();

	for (int i = 0; i < NumberOfFMVTextures; i++)
	{
		if (FMVTexture[i].IsTriggeredPlotFMV)
		{
			FMVTexture[i].MessageNumber = number;
			FMVTexture[i].StaticImageDrawn = 0;
		}
	}
}

void StartFMVAtFrame(int number, int frame)
{
	(void)frame;
	StartTriggerPlotFMV(number);
}

void GetFMVInformation(int *messageNumberPtr, int *frameNumberPtr)
{
	*messageNumberPtr = PlotFMVActive ? PlotFMVNumber : 0;
	*frameNumberPtr = 0;
}

void InitialiseTriggeredFMVs(void)
{
	StopPlotFMV();
	for (int i = 0; i < NumberOfFMVTextures; i++)
	{
		if (FMVTexture[i].IsTriggeredPlotFMV)
		{
			FMVTexture[i].MessageNumber = 0;
		}
	}
}

void FindLightingValuesFromTriggeredFMV(unsigned char *bufferPtr, FMVTEXTURE *ftPtr)
{
	(void)bufferPtr;
	(void)ftPtr;
}

void ReleaseFMVTexture(FMVTEXTURE *ftPtr)
{
	ftPtr->MessageNumber = 0;
	if (ftPtr->PalettedBuf != NULL)
	{
		free(ftPtr->PalettedBuf);
		ftPtr->PalettedBuf = NULL;
	}
	ftPtr->RGBBuf = NULL;
}
