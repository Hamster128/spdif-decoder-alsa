/*
 * codechandler.c
 *
 *  Created on: 25.04.2015
 *      Author: sebastian
 */

#include <stdio.h>
#include <errno.h>
#include <err.h>
#include "resample.h"
#include "codechandler.h"
#include "myspdif.h"

extern int debug_data;

void CodecHandler_init(CodecHandler* h)
{
	h->codec = NULL;
	h->codecContext = NULL;
	h->currentChannelCount = 0;
	h->currentCodecID = AV_CODEC_ID_NONE;
	h->currentSampleRate = 0;
	h->swr = resample_init();
	h->frame = av_frame_alloc();
}

//--------------------------------------------------------------------------------------------------
char error_bfufer[1024];
char* my_av_strerror(int err) 
{
  av_strerror(err, error_bfufer, sizeof(error_bfufer));
  return error_bfufer;
}

//--------------------------------------------------------------------------------------------------
void CodecHandler_deinit(CodecHandler* h)
{
	resample_deinit(h->swr);
	av_frame_free(&h->frame);
}

//--------------------------------------------------------------------------------------------------
int CodecHandler_loadCodec(CodecHandler * handler, AVFormatContext * formatcontext)
{
  int err;

	if (formatcontext->nb_streams == 0)
    errx(1, "loadCodec: no stream\n");

	if(handler->currentCodecID == formatcontext->streams[0]->codec->codec_id)
  {
		//Codec already loaded
		return 0;
	}

  if(debug_data) printf("loadCodec %s\n", avcodec_get_name(formatcontext->streams[0]->codec->codec_id));

	if(handler->codecContext) 
  {
    if(debug_data) printf("loadCodec closeCodec\n");    
		CodecHandler_closeCodec(handler);
  }

	handler->currentCodecID = AV_CODEC_ID_NONE;

	handler->codec = avcodec_find_decoder(formatcontext->streams[0]->codec->codec_id);

	if (!handler->codec) 
    errx(1, "loadCodec: could not find codec\n");

	handler->codecContext = avcodec_alloc_context3(handler->codec);

	if (!handler->codecContext)
		errx(1, "loadCodec: cannot allocate codec");

  if(formatcontext->streams[0]->codec->codec_id == AV_CODEC_ID_PCM_S16LE)
  {
    // https://www.ffmpeg.org/doxygen/4.3/structAVCodecContext.html
    handler->codecContext->sample_fmt      = AV_SAMPLE_FMT_S16;
    handler->codecContext->sample_rate     = 48000;
    handler->codecContext->channels        = 2;
    handler->codecContext->channel_layout  = AV_CH_LAYOUT_STEREO;
  }

	if ((err = avcodec_open2(handler->codecContext, handler->codec, NULL)) != 0)
		errx(1, "loadCodec: cannot open codec %s", my_av_strerror(err));

	handler->currentCodecID = formatcontext->streams[0]->codec->codec_id;

  printf("Loaded codec %s\n", avcodec_get_name(handler->currentCodecID));

	return 0;
}

//--------------------------------------------------------------------------------------------------
int CodecHandler_decodeCodec(CodecHandler * h, AVPacket * pkt,
		uint8_t *outbuffer, uint32_t* bufferfilled)
{
	int got_frame;

  if(debug_data) printf("decodeCodec decode_audio4 %d bytes\n", pkt->size);

	int processed_len = avcodec_decode_audio4(h->codecContext, h->frame, &got_frame, pkt);

	if (processed_len < 0) 
  {
    printf("cannot decode input: %s\n", my_av_strerror(processed_len));
    return SPIF_DECODER_RESTART_REQUIRED;
  }

	int ret = 0;

	pkt->data += processed_len;
	pkt->size -= processed_len;

	if(h->currentChannelCount  != h->codecContext->channels        || 
     h->currentSampleRate    != h->codecContext->sample_rate     ||
     h->currentChannelLayout != h->codecContext->channel_layout     )
  {
    if(debug_data) printf("ecodeCodec loadFromCodec\n");

		resample_loadFromCodec(h->swr, h->codecContext);

    if(h->currentChannelCount && h->currentChannelCount  != h->codecContext->channels)
      printf("channels changed: %d > %d\n", h->currentChannelCount, h->codecContext->channels);

		ret = 1;
	}

  if(debug_data) printf("decodeCodec swr_convert\n");

	swr_convert(h->swr, &outbuffer, h->frame->nb_samples, (const uint8_t **)h->frame->data, h->frame->nb_samples);

  if(debug_data) printf("decodeCodec get_buffer_size\n");

	*bufferfilled = av_samples_get_buffer_size(NULL,
			   h->codecContext->channels,
			   h->frame->nb_samples,
			   AV_SAMPLE_FMT_S16,
			   1);

	h->currentChannelCount  = h->codecContext->channels;
	h->currentSampleRate    = h->codecContext->sample_rate;
	h->currentChannelLayout = h->codecContext->channel_layout;

  if(debug_data) printf("decodeCodec done\n");
	return ret;
}


//--------------------------------------------------------------------------------------------------
int CodecHandler_closeCodec(CodecHandler * handler)
{
	if(handler->codecContext != NULL)
  {
		avcodec_close(handler->codecContext);
		avcodec_free_context(&handler->codecContext);
	}

	handler->codec = NULL;
	handler->codecContext = NULL;

	return 0;
}
