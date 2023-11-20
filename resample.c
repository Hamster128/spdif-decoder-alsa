/*
 * resample.c
 *
 *  Created on: 20.04.2015
 *      Author: sebastian
 */
#include "resample.h"

#include <libavutil/opt.h>

SwrContext* resample_init(){
	return swr_alloc();
}

void resample_deinit(SwrContext* swr){
	swr_free(&swr);
}

void resample_loadFromCodec(SwrContext *swr, AVCodecContext* audioCodec){
	int err;

	// Set up SWR context once you've got codec information
	av_opt_set_int(swr, "in_channel_layout",  audioCodec->channel_layout, 0);
	av_opt_set_int(swr, "out_channel_layout", audioCodec->channel_layout, 0);
	av_opt_set_int(swr, "in_sample_rate",     audioCodec->sample_rate, 0);
	av_opt_set_int(swr, "out_sample_rate",    audioCodec->sample_rate, 0);
	av_opt_set_sample_fmt(swr, "in_sample_fmt",  audioCodec->sample_fmt, 0);
	av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16,  0);

	if((err = swr_init(swr)) < 0) 
		errx(1, "resample_loadFromCodec: swr_init failed %s", my_av_strerror(err));
}

void resample_do(SwrContext* swr, AVFrame *audioFrame, uint8_t* outputBuffer){
	swr_convert(swr,
			&outputBuffer,
			audioFrame->nb_samples,
			(const uint8_t**)audioFrame->data,
			audioFrame->nb_samples);
}
