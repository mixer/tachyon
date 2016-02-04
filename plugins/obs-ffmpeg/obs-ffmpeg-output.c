/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>

#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <ftl/ftl.h>

#include "obs-ffmpeg-formats.h"
#include "closest-pixel-format.h"
#include "obs-ffmpeg-compat.h"

struct ffmpeg_cfg {
	const char         *url;
	const char         *format_name;
	const char         *format_mime_type;
	const char				 *audio_muxer_settings;
	const char         *muxer_settings;
	int                video_bitrate;
	int                audio_bitrate;
	const char         *video_encoder;
	int                video_encoder_id;
	const char         *audio_encoder;
	int                audio_encoder_id;
	const char         *video_settings;
	const char         *audio_settings;
	enum AVPixelFormat format;
	enum AVColorRange  color_range;
	enum AVColorSpace  color_space;
	int                scale_width;
	int                scale_height;
	int                width;
	int                height;
};

struct ffmpeg_data {
	AVStream           *video;
	AVStream           *audio;
	AVCodec            *acodec;
	AVCodec            *vcodec;
	AVFormatContext    *output_video;
	AVFormatContext    *output_audio;
	struct SwsContext  *swscale;

	int64_t            total_frames;
	AVPicture          dst_picture;
	AVFrame            *vframe;
	int                frame_size;

	uint64_t           start_timestamp;

	int64_t            total_samples;
	uint32_t           audio_samplerate;
	enum audio_format  audio_format;
	size_t             audio_planes;
	size_t             audio_size;
	struct circlebuf   excess_frames[MAX_AV_PLANES];
	uint8_t            *samples[MAX_AV_PLANES];
	AVFrame            *aframe;

	struct ffmpeg_cfg  config;

	bool               initialized;
};

struct ffmpeg_output {
	obs_output_t       *output;
	volatile bool      active;
	struct ffmpeg_data ff_data;
	bool               connecting;

	pthread_t          start_thread;

	bool               write_thread_active;
	pthread_mutex_t    write_mutex;
	pthread_t          write_thread;
	os_sem_t           *write_sem;
	os_event_t         *stop_event;

	DARRAY(AVPacket)   packets_video;
	DARRAY(AVPacket)   packets_audio;

	ftl_stream_configuration_t* stream_config;
	ftl_stream_video_component_t* video_component;
	ftl_stream_audio_component_t* audio_component;

};

/* ------------------------------------------------------------------------- */

static bool new_stream(struct ffmpeg_data *data, AVStream **stream,
		AVCodec **codec, enum AVCodecID id, int audio)
{
	if (audio == 1) {
		*codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
		*stream = avformat_new_stream(data->output_audio, *codec);
	} else {
		*codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
		*stream = avformat_new_stream(data->output_video, *codec);
	}
	if (!*stream) {
		blog(LOG_WARNING, "Couldn't create stream for encoder '%s'",
				avcodec_get_name(id));
		return false;
	}


	if (audio == 1) {
		(*stream)->id = 0;
	} else {
		(*stream)->id = 1;
	}
	return true;
}

static void parse_params(AVCodecContext *context, char **opts)
{
	if (!context || !context->priv_data)
		return;

	while (*opts) {
		char *opt = *opts;
		char *assign = strchr(opt, '=');

		if (assign) {
			char *name = opt;
			char *value;

			*assign = 0;
			value = assign+1;

			av_opt_set(context->priv_data, name, value, 0);
		}

		opts++;
	}
}

static bool open_video_codec(struct ffmpeg_data *data)
{
	AVCodecContext *context = data->video->codec;

	/* Hardcode in quality=realtime */
//	char **opts = strlist_split(data->config.video_settings, ' ', false);
	char **opts = strlist_split("quality=realtime", ' ', false);
	int ret;

	if (opts) {
		parse_params(context, opts);
		strlist_free(opts);
	}

	ret = avcodec_open2(context, data->vcodec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open video codec: %s",
				av_err2str(ret));
		return false;
	}

	data->vframe = av_frame_alloc();
	if (!data->vframe) {
		blog(LOG_WARNING, "Failed to allocate video frame");
		return false;
	}

	data->vframe->format = context->pix_fmt;
	data->vframe->width  = context->width;
	data->vframe->height = context->height;
	data->vframe->colorspace = data->config.color_space;
	data->vframe->color_range = data->config.color_range;

	ret = avpicture_alloc(&data->dst_picture, context->pix_fmt,
			context->width, context->height);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to allocate dst_picture: %s",
				av_err2str(ret));
		return false;
	}

	*((AVPicture*)data->vframe) = data->dst_picture;
	return true;
}

static bool init_swscale(struct ffmpeg_data *data, AVCodecContext *context)
{
	data->swscale = sws_getContext(
			data->config.width, data->config.height,
			data->config.format,
			data->config.scale_width, data->config.scale_height,
			context->pix_fmt,
			SWS_BICUBIC, NULL, NULL, NULL);

	if (!data->swscale) {
		blog(LOG_WARNING, "Could not initialize swscale");
		return false;
	}

	return true;
}

static bool create_video_stream(struct ffmpeg_data *data)
{
	enum AVPixelFormat closest_format;
	AVCodecContext *context;
	struct obs_video_info ovi;

	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "No active video");
		return false;
	}

	if (!new_stream(data, &data->video, &data->vcodec,
				data->output_video->oformat->video_codec,
				0)) {
		blog(LOG_ERROR, "new_stream() failed to make video codec");
		return false;
	}

	//closest_format = get_closest_format(data->config.format,
	//		data->vcodec->pix_fmts);

	/**
	 * closest_format hardcoded for VP8 as removing encoder boxes from the UI
	 * broke this. Annoying but managable. Acceptable PIX_FMTS gotten from FFmpeg
	 * source codec
	 */

	closest_format 					= AV_PIX_FMT_YUV420P;
	context                 = data->video->codec;
	context->bit_rate       = data->config.video_bitrate * 1000;
	context->width          = data->config.scale_width;
	context->height         = data->config.scale_height;
	context->time_base      = (AVRational){ ovi.fps_den, ovi.fps_num };
	context->gop_size       = 120;
	context->pix_fmt        = closest_format;
	context->colorspace     = data->config.color_space;
	context->color_range    = data->config.color_range;

	data->video->time_base = context->time_base;

	if (data->output_video->oformat->flags & AVFMT_GLOBALHEADER)
		context->flags |= CODEC_FLAG_GLOBAL_HEADER;

	if (!open_video_codec(data)) {
		blog(LOG_ERROR, "Failed to open video codec");
		return false;
	}

	if (context->pix_fmt    != data->config.format ||
	    data->config.width  != data->config.scale_width ||
	    data->config.height != data->config.scale_height) {

		if (!init_swscale(data, context)) {
				blog(LOG_ERROR, "Failed to init scale stuff");
				return false;
			}
		}

	return true;
}

static bool open_audio_codec(struct ffmpeg_data *data)
{
	AVCodecContext *context = data->audio->codec;
	char **opts = strlist_split(data->config.video_settings, ' ', false);
	int ret;

	if (opts) {
		parse_params(context, opts);
		strlist_free(opts);
	}

	data->aframe = av_frame_alloc();
	if (!data->aframe) {
		blog(LOG_WARNING, "Failed to allocate audio frame");
		return false;
	}

	context->strict_std_compliance = -2;

	ret = avcodec_open2(context, data->acodec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open audio codec: %s",
				av_err2str(ret));
		return false;
	}

	data->frame_size = context->frame_size ? context->frame_size : 1024;

	ret = av_samples_alloc(data->samples, NULL, context->channels,
			data->frame_size, context->sample_fmt, 0);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to create audio buffer: %s",
		                av_err2str(ret));
		return false;
	}

	return true;
}

static bool create_audio_stream(struct ffmpeg_data *data)
{
	AVCodecContext *context;
	struct obs_audio_info aoi;

	if (!obs_get_audio_info(&aoi)) {
		blog(LOG_WARNING, "No active audio");
		return false;
	}

	if (!new_stream(data, &data->audio, &data->acodec,
				data->output_audio->oformat->audio_codec,
				1))
		return false;

	context              = data->audio->codec;
	context->bit_rate    = data->config.audio_bitrate * 1000;
	context->time_base   = (AVRational){ 1, aoi.samples_per_sec };
	context->channels    = get_audio_channels(aoi.speakers);
	context->sample_rate = aoi.samples_per_sec;
	context->channel_layout =
			av_get_default_channel_layout(context->channels);
	context->sample_fmt  = data->acodec->sample_fmts ?
		data->acodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

	data->audio->time_base = context->time_base;

	data->audio_samplerate = aoi.samples_per_sec;
	data->audio_format = convert_ffmpeg_sample_format(context->sample_fmt);
	data->audio_planes = get_audio_planes(data->audio_format, aoi.speakers);
	data->audio_size = get_audio_size(data->audio_format, aoi.speakers, 1);

	if (data->output_audio->oformat->flags & AVFMT_GLOBALHEADER)
		context->flags |= CODEC_FLAG_GLOBAL_HEADER;

	return open_audio_codec(data);
}

static inline bool init_streams(struct ffmpeg_data *data)
{
	if (data->output_video->oformat->video_codec != AV_CODEC_ID_NONE)
		if (!create_video_stream(data))
			return false;

	if (data->output_audio->oformat->audio_codec != AV_CODEC_ID_NONE)
		if (!create_audio_stream(data))
			return false;

	return true;
}

static inline bool open_output_file(struct ffmpeg_data *data)
{
	AVOutputFormat *format_video = data->output_video->oformat;
	AVOutputFormat *format_audio = data->output_audio->oformat;

	int ret;

	/* Do video */
	if ((format_video->flags & AVFMT_NOFILE) == 0) {
		ret = avio_open(&data->output_video->pb, data->config.url,
				AVIO_FLAG_WRITE);
		if (ret < 0) {
			blog(LOG_WARNING, "Couldn't open video '%s', %s",
					data->config.url, av_err2str(ret));
			return false;
		}
	}

	if ((format_audio->flags & AVFMT_NOFILE) == 0) {
		ret = avio_open(&data->output_audio->pb, data->config.url,
				AVIO_FLAG_WRITE);
		if (ret < 0) {
			blog(LOG_WARNING, "Couldn't open video '%s', %s",
					data->config.url, av_err2str(ret));
			return false;
		}
	}

	strncpy(data->output_audio->filename, data->config.url,
			sizeof(data->output_audio->filename));
	data->output_audio->filename[sizeof(data->output_audio->filename) - 1] = 0;

	strncpy(data->output_video->filename, data->config.url,
			sizeof(data->output_video->filename));
	data->output_video->filename[sizeof(data->output_video->filename) - 1] = 0;

	AVDictionary *dict = NULL;
	if ((ret = av_dict_parse_string(&dict, data->config.muxer_settings,
				"=", " ", 0))) {
		blog(LOG_WARNING, "Failed to parse muxer settings: %s\n%s",
				av_err2str(ret), data->config.muxer_settings);

		av_dict_free(&dict);
		return false;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry,
						AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		blog(LOG_INFO, "Using muxer settings:%s", str.array);
		dstr_free(&str);
	}


	AVDictionary *audio_dict = NULL;
	if ((ret = av_dict_parse_string(&audio_dict, data->config.audio_muxer_settings,
				"=", " ", 0))) {
		blog(LOG_WARNING, "Failed to parse audio muxer settings: %s\n%s",
				av_err2str(ret), data->config.audio_muxer_settings);

		av_dict_free(&audio_dict);
		return false;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(audio_dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);
		}

		blog(LOG_INFO, "Using audio muxer settings:%s", str.array);
		dstr_free(&str);
	}

	ret = avformat_write_header(data->output_audio, &audio_dict);
	if (ret < 0) {
		blog(LOG_WARNING, "Error opening audio '%s': %s",
				data->config.url, av_err2str(ret));
		return false;
	}

	ret = avformat_write_header(data->output_video, &dict);
	if (ret < 0) {
		blog(LOG_WARNING, "Error opening video '%s': %s",
				data->config.url, av_err2str(ret));
		return false;
	}

	av_dict_free(&audio_dict);
	av_dict_free(&dict);

	return true;
}

static void close_video(struct ffmpeg_data *data)
{
	avcodec_close(data->video->codec);
	avpicture_free(&data->dst_picture);

	// This format for some reason derefs video frame
	// too many times
	if (data->vcodec->id == AV_CODEC_ID_A64_MULTI ||
	    data->vcodec->id == AV_CODEC_ID_A64_MULTI5)
		return;

	av_frame_free(&data->vframe);
}

static void close_audio(struct ffmpeg_data *data)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		circlebuf_free(&data->excess_frames[i]);

	av_freep(&data->samples[0]);
	avcodec_close(data->audio->codec);
	av_frame_free(&data->aframe);
}

static void ffmpeg_data_free(struct ffmpeg_data *data)
{
	if (data->initialized) {
		av_write_trailer(data->output_video);
		av_write_trailer(data->output_audio);
	}

	if (data->video)
		close_video(data);
	if (data->audio)
		close_audio(data);

	if (data->output_video) {
		if ((data->output_video->oformat->flags & AVFMT_NOFILE) == 0)
			avio_close(data->output_video->pb);

		avformat_free_context(data->output_video);
	}

	if (data->output_audio) {
		if ((data->output_audio->oformat->flags & AVFMT_NOFILE) == 0)
			avio_close(data->output_audio->pb);

		avformat_free_context(data->output_audio);
	}

	memset(data, 0, sizeof(struct ffmpeg_data));
}

static inline const char *safe_str(const char *s)
{
	if (s == NULL)
		return "(NULL)";
	else
		return s;
}

static enum AVCodecID get_codec_id(const char *name, int id)
{
	AVCodec *codec;

	if (id != 0)
		return (enum AVCodecID)id;

	if (!name || !*name)
		return AV_CODEC_ID_NONE;

	codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return AV_CODEC_ID_NONE;

	return codec->id;
}

static void set_encoder_ids(struct ffmpeg_data *data)
{
	data->output_video->oformat->audio_codec = AV_CODEC_ID_OPUS;
	data->output_video->oformat->video_codec = AV_CODEC_ID_VP8;
	data->output_audio->oformat->audio_codec = AV_CODEC_ID_OPUS;
	data->output_audio->oformat->video_codec = AV_CODEC_ID_VP8;

/*	data->output_video->oformat->video_codec = get_codec_id(
			data->config.video_encoder,
			data->config.video_encoder_id);
	//data->output_video->oformat->audio_codec = AV_CODEC_ID_NONE;

	data->output_audio->oformat->audio_codec = get_codec_id(
			data->config.audio_encoder,
			data->config.audio_encoder_id);
	//data->output_audio->oformat->video_codec = AV_CODEC_ID_NONE;*/
}

static bool ffmpeg_data_init(struct ffmpeg_data *data,
		struct ffmpeg_cfg *config)
{
	bool is_rtmp = false;

	memset(data, 0, sizeof(struct ffmpeg_data));
	data->config = *config;

	if (!config->url || !*config->url)
		return false;

	av_register_all();
	avformat_network_init();

	is_rtmp = (astrcmpi_n(config->url, "rtmp://", 7) == 0);

	AVOutputFormat *output_format = av_guess_format("rtp", NULL, NULL);

	/* Do it twice because avformat_alloc requires it */
	AVOutputFormat *output_format2 = av_guess_format("rtp", NULL, NULL);


	if (output_format == NULL) {
		blog(LOG_WARNING, "Couldn't find matching output format with "
				" parameters: name=%s, url=%s, mime=%s",
				safe_str(is_rtmp ?
					"flv" :	data->config.format_name),
				safe_str(data->config.url),
				safe_str(is_rtmp ?
					NULL : data->config.format_mime_type));
		goto fail;
	}

	avformat_alloc_output_context2(&data->output_audio, output_format,
			NULL, NULL);
	avformat_alloc_output_context2(&data->output_video, output_format2,
			NULL, NULL);

	if (data->config.format_name) {
		set_encoder_ids(data);
	}

	if (!data->output_audio) {
		blog(LOG_WARNING, "Couldn't create audio avformat context");
		goto fail;
	}

	if (!data->output_video) {
		blog(LOG_WARNING, "Couldn't create video avformat context");
		goto fail;
	}

	if (!init_streams(data))
		goto fail;
	if (!open_output_file(data))
		goto fail;

	av_dump_format(data->output_audio, 0, NULL, 1);
	av_dump_format(data->output_video, 0, NULL, 1);

	data->initialized = true;
	return true;

fail:
	blog(LOG_WARNING, "ffmpeg_data_init failed");
	ffmpeg_data_free(data);
	return false;
}

/* ------------------------------------------------------------------------- */

static const char *ffmpeg_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFmpegOutput");
}

static void ffmpeg_log_callback(void *param, int level, const char *format,
		va_list args)
{
	if (level <= AV_LOG_INFO)
		blogva(LOG_DEBUG, format, args);

	UNUSED_PARAMETER(param);
}

static void *ffmpeg_output_create(obs_data_t *settings, obs_output_t *output)
{
	struct ffmpeg_output *data = bzalloc(sizeof(struct ffmpeg_output));
	pthread_mutex_init_value(&data->write_mutex);
	data->output = output;

	if (pthread_mutex_init(&data->write_mutex, NULL) != 0)
		goto fail;
	if (os_event_init(&data->stop_event, OS_EVENT_TYPE_AUTO) != 0)
		goto fail;
	if (os_sem_init(&data->write_sem, 0) != 0)
		goto fail;

	av_log_set_callback(ffmpeg_log_callback);

	UNUSED_PARAMETER(settings);
	return data;

fail:
	pthread_mutex_destroy(&data->write_mutex);
	os_event_destroy(data->stop_event);
	bfree(data);
	return NULL;
}

static void ffmpeg_output_stop(void *data);
static void ffmpeg_deactivate(struct ffmpeg_output *output);

static void ffmpeg_output_destroy(void *data)
{
	struct ffmpeg_output *output = data;

	if (output) {
		if (output->connecting)
			pthread_join(output->start_thread, NULL);

		ffmpeg_output_stop(output);

		pthread_mutex_destroy(&output->write_mutex);
		os_sem_destroy(output->write_sem);
		os_event_destroy(output->stop_event);
		bfree(data);
	}
}

static inline void copy_data(AVPicture *pic, const struct video_data *frame,
		int height)
{
	for (int plane = 0; plane < MAX_AV_PLANES; plane++) {
		if (!frame->data[plane])
			continue;

		int frame_rowsize = (int)frame->linesize[plane];
		int pic_rowsize   = pic->linesize[plane];
		int bytes = frame_rowsize < pic_rowsize ?
			frame_rowsize : pic_rowsize;
		int plane_height = plane == 0 ? height : height/2;

		for (int y = 0; y < plane_height; y++) {
			int pos_frame = y * frame_rowsize;
			int pos_pic   = y * pic_rowsize;

			memcpy(pic->data[plane] + pos_pic,
			       frame->data[plane] + pos_frame,
			       bytes);
		}
	}
}

static void receive_video(void *param, struct video_data *frame)
{
	struct ffmpeg_output *output = param;
	struct ffmpeg_data   *data   = &output->ff_data;

	// codec doesn't support video or none configured
	if (!data->video)
		return;

	AVCodecContext *context = data->video->codec;
	AVPacket packet = {0};
	int ret = 0, got_packet;

	av_init_packet(&packet);

	if (!data->start_timestamp)
		data->start_timestamp = frame->timestamp;

	if (!!data->swscale)
		sws_scale(data->swscale, (const uint8_t *const *)frame->data,
				(const int*)frame->linesize,
				0, data->config.height, data->dst_picture.data,
				data->dst_picture.linesize);
	else
		copy_data(&data->dst_picture, frame, context->height);

	if (data->output_video->flags & AVFMT_RAWPICTURE) {
		packet.flags        |= AV_PKT_FLAG_KEY;
		packet.stream_index  = data->video->index;
		packet.data          = data->dst_picture.data[0];
		packet.size          = sizeof(AVPicture);

		pthread_mutex_lock(&output->write_mutex);
		da_push_back(output->packets_video, &packet);
		pthread_mutex_unlock(&output->write_mutex);
		os_sem_post(output->write_sem);

	} else {
		data->vframe->pts = data->total_frames;
		ret = avcodec_encode_video2(context, &packet, data->vframe,
				&got_packet);
		if (ret < 0) {
			blog(LOG_WARNING, "receive_video: Error encoding "
			                  "video: %s", av_err2str(ret));
			return;
		}

		if (!ret && got_packet && packet.size) {
			packet.pts = rescale_ts(packet.pts, context,
					data->video->time_base);
			packet.dts = rescale_ts(packet.dts, context,
					data->video->time_base);
			packet.duration = (int)av_rescale_q(packet.duration,
					context->time_base,
					data->video->time_base);

			pthread_mutex_lock(&output->write_mutex);
			da_push_back(output->packets_video, &packet);
			pthread_mutex_unlock(&output->write_mutex);
			os_sem_post(output->write_sem);
		} else {
			ret = 0;
		}
	}

	if (ret != 0) {
		blog(LOG_WARNING, "receive_video: Error writing video: %s",
				av_err2str(ret));
	}

	data->total_frames++;
}

static void encode_audio(struct ffmpeg_output *output,
		struct AVCodecContext *context, size_t block_size)
{
	struct ffmpeg_data *data = &output->ff_data;

	AVPacket packet = {0};
	int ret, got_packet;
	size_t total_size = data->frame_size * block_size * context->channels;

	data->aframe->nb_samples = data->frame_size;
	data->aframe->pts = av_rescale_q(data->total_samples,
			(AVRational){1, context->sample_rate},
			context->time_base);

	ret = avcodec_fill_audio_frame(data->aframe, context->channels,
			context->sample_fmt, data->samples[0],
			(int)total_size, 1);
	if (ret < 0) {
		blog(LOG_WARNING, "encode_audio: avcodec_fill_audio_frame "
		                  "failed: %s", av_err2str(ret));
		return;
	}

	data->total_samples += data->frame_size;

	ret = avcodec_encode_audio2(context, &packet, data->aframe,
			&got_packet);
	if (ret < 0) {
		blog(LOG_WARNING, "encode_audio: Error encoding audio: %s",
				av_err2str(ret));
		return;
	}

	if (!got_packet)
		return;

	packet.pts = rescale_ts(packet.pts, context, data->audio->time_base);
	packet.dts = rescale_ts(packet.dts, context, data->audio->time_base);
	packet.duration = (int)av_rescale_q(packet.duration, context->time_base,
			data->audio->time_base);
	packet.stream_index = data->audio->index;

	pthread_mutex_lock(&output->write_mutex);
	da_push_back(output->packets_audio, &packet);
	pthread_mutex_unlock(&output->write_mutex);
	os_sem_post(output->write_sem);
}

static bool prepare_audio(struct ffmpeg_data *data,
		const struct audio_data *frame, struct audio_data *output)
{
	*output = *frame;

	if (frame->timestamp < data->start_timestamp) {
		uint64_t duration = (uint64_t)frame->frames * 1000000000 /
			(uint64_t)data->audio_samplerate;
		uint64_t end_ts = (frame->timestamp + duration);
		uint64_t cutoff;

		if (end_ts <= data->start_timestamp)
			return false;

		cutoff = data->start_timestamp - frame->timestamp;
		cutoff = cutoff * (uint64_t)data->audio_samplerate /
			1000000000;

		for (size_t i = 0; i < data->audio_planes; i++)
			output->data[i] += data->audio_size * (uint32_t)cutoff;
		output->frames -= (uint32_t)cutoff;
	}

	return true;
}

static void receive_audio(void *param, struct audio_data *frame)
{
	struct ffmpeg_output *output = param;
	struct ffmpeg_data   *data   = &output->ff_data;
	size_t frame_size_bytes;
	struct audio_data in;

	// codec doesn't support audio or none configured
	if (!data->audio)
		return;

	AVCodecContext *context = data->audio->codec;

	if (!data->start_timestamp)
		return;
	if (!prepare_audio(data, frame, &in))
		return;

	frame_size_bytes = (size_t)data->frame_size * data->audio_size;

	for (size_t i = 0; i < data->audio_planes; i++)
		circlebuf_push_back(&data->excess_frames[i], in.data[i],
				in.frames * data->audio_size);

	while (data->excess_frames[0].size >= frame_size_bytes) {
		for (size_t i = 0; i < data->audio_planes; i++)
			circlebuf_pop_front(&data->excess_frames[i],
					data->samples[i], frame_size_bytes);

		encode_audio(output, context, data->audio_size);
	}
}

static int process_packet_video(struct ffmpeg_output *output)
{
	AVPacket packet;
	bool new_packet = false;
	int ret;

	pthread_mutex_lock(&output->write_mutex);
	if (output->packets_video.num) {
		packet = output->packets_video.array[0];
		da_erase(output->packets_video, 0);
		new_packet = true;
	}
	pthread_mutex_unlock(&output->write_mutex);

	if (!new_packet)
		return 0;

	/*blog(LOG_DEBUG, "size = %d, flags = %lX, stream = %d, "
			"packets queued: %lu",
			packet.size, packet.flags,
			packet.stream_index, output->packets.num);*/

	ret = av_interleaved_write_frame(output->ff_data.output_video, &packet);
	if (ret < 0) {
		av_free_packet(&packet);
		blog(LOG_WARNING, "receive_audio: Error writing packet: %s",
				av_err2str(ret));
		return ret;
	}

	return 0;
}

static int process_packet_audio(struct ffmpeg_output *output)
{
	AVPacket packet;
	bool new_packet = false;
	int ret;

	pthread_mutex_lock(&output->write_mutex);
	if (output->packets_audio.num) {
		packet = output->packets_audio.array[0];
		da_erase(output->packets_audio, 0);
		new_packet = true;
	}
	pthread_mutex_unlock(&output->write_mutex);

	if (!new_packet)
		return 0;

	/*blog(LOG_DEBUG, "size = %d, flags = %lX, stream = %d, "
			"packets queued: %lu",
			packet.size, packet.flags,
			packet.stream_index, output->packets.num);*/

	ret = av_interleaved_write_frame(output->ff_data.output_audio, &packet);
	if (ret < 0) {
		av_free_packet(&packet);
		blog(LOG_WARNING, "receive_audio: Error writing packet: %s",
				av_err2str(ret));
		return ret;
	}

	return 0;
}

static void *write_thread(void *data)
{
	struct ffmpeg_output *output = data;

	while (os_sem_wait(output->write_sem) == 0) {
		/* check to see if shutting down */
		if (os_event_try(output->stop_event) == 0)
			break;

		int ret = process_packet_video(output);

		/* HACKY HAC HACK HACK */
		if (ret == 0) {
			ret = process_packet_audio(output);
		}

		if (ret != 0) {
			int code = OBS_OUTPUT_ERROR;

			pthread_detach(output->write_thread);
			output->write_thread_active = false;

			if (ret == -ENOSPC)
				code = OBS_OUTPUT_NO_SPACE;

			obs_output_signal_stop(output->output, code);
			ffmpeg_deactivate(output);
			break;
		}
	}

	output->active = false;
	return NULL;
}

static inline const char *get_string_or_null(obs_data_t *settings,
		const char *name)
{
	const char *value = obs_data_get_string(settings, name);
	if (!value || !strlen(value))
		return NULL;
	return value;
}

static bool try_connect(struct ffmpeg_output *output)
{
	video_t *video = obs_output_video(output->output);
	const struct video_output_info *voi = video_output_get_info(video);
	struct ffmpeg_cfg config;
	obs_data_t *settings;
	bool success;
	ftl_status_t status_code;
	int ret;

	settings = obs_output_get_settings(output->output);
	config.url = obs_data_get_string(settings, "url");
	config.format_name = get_string_or_null(settings, "format_name");
	config.format_mime_type = get_string_or_null(settings,
			"format_mime_type");
	config.audio_muxer_settings = "ssrc=2 payload_type=97";
	config.muxer_settings = "ssrc=1";
	config.video_bitrate = (int)obs_data_get_int(settings, "video_bitrate");
	config.audio_bitrate = (int)obs_data_get_int(settings, "audio_bitrate");
	config.scale_width = (int)obs_data_get_int(settings, "scale_width");
	config.scale_height = (int)obs_data_get_int(settings, "scale_height");
	config.width  = (int)obs_output_get_width(output->output);
	config.height = (int)obs_output_get_height(output->output);
	config.format = AV_PIX_FMT_YUV420P;

	if (format_is_yuv(voi->format)) {
		config.color_range = voi->range == VIDEO_RANGE_FULL ?
			AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		config.color_space = voi->colorspace == VIDEO_CS_709 ?
			AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
	} else {
		config.color_range = AVCOL_RANGE_UNSPECIFIED;
		config.color_space = AVCOL_SPC_RGB;
	}

	if (config.format == AV_PIX_FMT_NONE) {
		blog(LOG_DEBUG, "invalid pixel format used for FFmpeg output");
		return false;
	}

	if (!config.scale_width)
		config.scale_width = config.width;
	if (!config.scale_height)
		config.scale_height = config.height;

	/* Use Charon to autheticate and configure muxer settings */
	ftl_init();
	status_code = ftl_create_stream_configuration(&(output->stream_config));
	if (status_code != FTL_SUCCESS) {
     blog(LOG_WARNING, "Failed to initialize stream configuration: errno %d\n", status_code);
     return false;
   }

	ftl_set_ingest_location(output->stream_config, "127.0.0.1");
	ftl_set_authetication_key(output->stream_config, 2, "1234561abcde");

	int video_ssrc = 1;

	output->video_component = ftl_create_video_component(FTL_VIDEO_VP8, 96, video_ssrc, config.scale_width, config.scale_height);
	ftl_attach_video_component_to_stream(output->stream_config, output->video_component);

	int audio_ssrc = 2;
	output->audio_component = ftl_create_audio_component(FTL_AUDIO_OPUS, 97, audio_ssrc);
	ftl_attach_audio_component_to_stream(output->stream_config, output->audio_component);

	if (ftl_activate_stream(output->stream_config)  != FTL_SUCCESS) {
		blog(LOG_ERROR, "Failed to initialize FTL Stream");
		return false;
	}

	success = ffmpeg_data_init(&output->ff_data, &config);
	obs_data_release(settings);

	if (!success)
		return false;

	struct audio_convert_info aci = {
		.format = output->ff_data.audio_format
	};

	output->active = true;

	if (!obs_output_can_begin_data_capture(output->output, 0))
		return false;

	ret = pthread_create(&output->write_thread, NULL, write_thread, output);
	if (ret != 0) {
		blog(LOG_WARNING, "ffmpeg_output_start: failed to create write "
		                  "thread.");
		ffmpeg_output_stop(output);
		return false;
	}

	obs_output_set_video_conversion(output->output, NULL);
	obs_output_set_audio_conversion(output->output, &aci);
	obs_output_begin_data_capture(output->output, 0);
	output->write_thread_active = true;
	return true;
}

static void *start_thread(void *data)
{
	struct ffmpeg_output *output = data;

	if (!try_connect(output))
		obs_output_signal_stop(output->output,
				OBS_OUTPUT_CONNECT_FAILED);

	output->connecting = false;
	return NULL;
}

static bool ffmpeg_output_start(void *data)
{
	struct ffmpeg_output *output = data;
	int ret;

	if (output->connecting)
		return false;

	ret = pthread_create(&output->start_thread, NULL, start_thread, output);
	return (output->connecting = (ret == 0));
}

static void ffmpeg_output_stop(void *data)
{
	struct ffmpeg_output *output = data;

	if (output->active) {
		obs_output_end_data_capture(output->output);
		ffmpeg_deactivate(output);
	}

	if (output->stream_config) {
		ftl_deactivate_stream(output->stream_config);
		ftl_destory_stream(&(output->stream_config));
		output->stream_config = 0; /* FTL requires the pointer be 0ed out */
	}
}

static void ffmpeg_deactivate(struct ffmpeg_output *output)
{
	if (output->write_thread_active) {
		os_event_signal(output->stop_event);
		os_sem_post(output->write_sem);
		pthread_join(output->write_thread, NULL);
		output->write_thread_active = false;
	}

	pthread_mutex_lock(&output->write_mutex);

	for (size_t i = 0; i < output->packets_video.num; i++)
		av_free_packet(output->packets_video.array+i);
	da_free(output->packets_video);

	for (size_t i = 0; i < output->packets_audio.num; i++)
		av_free_packet(output->packets_audio.array+i);
	da_free(output->packets_audio);

	pthread_mutex_unlock(&output->write_mutex);

	ffmpeg_data_free(&output->ff_data);
}

struct obs_output_info ffmpeg_output = {
	.id        = "ffmpeg_output",
	.flags     = OBS_OUTPUT_AUDIO | OBS_OUTPUT_VIDEO,
	.get_name  = ffmpeg_output_getname,
	.create    = ffmpeg_output_create,
	.destroy   = ffmpeg_output_destroy,
	.start     = ffmpeg_output_start,
	.stop      = ffmpeg_output_stop,
	.raw_video = receive_video,
	.raw_audio = receive_audio,
};
