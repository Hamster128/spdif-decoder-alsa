/*
 * IEC 61937 demuxer
 * Copyright (c) 2010 Anssi Hannula <anssi.hannula at iki.fi>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * IEC 61937 demuxer, used for compressed data in S/PDIF
 * @author Anssi Hannula
 */

#include <libavformat/avformat.h>
#include <libavformat/spdif.h>
#include "myspdif.h"
#include <libavcodec/ac3.h>
#include "libavcodec/adts_parser.h"
#include "libavutil/bswap.h"

extern int debug_data, skipPkts;

static int spdif_get_offset_and_codec(AVFormatContext *s,
                                      enum IEC61937DataType data_type,
                                      const char *buf, int *offset,
                                      enum AVCodecID *codec)
{
    uint32_t samples;
    uint8_t frames;
    int ret;

    switch (data_type & 0xff) {
    case IEC61937_AC3:
        *offset = AC3_FRAME_SIZE << 2;
        *codec = AV_CODEC_ID_AC3;
        break;
    case IEC61937_EAC3:
        *offset = 0;
        *codec = AV_CODEC_ID_EAC3;
        break;
    case IEC61937_MPEG1_LAYER1:
        *offset = spdif_mpeg_pkt_offset[1][0];
        *codec = AV_CODEC_ID_MP1;
        break;
    case IEC61937_MPEG1_LAYER23:
        *offset = spdif_mpeg_pkt_offset[1][0];
        *codec = AV_CODEC_ID_MP3;
        break;
    case IEC61937_MPEG2_EXT:
        *offset = 4608;
        *codec = AV_CODEC_ID_MP3;
        break;
    case IEC61937_MPEG2_AAC:
        ret = av_adts_header_parse(buf, &samples, &frames);
        if (ret < 0) {
            if (s) /* be silent during a probe */
                av_log(s, AV_LOG_ERROR, "Invalid AAC packet in IEC 61937\n");
            return ret;
        }
        *offset = samples << 2;
        *codec = AV_CODEC_ID_AAC;
        break;
    case IEC61937_MPEG2_LAYER1_LSF:
        *offset = spdif_mpeg_pkt_offset[0][0];
        *codec = AV_CODEC_ID_MP1;
        break;
    case IEC61937_MPEG2_LAYER2_LSF:
        *offset = spdif_mpeg_pkt_offset[0][1];
        *codec = AV_CODEC_ID_MP2;
        break;
    case IEC61937_MPEG2_LAYER3_LSF:
        *offset = spdif_mpeg_pkt_offset[0][2];
        *codec = AV_CODEC_ID_MP3;
        break;
    case IEC61937_DTS1:
        *offset = 2048;
        *codec = AV_CODEC_ID_DTS;
        break;
    case IEC61937_DTS2:
        *offset = 4096;
        *codec = AV_CODEC_ID_DTS;
        break;
    case IEC61937_DTS3:
        *offset = 8192;
        *codec = AV_CODEC_ID_DTS;
        break;
    default:
        return AVERROR_PATCHWELCOME;
    }
    return 0;
}


enum IEC61937DataType last_data_type = 0xFF;
uint32_t state = 0;

int my_spdif_read_packet(AVFormatContext *spdif_ctx, AVPacket *pkt,
		uint8_t * garbagebuffer, int garbagebuffersize, int * garbagebufferfilled)
{
    AVIOContext *pb = spdif_ctx->pb;
    enum IEC61937DataType data_type;
    enum AVCodecID codec_id;
    unsigned int pkt_size, offset;
    int ret;
    uint8_t *garbagebuffer_start = garbagebuffer;
    double start = 0;

    *garbagebufferfilled = 0;

    if(debug_data)
      start = gettimeofday_ms();

    while (state != (AV_BSWAP16C(SYNCWORD1) << 16 | AV_BSWAP16C(SYNCWORD2))) 
    {
    	if(*garbagebufferfilled < garbagebuffersize)
      {
    		*garbagebuffer = avio_r8(pb);
    		(*garbagebufferfilled)++;

    		state = (state << 8) | *garbagebuffer;
    		garbagebuffer++;

    		if (avio_feof(pb)) {
          printf("read_packet EOF\n");
          return AVERROR_EOF;
        }
    	}
      else 
      {
        if(skipPkts) 
        {
          skipPkts--;
          printf("skipping packet %d\n", skipPkts);
          return SPIF_DECODER_RETRY_REQUIRED;
        }

        if(last_data_type)
          printf("No packet found > PCM\n");

        // no stream found > unencoded PCM
        last_data_type = 0;

        if (spdif_ctx->nb_streams)
        {
          printf("active stream > restart\n");
          return SPIF_DECODER_RESTART_REQUIRED;
        }

        if(debug_data)
          printf("read_packet PCM\n");

    		return SPIF_DECODER_PCM;
      }
    }

    if(debug_data)
    {
      double end = gettimeofday_ms();
      printf("read_packet start in %.1lf ms\n", end - start);
      start = end;
    }

    *garbagebufferfilled -= 4;
    state = 0;
    data_type = avio_rl16(pb);
    pkt_size  = avio_rl16(pb); 

    if(data_type != IEC61937_EAC3)
    {
      // size in bits, max 2048 frames

      if (pkt_size % 16)
        avpriv_request_sample(spdif_ctx, "Packet not ending at a 16-bit boundary");

        pkt_size = pkt_size >> 3;  // bits -> bytes
    }

    ret = av_new_packet(pkt, pkt_size);
    if (ret)
      return ret;

    pkt->pos = -1;

    if (avio_read(pb, pkt->data, pkt->size) < pkt->size) 
    {
      printf("read_packet: error avio_read\n");
      av_free_packet(pkt);
      return AVERROR_EOF;
    }

    if(debug_data)
    {
      double end = gettimeofday_ms();
      printf("read_packet %d bytes in %.1lf ms\n", pkt->size, gettimeofday_ms() - start);
      start = end;
    }

    my_spdif_bswap_buf16((uint16_t *)pkt->data, (uint16_t *)pkt->data, pkt->size >> 1);

    ret = spdif_get_offset_and_codec(spdif_ctx, data_type, pkt->data, &offset, &codec_id);

    if (ret) 
    {
      if(data_type != last_data_type)
        printf("Unknown codec %d\n", data_type & 0xff);

      last_data_type = data_type;
      av_free_packet(pkt);

      if (spdif_ctx->nb_streams)
      {
        printf("active stream > restart\n");
        return SPIF_DECODER_RESTART_REQUIRED;
      }
      else 
        return SPIF_DECODER_RETRY_REQUIRED;
    }

    last_data_type = data_type;

    if(debug_data)
      printf("read_packet codec %s\n", avcodec_get_name(codec_id));

    // skip over the padding to the beginning of the next frame
    int skip_bytes = offset - pkt->size - BURST_HEADER_SIZE;

    if(offset && skip_bytes) 
    {
      if(debug_data)
        start = gettimeofday_ms();

      avio_skip(pb, skip_bytes);

      if(debug_data)
      {
        double end = gettimeofday_ms();
        printf("read_packet skip %d bytes in %.1lf ms\n", skip_bytes, gettimeofday_ms() - start);
        start = end;
      }
    }

    if(skipPkts) 
    {
      skipPkts--;
      av_free_packet(pkt);
      printf("skipping packet %d\n", skipPkts);
      return SPIF_DECODER_RETRY_REQUIRED;
    }

    if (!spdif_ctx->nb_streams) 
    {
      // first packet, create a stream 
      AVStream *st = avformat_new_stream(spdif_ctx, NULL);

      if (!st) 
      {
        printf("read_packet: Could not create stream\n");
        av_free_packet(pkt);
        return AVERROR(ENOMEM);
      }

      st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
      st->codec->codec_id   = codec_id;

      if (!spdif_ctx->bit_rate && spdif_ctx->streams[0]->codec->sample_rate)
        // stream bitrate matches 16-bit stereo PCM bitrate for currently supported codecs
        spdif_ctx->bit_rate = 2 * 16 * spdif_ctx->streams[0]->codec->sample_rate;
    } 
    else if (codec_id != spdif_ctx->streams[0]->codec->codec_id)
    {
      printf("codec changed from %s to %s\n", avcodec_get_name(spdif_ctx->streams[0]->codec->codec_id), avcodec_get_name(codec_id));
      av_free_packet(pkt);
      return SPIF_DECODER_RESTART_REQUIRED;
    }
   
    return 0;
}
