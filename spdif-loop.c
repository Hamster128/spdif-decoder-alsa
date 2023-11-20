#define _GNU_SOURCE

/*
journalctl -r -u dsp
*/

#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <err.h>
#include <getopt.h>

#include <sys/socket.h> 
#include <arpa/inet.h> 
#include <netinet/in.h> 
#include <netinet/tcp.h>
#include <netdb.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <ao/ao.h>
#include <libavformat/avio.h>
#include <libavformat/spdif.h>

#include <alsa/asoundlib.h>

#include "resample.h"
#include "helper.h"
#include "myspdif.h"
#include "codechandler.h"

//#define DEBUG
//#define MAX_BURST_SIZE	24576           //  Dolby Digital+ bust            = 6144 frames = 128ms
#define MAX_BURST_SIZE	(8+1792+4344)     //  Dolby Digital  bust 6144 bytes = 1536 frames =  32ms
#define I_BUFFER_SIZE 768

typedef double sample_t;

char *alsa_dev_name = NULL;
char *out_dev_buffer_time = "64"; // 2 packets of 32ms
AVFormatContext *spdif_ctx = NULL;
unsigned char *alsa_buf = NULL;
AVInputFormat *spdif_fmt = NULL;
CodecHandler codecHandler;

struct alsa_read_state {
	snd_pcm_t *dev;
};
snd_pcm_t *out_dev = NULL;

struct alsa_read_state read_state = {.dev = NULL,};

int debug_data = 0, progPos = 0, progCycles = 0;
int outDelay = 0;

//--------------------------------------------------------------------------------------------------
void usage(void)
{
	fprintf(stderr,
		"usage:\n"
		"  spdif-loop -i <alsa-input-dev> -o <alsa-output-dev>\n\n"

    " -b n ... output device buffer time in ms (default 2 packets = 64ms)\n"
    " -v   ... verbose\n\n"

    " <alsa-output-dev> can contain '#' to direct output to different devices depending on channel count.\n"
    "   ex: -o dsp#    2 channels -> dsp2, 6 channels -> dsp6, and so on\n");

	exit(1);
}

//--------------------------------------------------------------------------------------------------
static int alsa_reader(void *data, uint8_t *buf, int buf_size)
{
	struct alsa_read_state *state = data;
  double start = 0;

  if(debug_data) 
    start = gettimeofday_ms();

	if (snd_pcm_state(state->dev) == SND_PCM_STATE_SETUP)
  {
  	int err = snd_pcm_prepare(state->dev);

	  if(err < 0)
    {
      printf("error: alsa failed to prepare input device: %s", snd_strerror(err));
      return AVERROR_EOF;
    }

    if(debug_data)
      printf("alsa input prepared\n");
  }

  int frames = buf_size / 4;

	while(1)
  {
    ssize_t n = snd_pcm_readi(state->dev, (char *) buf, frames);

    if(n >= 0) 
    {
      if(debug_data)
        printf("alsa_reader %d bytes in %.1f ms\n", n*4, gettimeofday_ms() - start);

      return n * 4;
    }

    if (n == -EPIPE)
      printf("warning: alsa input overrun occurred\n");
    else
      printf("warning: alsa input %s\n", snd_strerror(n));

    n = snd_pcm_recover(state->dev, n, 1);

    if (n < 0)
    {
      printf("error: alsa input recover failed %s\n", snd_strerror(n));
      return AVERROR_EOF;
    }
  }
}

//--------------------------------------------------------------------------------------------------
ssize_t alsa_write(sample_t *buf, int buf_size)
{
	ssize_t n;

	if (snd_pcm_state(out_dev) == SND_PCM_STATE_SETUP)
  {
  	int err = snd_pcm_prepare(out_dev);

	  if(err < 0)
    {
      printf("error: alsa failed to prepare output device: %s\n", snd_strerror(err));
      return 0;
    }

    if(debug_data)
      printf("alsa output prepared\n");
  }

  snd_pcm_sframes_t delay;
  int err;
  if ((err = snd_pcm_delay(out_dev, &delay)) < 0)
    printf("alsa error: failed to get output latency: %s\n", snd_strerror(err));
  else
  {
    delay /= 48;

    if(delay > outDelay) 
    {
    outDelay = delay;
    printf("alsa output latency: %d ms\n", outDelay);
    }
  }

  int frames = buf_size / 2 / codecHandler.currentChannelCount;

  while(1) 
  {
    n = snd_pcm_writei(out_dev, buf, frames);

    if(n >= 0)
      return n;

    if (n == -EPIPE)
      printf("warning: alsa output underrun occurred\n");
    else
      printf("warning: alsa output %s\n", snd_strerror(n));

    n = snd_pcm_recover(out_dev, n, 1);

    if (n < 0) 
    {
      printf("error: alsa output recover failed %s\n", snd_strerror(n));
      return 0;
    }
  }
}

//--------------------------------------------------------------------------------------------------
snd_pcm_t* alsa_open(char* dev_name, int channels)
{
	snd_pcm_hw_params_t *p = NULL;
  snd_pcm_t *dev = NULL;
  int err;
  double start;

  if(debug_data)
    start = gettimeofday_ms();

	if ((err = snd_pcm_open(&dev, dev_name, channels ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE, 0)) < 0)
		errx(1, "alsa error: failed to open device: %s", snd_strerror(err));
	
	if ((err = snd_pcm_hw_params_malloc(&p)) < 0)
		errx(1, "alsa error: failed to allocate hw params: %s", snd_strerror(err));
	
	if ((err = snd_pcm_hw_params_any(dev, p)) < 0)
		errx(1, "alsa error: failed to initialize hw params: %s", snd_strerror(err));

	if ((err = snd_pcm_hw_params_set_access(dev, p, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		errx(1, "alsa error: failed to set access: %s", snd_strerror(err));
	
	if ((err = snd_pcm_hw_params_set_format(dev, p, SND_PCM_FORMAT_S16)) < 0)
		errx(1, "alsa error: failed to set format: %s", snd_strerror(err));

	if ((err = snd_pcm_hw_params_set_rate(dev, p, 48000, 0)) < 0)
		errx(1, "alsa error: failed to set rate: %s", snd_strerror(err));

	if (channels)
  {
    unsigned int us = atoi(out_dev_buffer_time) * 1000;

    if(( err = snd_pcm_hw_params_set_buffer_time_min(dev, p, &us, 0)) < 0)
  		errx(1, "alsa error: cannot set output device buffer_time_min %s %s", out_dev_buffer_time, snd_strerror(err));

    if(debug_data)
      printf("alse open output, channels=%d\n", channels);
  } 
  else
  {
    channels = 2;
    if(debug_data)
      printf("alse open input, channels=%d\n", channels);
  }

	if ((err = snd_pcm_hw_params_set_channels(dev, p, channels)) < 0)
		errx(1, "alsa error: failed to set channels %d: %s", channels, snd_strerror(err));

	if ((err = snd_pcm_hw_params(dev, p)) < 0) 
		errx(1, "alsa error: failed to set params: %s", snd_strerror(err));

	snd_pcm_hw_params_free(p);

  if(debug_data) 
    printf("alse open %s, channels=%d in %.1lf ms\n", dev_name, channels, gettimeofday_ms() - start);

  return dev;
}

//--------------------------------------------------------------------------------------------------
void* sendInfoToSocketThread(CodecHandler* codecHandler)
{
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  
  if (sockfd < 0)
    return;

  /* get the address of the host */
  struct hostent* hptr = gethostbyname("localhost");
  
  if (!hptr) 
  {
    printf("sendInfoToSocket: gethostbyname\n");
    return;
  }

  if (hptr->h_addrtype != AF_INET) /* versus AF_LOCAL */
  {
    printf("sendInfoToSocket: bad address family\n");
    return;
  }

  /* connect to the server: configure server's address 1st */
  struct sockaddr_in saddr; 
  memset(&saddr, 0, sizeof(saddr)); 
  saddr.sin_family = AF_INET; 
  saddr.sin_addr.s_addr = ((struct in_addr*) hptr->h_addr_list[0])->s_addr;  
  saddr.sin_port = htons(8787);

  if (connect(sockfd, (struct sockaddr*) &saddr, sizeof(saddr)) < 0) 
    return;

  char msg[1024];

  sprintf(msg, "{\"codec\":\"%s\", \"channels\":%d}\n", 
    avcodec_get_name(codecHandler->currentCodecID), 
    codecHandler->currentChannelCount);

  write(sockfd, msg, strlen(msg));
  
  close(sockfd); /* close the connection */
}

//--------------------------------------------------------------------------------------------------
void sendInfoToSocket(CodecHandler* codecHandler)
{
  pthread_t threadId;
  int err = pthread_create(&threadId, NULL, &sendInfoToSocketThread, codecHandler);
}

//--------------------------------------------------------------------------------------------------
void initContext() 
{
  if(debug_data) printf("initContext...\n");

  if(spdif_ctx) 
    avformat_close_input(&spdif_ctx);

	spdif_ctx = avformat_alloc_context();
	if (!spdif_ctx)
		errx(1, "cannot allocate S/PDIF context");

	alsa_buf = av_malloc(I_BUFFER_SIZE);
	if (!alsa_buf)
		errx(1, "cannot allocate input buffer");

	spdif_ctx->pb = avio_alloc_context(alsa_buf, I_BUFFER_SIZE, 0, &read_state, alsa_reader, NULL, NULL);

	if (!spdif_ctx->pb)
		errx(1, "cannot set up alsa reader");

	if (avformat_open_input(&spdif_ctx, "internal", spdif_fmt, NULL) != 0)
		errx(1, "cannot open S/PDIF input");

  if(debug_data) printf("initContext...ok\n");
}

//--------------------------------------------------------------------------------------------------
void closeOutDev()
{
  if (!out_dev) 
    return;
  
  snd_pcm_close(out_dev);
  out_dev = NULL;
}

//--------------------------------------------------------------------------------------------------
void closeInDev()
{
  if (!read_state.dev)
    return;
  
 	snd_pcm_close(read_state.dev);
  read_state.dev = NULL;
}

//--------------------------------------------------------------------------------------------------
void reinit()
{
  printf("reinit...\n");

  closeOutDev();
  closeInDev();
  CodecHandler_closeCodec(&codecHandler);
  CodecHandler_deinit(&codecHandler);

  // snd_pcm_drain(read_state.dev); // long delay !?

  read_state.dev = alsa_open(alsa_dev_name, 0);
  initContext();
  CodecHandler_init(&codecHandler);

  printf("reinit...ok\n");
	fflush(stdout);
}

//--------------------------------------------------------------------------------------------------
void reinit_input()
{
  printf("reinit input...\n");

  closeInDev();

  // snd_pcm_drain(read_state.dev); // long delay !?

  read_state.dev = alsa_open(alsa_dev_name, 0);
  initContext();

  printf("reinit input...ok\n");
	fflush(stdout);
}

//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
	char *out_dev_name = NULL, *out_dev_name_ch = NULL;
	int opt;
  double start = 0;

	for (opt = 0; (opt = getopt(argc, argv, "hi:o:vb:")) != -1;) {
		switch (opt) {
		case 'i':
			alsa_dev_name = optarg;
			break;
		case 'o':
			out_dev_name = optarg;
      out_dev_name_ch = strchr(out_dev_name, '#');
			break;
		case 'v':
			debug_data = 1;
			break;
    case 'b':
      out_dev_buffer_time = optarg;
      break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 0)
		usage();

	if (!alsa_dev_name)
  {
		fprintf(stderr, "please specify input device\n\n");
		usage();
	}

	av_register_all();
	avcodec_register_all();
	avdevice_register_all();
	ao_initialize();

  char *resamples = malloc(1*1024*1024);

  read_state.dev = alsa_open(alsa_dev_name, 0);

	spdif_fmt = av_find_input_format("spdif");
	if (!spdif_fmt)
		errx(1, "cannot find S/PDIF demux driver");

  initContext();

  AVPacket pkt = {.size = 0, .data = NULL};
  memset(&pkt, 0, sizeof(AVPacket));
  av_init_packet(&pkt);

  uint32_t howmuch = 0;
  
  CodecHandler_init(&codecHandler);

	printf("start loop\n");

	while(1) 
  {
    progCycles ++;

    if(debug_data)
      start = gettimeofday_ms();

		int ret = my_spdif_read_packet(spdif_ctx, &pkt, (uint8_t*)resamples, MAX_BURST_SIZE, &howmuch);

    if(ret == SPIF_DECODER_RETRY_REQUIRED)
      continue;

    if(ret == SPIF_DECODER_RESTART_REQUIRED)
    {
      // codec changed ... reinit system
      reinit();
      continue;
    }

    if(ret && ret != SPIF_DECODER_PCM)
      errx(1, "error: read packet");

    if(debug_data)
      printf("read_packet() bytes=%d in %.1lf ms\n", pkt.size, gettimeofday_ms() - start);

    if(ret == SPIF_DECODER_PCM)
    {
      CodecHandler_closeCodec(&codecHandler);
      codecHandler.currentChannelCount = 2;
      codecHandler.currentSampleRate = 48000;
      codecHandler.currentChannelLayout = AV_CH_LAYOUT_STEREO;
      howmuch = MAX_BURST_SIZE;
    }
    else
    {
      int newCodec = CodecHandler_loadCodec(&codecHandler, spdif_ctx);

      if( (ret = CodecHandler_decodeCodec(&codecHandler, &pkt, (uint8_t*)resamples, &howmuch)) == 1)
      {
        //channel count has changed
        closeOutDev();
      }

      if(!ret && !codecHandler.currentSampleRate) 
      {
        // decodeing did not set a sample rate > retry
        printf("no sample rate detected!\n");
        ret = SPIF_DECODER_RESTART_REQUIRED;
      }

      if(ret == SPIF_DECODER_RESTART_REQUIRED) 
      {
        // decodeing failed, restart
        reinit();
        continue;
      }

      if(newCodec)
        printf("Loaded codec %s channels:%d, channel-layout:%08x \n", avcodec_get_name(codecHandler.currentCodecID), codecHandler.currentChannelCount, codecHandler.currentChannelLayout);

      if(pkt.size != 0)
        printf("still some bytes left %d\n",pkt.size);
    }

    if (!out_dev) 
    {
      sendInfoToSocket(&codecHandler);

      if(out_dev_name_ch)
        *out_dev_name_ch = '0' + codecHandler.currentChannelCount;

      out_dev = alsa_open(out_dev_name, codecHandler.currentChannelCount);

      if (!out_dev)
        errx(1, "cannot open audio output, channels=%d, format=s16, rate=%d", codecHandler.currentChannelCount, codecHandler.currentSampleRate);


      outDelay = 0;

      // alsa_open() takes some time, flush input and restart with lowest possible latency
      reinit_input();
      memset(resamples, 0, howmuch);
    }

    if(debug_data)
      start = gettimeofday_ms();

    if(!alsa_write(resamples, howmuch))
      errx(1, "Could not play audio to output device");

    if(debug_data)
      printf("alsa_write() frames=%d ms=%.1f in %.1lf ms\n", howmuch / 2 / codecHandler.currentChannelCount, howmuch / 2 / codecHandler.currentChannelCount / 48.0, gettimeofday_ms() - start);

    av_packet_unref(&pkt); 
	}

	return (0);
}
