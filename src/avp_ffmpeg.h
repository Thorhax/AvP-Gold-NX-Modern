#ifndef _AVP_FFMPEG_H_
#define _AVP_FFMPEG_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

typedef struct AvpAvioContext {
	FILE *fp;
	AVIOContext *avio_ctx;
	AVFormatContext *fmt_ctx;
} AvpAvioContext;

static inline int avp_avio_read_callback(void *opaque, uint8_t *buf, int buf_size)
{
	FILE *fp = (FILE *)opaque;
	size_t r = fread(buf, 1, buf_size, fp);
	if (r == 0)
	{
		if (feof(fp)) return AVERROR_EOF;
		return AVERROR(errno ? errno : EIO);
	}
	return (int)r;
}

static inline int64_t avp_avio_seek_callback(void *opaque, int64_t offset, int whence)
{
	FILE *fp = (FILE *)opaque;
	if (whence == AVSEEK_SIZE)
	{
		struct stat st;
		if (fstat(fileno(fp), &st) == 0)
			return st.st_size;
		return -1;
	}
	if (fseek(fp, (long)offset, whence) != 0)
		return -1;
	return ftell(fp);
}

static inline AVFormatContext *AvpOpenMediaFile(const char *path, AvpAvioContext *ctx)
{
	if (!path || !ctx) return NULL;
	memset(ctx, 0, sizeof(*ctx));

	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;

	size_t buf_size = 64 * 1024;
	uint8_t *buf = (uint8_t *)av_malloc(buf_size);
	if (!buf)
	{
		fclose(fp);
		return NULL;
	}

	AVIOContext *avio = avio_alloc_context(buf, buf_size, 0, fp,
	                                       avp_avio_read_callback, NULL, avp_avio_seek_callback);
	if (!avio)
	{
		av_free(buf);
		fclose(fp);
		return NULL;
	}

	AVFormatContext *fmt = avformat_alloc_context();
	if (!fmt)
	{
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		fclose(fp);
		return NULL;
	}

	fmt->pb = avio;
	fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

	if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
	{
		avformat_close_input(&fmt);
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		fclose(fp);
		return NULL;
	}

	ctx->fp = fp;
	ctx->avio_ctx = avio;
	ctx->fmt_ctx = fmt;
	return fmt;
}

static inline void AvpCloseMediaFile(AvpAvioContext *ctx)
{
	if (!ctx) return;
	if (ctx->fmt_ctx)
	{
		avformat_close_input(&ctx->fmt_ctx);
		ctx->fmt_ctx = NULL;
	}
	if (ctx->avio_ctx)
	{
		av_freep(&ctx->avio_ctx->buffer);
		avio_context_free(&ctx->avio_ctx);
		ctx->avio_ctx = NULL;
	}
	if (ctx->fp)
	{
		fclose(ctx->fp);
		ctx->fp = NULL;
	}
}

#endif // _AVP_FFMPEG_H_
