#pragma once

#include <sys/ioctl.h>

#define AFMT_QUERY  0x00000000
#define AFMT_U8     0x00000008
#define AFMT_S16_LE 0x00000010

#define SNDCTL_DSP_RESET      0x00005000
#define SNDCTL_DSP_SYNC       0x00005001
#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_STEREO     0xC0045003
#define SNDCTL_DSP_GETBLKSIZE 0xC0045004
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define SNDCTL_DSP_GETFMTS    0x8004500B
#define SNDCTL_DSP_GETOSPACE  0x8010500C

typedef struct audio_buf_info {
    int fragments;
    int fragstotal;
    int fragsize;
    int bytes;
} audio_buf_info;
