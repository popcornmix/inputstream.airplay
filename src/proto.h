/*
 *  Wire protocol between airplay-receiver (server) and inputstream.airplay (client).
 *
 *  The receiver owns the AirPlay session and hands over already-decrypted
 *  elementary streams: Annex-B H.264/HEVC access units and raw AAC/ALAC/PCM
 *  frames. Kodi does all decoding, so nothing here is codec specific beyond
 *  naming the codec and carrying its extradata.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef AIRPLAY_PROTO_H
#define AIRPLAY_PROTO_H

#include <stdint.h>

#define APX_MAGIC 0x31585041u /* "APX1" */

/*
 * Where the socket lives. Both ends resolve this the same way and neither
 * tells the other, so there is nothing to keep in step. special://temp rather
 * than the add-on profile because a unix socket path is limited to 104 bytes
 * on macOS, which a profile path can exceed on its own.
 */
#define APX_SOCKET_SPECIAL "special://temp/airplay.sock"

/*
 * Fallback for running the receiver by hand, outside Kodi, where nothing can
 * resolve special://. Overridden with AIRPLAY_SOCKET.
 */
#define APX_DEFAULT_SOCKET "/tmp/kodi-airplay.sock"

/*
 * Largest payload either end will accept. Real ones are far smaller -- a 4K
 * keyframe is a megabyte or so -- and the cap exists so a corrupt or hostile
 * length cannot be turned into an allocation.
 */
#define APX_MAX_PAYLOAD (8u * 1024u * 1024u)

enum apx_msg_type
{
  APX_MSG_STREAMINFO = 1, /* payload: struct apx_streaminfo            */
  APX_MSG_VIDEO = 2,      /* payload: Annex-B access unit              */
  APX_MSG_AUDIO = 3,      /* payload: one compressed audio frame       */
  APX_MSG_EOS = 4,        /* no payload; session finished              */
  APX_MSG_PROGRESS = 5,   /* payload: struct apx_progress              */
};

/*
 * Where the sender is within the track it is streaming. Only an audio session
 * has this: the sender reports it as RTP sample positions, converted here so
 * the add-on can hand Kodi a real elapsed time and duration.
 */
struct apx_progress
{
  uint32_t position_ms;
  uint32_t duration_ms;
};

enum apx_video_codec
{
  APX_VCODEC_NONE = 0,
  APX_VCODEC_H264 = 1,
  APX_VCODEC_HEVC = 2,
};

/* AirPlay "compression type" values as reported by audio_get_format(). */
enum apx_audio_ct
{
  APX_ACT_NONE = 0,
  APX_ACT_PCM = 1,
  APX_ACT_ALAC = 2,
  APX_ACT_AAC_LC = 4,
  APX_ACT_AAC_ELD = 8,
};

#define APX_FLAG_KEYFRAME 0x1u

/* All fields little-endian; both ends are the same machine. */
struct apx_hdr
{
  uint32_t magic;
  uint32_t type;
  uint32_t size; /* payload bytes following this header */
  uint32_t flags;
  uint64_t pts_ns; /* receiver-local clock, nanoseconds */
};

#define APX_MAX_EXTRADATA 64
/* SPS+PPS, or VPS+SPS+PPS for HEVC, in Annex-B form. */
#define APX_MAX_VIDEO_EXTRADATA 256

struct apx_streaminfo
{
  uint32_t video_codec; /* enum apx_video_codec */
  uint32_t audio_ct;    /* enum apx_audio_ct    */
  uint32_t width;
  uint32_t height;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t audio_extradata_size;
  uint8_t audio_extradata[APX_MAX_EXTRADATA];
  /* CVideoPlayerVideo refuses to open an H.264/HEVC codec whose stream has no
   * extradata, so the parameter sets are published here rather than left for
   * the demuxer to recover from the bitstream. */
  uint32_t video_extradata_size;
  uint8_t video_extradata[APX_MAX_VIDEO_EXTRADATA];
};

/*
 * AudioSpecificConfig blobs for the AirPlay audio formats, lifted from
 * UxPlay's renderers/audio_renderer.c where they are hard-coded for the
 * GStreamer decoders. Kodi's ffmpeg wants exactly the same bytes.
 */
#define APX_ASC_AAC_ELD                                                                            \
  {                                                                                                \
    0xf8, 0xe8, 0x50, 0x00                                                                         \
  }
#define APX_ASC_AAC_ELD_LEN 4

#define APX_ASC_AAC_LC                                                                             \
  {                                                                                                \
    0x12, 0x10                                                                                     \
  }
#define APX_ASC_AAC_LC_LEN 2

/* 36-byte ALAC magic cookie: 44100 Hz, 16 bit, 2 channels, 352 frames/packet. */
#define APX_ALAC_COOKIE                                                                            \
  {                                                                                                \
    0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x60, \
        0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  \
        0x00, 0x00, 0x00, 0xac, 0x44                                                               \
  }
#define APX_ALAC_COOKIE_LEN 36

#endif /* AIRPLAY_PROTO_H */
