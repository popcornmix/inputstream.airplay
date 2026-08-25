/*
 *  inputstream.airplay — feeds Kodi the elementary streams produced by
 *  airplay-receiver.
 *
 *  The add-on itself knows nothing about AirPlay: it reads framed Annex-B
 *  access units and compressed audio frames off a unix socket and republishes
 *  them as demux packets, so Kodi's own hardware decoders and renderer do the
 *  work. Deliberately it links nothing from UxPlay, which keeps the FairPlay
 *  code out of the Kodi process image.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "InputStreamAirPlay.h"

#include <cerrno>
#include <cstring>

#include <kodi/Filesystem.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
constexpr unsigned int STREAM_ID_VIDEO = 1;
constexpr unsigned int STREAM_ID_AUDIO = 2;

/* How long DemuxRead waits before handing Kodi an empty packet. Keeping this
 * short lets Kodi stay responsive while the sender is idle. */
constexpr int READ_POLL_MS = 100;

/* Give the receiver time to report the session format when we attach. */
constexpr int STREAMINFO_TIMEOUT_MS = 10000;
} // namespace

CInputStreamAirPlay::CInputStreamAirPlay(const kodi::addon::IInstanceInfo& instance)
  : kodi::addon::CInstanceInputStream(instance)
{
}

CInputStreamAirPlay::~CInputStreamAirPlay()
{
  Close();
}

bool CInputStreamAirPlay::SkipPayload(uint32_t size)
{
  if (!size)
    return true;

  /* In bounded chunks rather than one allocation the size of the message: the
   * caller has already rejected anything over APX_MAX_PAYLOAD, but there is no
   * reason to take a megabyte of scratch space to throw a megabyte away. */
  uint8_t scratch[8192];
  while (size)
  {
    const uint32_t chunk = size < sizeof(scratch) ? size : (uint32_t)sizeof(scratch);
    if (!ReadExact(scratch, chunk, 2000))
      return false;
    size -= chunk;
  }
  return true;
}

bool CInputStreamAirPlay::ReadExact(void* buf, size_t len, int timeoutMs)
{
  auto* p = static_cast<uint8_t*>(buf);
  while (len)
  {
    if (m_fd < 0)
      return false;

    struct pollfd pfd = {m_fd, POLLIN, 0};
    int rc = poll(&pfd, 1, timeoutMs);
    if (rc == 0)
      return false; /* timed out */
    if (rc < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }

    ssize_t n = recv(m_fd, p, len, 0);
    if (n <= 0)
    {
      if (n < 0 && errno == EINTR)
        continue;
      m_eof = true;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool CInputStreamAirPlay::Connect()
{
  m_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (m_fd < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "airplay: socket() failed: %s", std::strerror(errno));
    return false;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", m_socketPath.c_str());

  if (connect(m_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
  {
    close(m_fd);
    m_fd = -1;
    return false;
  }
  return true;
}

/*
 * The receiver restarts, or drops us if it cannot place a frame. Neither
 * should end playback while the AirPlay session itself is still running, so
 * reattach and let the receiver prime us with a fresh keyframe.
 */
bool CInputStreamAirPlay::TryReconnect()
{
  /* Reattaching during a teardown would fight the shutdown: Kodi aborts the
   * demuxer by closing our read side, which looks exactly like the receiver
   * going away. */
  if (m_aborted || m_sessionEnded || ++m_reconnectAttempts > 50)
    return false;

  if (m_fd >= 0)
  {
    close(m_fd);
    m_fd = -1;
  }
  m_eof = false;

  if (!Connect())
    return true; /* keep trying on later calls */

  kodi::Log(ADDON_LOG_INFO, "airplay: reattached to receiver (attempt %d)", m_reconnectAttempts);
  m_reconnectAttempts = 0;
  return true;
}

bool CInputStreamAirPlay::Open(const kodi::addon::InputstreamProperty& props)
{
  m_verbose = kodi::addon::GetSettingBoolean("debuglog", false);

  /*
   * The same rule the service uses, resolved independently rather than passed
   * across: putenv() in a running Kodi races anything else calling getenv(),
   * and a unix socket path is short enough -- 104 bytes on macOS -- that the
   * profile directory does not reliably fit. special://temp does.
   *
   * AIRPLAY_SOCKET stays as the override for running the receiver by hand.
   */
  m_socketPath = kodi::vfs::TranslateSpecialProtocol(APX_SOCKET_SPECIAL);
  const char* env = std::getenv("AIRPLAY_SOCKET");
  if (env && *env)
    m_socketPath = env;

  if (!Connect())
  {
    kodi::Log(ADDON_LOG_ERROR, "airplay: cannot reach the receiver at %s: %s", m_socketPath.c_str(),
              std::strerror(errno));
    return false;
  }

  kodi::Log(ADDON_LOG_INFO, "airplay: connected to receiver at %s", m_socketPath.c_str());

  /* The first message describes the session. Without it we cannot declare
   * streams, so wait for it rather than guessing. */
  const int deadlineSlices = STREAMINFO_TIMEOUT_MS / READ_POLL_MS;
  for (int i = 0; i < deadlineSlices && !m_haveInfo; ++i)
  {
    apx_hdr hdr{};
    if (!ReadExact(&hdr, sizeof(hdr), READ_POLL_MS))
    {
      if (m_eof)
        break;
      continue;
    }
    if (hdr.magic != APX_MAGIC || hdr.size > APX_MAX_PAYLOAD)
    {
      kodi::Log(ADDON_LOG_ERROR, "airplay: framing error (magic %08x, size %u), giving up",
                hdr.magic, hdr.size);
      break;
    }
    if (hdr.type == APX_MSG_STREAMINFO && hdr.size == sizeof(m_info))
    {
      if (!ReadExact(&m_info, sizeof(m_info), 2000))
        break;
      m_haveInfo = true;
      break;
    }
    /* Anything arriving before the stream info is not useful yet. */
    if (!SkipPayload(hdr.size))
      break;
  }

  if (!m_haveInfo)
  {
    kodi::Log(ADDON_LOG_ERROR, "airplay: receiver never reported a stream format");
    Close();
    return false;
  }

  m_hasAudio = m_info.audio_ct != APX_ACT_NONE;
  m_hasVideo = m_info.video_codec != APX_VCODEC_NONE;
  if (m_hasVideo)
    kodi::Log(ADDON_LOG_INFO, "airplay: mirroring, %s %ux%u, audio ct=%u, video extradata %u bytes",
              VideoCodecName().c_str(), m_info.width, m_info.height, m_info.audio_ct,
              m_info.video_extradata_size);
  else
    kodi::Log(ADDON_LOG_INFO, "airplay: audio only, ct=%u, %u Hz, %u channels", m_info.audio_ct,
              m_info.sample_rate, m_info.channels);

  if (!m_hasVideo && !m_hasAudio)
  {
    kodi::Log(ADDON_LOG_ERROR, "airplay: session has neither video nor audio");
    Close();
    return false;
  }
  return true;
}

void CInputStreamAirPlay::Close()
{
  if (m_fd >= 0)
  {
    shutdown(m_fd, SHUT_RDWR);
    close(m_fd);
    m_fd = -1;
  }
  m_haveInfo = false;
  m_hasAudio = false;
  m_hasVideo = false;
  m_eof = false;
  m_sessionEnded = false;
  m_aborted = false;
  m_reconnectAttempts = 0;
}

std::string CInputStreamAirPlay::VideoCodecName() const
{
  return m_info.video_codec == APX_VCODEC_HEVC ? "hevc" : "h264";
}

void CInputStreamAirPlay::GetCapabilities(kodi::addon::InputstreamCapabilities& capabilities)
{
  /*
   * Mirroring is worth keeping close to the phone rather than buffered: the
   * point of it is watching something being interacted with. LOW_LATENCY_LIVE
   * asks the player to start at the newest frame it has rather than the first
   * one it decoded, which otherwise leaves it permanently behind by however
   * long it took to start. Ignored by a Kodi that predates the flag.
   */
  /*
   * IDISPLAYTIME is what gives a streamed track an elapsed time and a
   * duration. Kodi only uses it while GetTotalTime() is above zero, so
   * declaring it costs a mirroring session nothing.
   */
  uint32_t mask = INPUTSTREAM_SUPPORTS_IDEMUX | INPUTSTREAM_SUPPORTS_IDISPLAYTIME;

  /*
   * Off by default. It trades away the buffer the player would otherwise
   * start with for being closer to the sender, and that buffer is what
   * absorbs a source that stutters.
   */
#ifdef HAVE_INPUTSTREAM_LOW_LATENCY_LIVE
  if (kodi::addon::GetSettingBoolean("lowlatency", false))
    mask |= INPUTSTREAM_LOW_LATENCY_LIVE;
#endif

  capabilities.SetMask(mask);
}

bool CInputStreamAirPlay::GetStreamIds(std::vector<unsigned int>& ids)
{
  ids.clear();
  if (!m_haveInfo)
    return false;
  if (m_hasVideo)
    ids.emplace_back(STREAM_ID_VIDEO);
  if (m_hasAudio)
    ids.emplace_back(STREAM_ID_AUDIO);
  return !ids.empty();
}

bool CInputStreamAirPlay::GetStream(int streamid, kodi::addon::InputstreamInfo& stream)
{
  if (m_hasVideo && streamid == static_cast<int>(STREAM_ID_VIDEO))
  {
    stream.SetStreamType(INPUTSTREAM_TYPE_VIDEO);
    stream.SetFeatures(0);
    stream.SetFlags(0);
    stream.SetCodecName(VideoCodecName());
    stream.SetPhysicalIndex(STREAM_ID_VIDEO);
    stream.SetWidth(m_info.width ? m_info.width : 1920);
    stream.SetHeight(m_info.height ? m_info.height : 1080);
    /* The sender drives the frame rate; declaring 60 keeps Kodi's initial
     * timing sane and CDVDDemuxClient corrects it from the bitstream. */
    stream.SetFpsScale(1);
    stream.SetFpsRate(60);
    /* Annex-B parameter sets. CVideoPlayerVideo::OpenStream rejects H.264 and
     * HEVC streams whose extradata is empty, and the demuxer's own bitstream
     * parsing does not survive the stream updates this feed generates. */
    if (m_info.video_extradata_size)
      stream.SetExtraData(m_info.video_extradata, m_info.video_extradata_size);
    return true;
  }

  if (m_hasAudio && streamid == static_cast<int>(STREAM_ID_AUDIO))
  {
    stream.SetStreamType(INPUTSTREAM_TYPE_AUDIO);
    stream.SetFeatures(0);
    stream.SetFlags(0);
    /* AAC-ELD (mirroring) and AAC-LC both decode through ffmpeg's "aac"; the
     * AudioSpecificConfig in extradata is what selects the object type. */
    const char* codec = "aac";
    if (m_info.audio_ct == APX_ACT_ALAC)
      codec = "alac";
    else if (m_info.audio_ct == APX_ACT_PCM)
      codec = "pcm_s16le";
    stream.SetCodecName(codec);
    stream.SetPhysicalIndex(STREAM_ID_AUDIO);
    stream.SetSampleRate(m_info.sample_rate ? m_info.sample_rate : 44100);
    stream.SetChannels(m_info.channels ? m_info.channels : 2);
    stream.SetBitsPerSample(16);
    if (m_info.audio_extradata_size)
      stream.SetExtraData(m_info.audio_extradata, m_info.audio_extradata_size);
    return true;
  }

  return false;
}

bool CInputStreamAirPlay::OpenStream(int streamid)
{
  return true;
}

DEMUX_PACKET* CInputStreamAirPlay::DemuxRead()
{
  if (m_aborted || m_fd < 0)
    return nullptr;

  apx_hdr hdr{};
  if (!ReadExact(&hdr, sizeof(hdr), READ_POLL_MS))
  {
    if (m_eof && !TryReconnect())
      return nullptr; /* session over, or the receiver is not coming back */
    return AllocateDemuxPacket(0); /* nothing yet, ask again */
  }

  if (hdr.magic != APX_MAGIC || hdr.size > APX_MAX_PAYLOAD)
  {
    kodi::Log(ADDON_LOG_ERROR, "airplay: lost framing (magic %08x, size %u), ending stream",
              hdr.magic, hdr.size);
    return nullptr;
  }

  if (hdr.type == APX_MSG_EOS)
  {
    kodi::Log(ADDON_LOG_INFO, "airplay: session ended");
    m_sessionEnded = true;
    m_positionMs = 0;
    m_durationMs = 0;
    m_positionAnchorMs = 0;
    m_lastAudioMs = 0;
    return nullptr;
  }

  if (hdr.type == APX_MSG_PROGRESS)
  {
    apx_progress progress{};
    if (hdr.size != sizeof(progress))
    {
      /* Not what this build expects. Take it off the socket anyway: leaving it
       * there turns the next header read into payload bytes, and the stream
       * ends as a framing error rather than one ignored message. */
      SkipPayload(hdr.size);
      return AllocateDemuxPacket(0);
    }
    if (ReadExact(&progress, sizeof(progress), 2000))
    {
      m_positionMs = progress.position_ms;
      m_durationMs = progress.duration_ms;
      m_positionAnchorMs = m_lastAudioMs.load();
    }
    return AllocateDemuxPacket(0);
  }

  if (hdr.type == APX_MSG_STREAMINFO)
  {
    if (hdr.size != sizeof(m_info))
    {
      SkipPayload(hdr.size); /* see the note in the progress branch */
      return AllocateDemuxPacket(0);
    }
    /* A mid-session format change: pick up the new values and tell Kodi. */
    if (ReadExact(&m_info, sizeof(m_info), 2000))
    {
      const bool hadAudio = m_hasAudio;
      m_hasAudio = m_info.audio_ct != APX_ACT_NONE;
      m_hasVideo = m_info.video_codec != APX_VCODEC_NONE;
      if (m_hasAudio != hadAudio)
        kodi::Log(ADDON_LOG_INFO, "airplay: audio track %s (ct=%u)",
                  m_hasAudio ? "appeared" : "went away", m_info.audio_ct);
      DEMUX_PACKET* pkt = AllocateDemuxPacket(0);
      pkt->iStreamId = DEMUX_SPECIALID_STREAMCHANGE;
      return pkt;
    }
    return AllocateDemuxPacket(0);
  }

  if (hdr.type != APX_MSG_VIDEO && hdr.type != APX_MSG_AUDIO)
  {
    SkipPayload(hdr.size);
    return AllocateDemuxPacket(0);
  }

  DEMUX_PACKET* pkt = AllocateDemuxPacket(static_cast<int>(hdr.size));
  if (!pkt)
    return nullptr;

  if (hdr.size && !ReadExact(pkt->pData, hdr.size, 5000))
  {
    FreeDemuxPacket(pkt);
    return nullptr;
  }

  ++m_packetsRead;
  if (m_verbose && (m_packetsRead <= 5 || (m_packetsRead % 120) == 0))
    kodi::Log(ADDON_LOG_INFO, "airplay: packet %llu, %s, %u bytes, pts %.1f ms",
              static_cast<unsigned long long>(m_packetsRead),
              hdr.type == APX_MSG_VIDEO ? "video" : "audio", hdr.size,
              static_cast<double>(hdr.pts_ns) / 1e6);

  pkt->iSize = static_cast<int>(hdr.size);
  pkt->iStreamId = (hdr.type == APX_MSG_VIDEO) ? static_cast<int>(STREAM_ID_VIDEO)
                                               : static_cast<int>(STREAM_ID_AUDIO);

  /* Receiver timestamps are nanoseconds on its local clock; Kodi wants
   * microseconds. Both streams share the clock, so A/V stay aligned. */
  const double pts = static_cast<double>(hdr.pts_ns) / 1000.0;
  pkt->pts = pts;
  pkt->dts = (hdr.type == APX_MSG_VIDEO) ? STREAM_NOPTS_VALUE : pts;
  pkt->duration = 0;

  /* How far the audio has got, used to carry the reported track position
   * forward between the sender's updates. */
  if (hdr.type == APX_MSG_AUDIO)
    m_lastAudioMs = static_cast<unsigned int>(hdr.pts_ns / 1000000ull);

  return pkt;
}

int CInputStreamAirPlay::GetTotalTime()
{
  return static_cast<int>(m_durationMs.load());
}

int CInputStreamAirPlay::GetTime()
{
  /*
   * The sender reports where it is in the track only now and then, so this
   * carries the position forward using the audio it has sent since. It also
   * has to keep moving: Kodi only re-anchors this against the stream clock
   * when the value changes, and a position stuck at zero leaves it deriving
   * the elapsed time from the raw timestamps, which run negative.
   */
  const unsigned int anchor = m_positionAnchorMs.load();
  const unsigned int now = m_lastAudioMs.load();
  unsigned int position = m_positionMs.load();
  if (now > anchor)
    position += now - anchor;

  const unsigned int duration = m_durationMs.load();
  if (duration && position > duration)
    position = duration;
  return static_cast<int>(position);
}

bool CInputStreamAirPlay::IsRealTimeStream()
{
  return true;
}

void CInputStreamAirPlay::DemuxReset()
{
}

void CInputStreamAirPlay::DemuxAbort()
{
  /* Playback is being torn down. Record that before unblocking the reader, so
   * the resulting end-of-file is treated as the end of the stream rather than
   * a receiver that needs reattaching. */
  m_aborted = true;
  if (m_fd >= 0)
    shutdown(m_fd, SHUT_RD);
}

void CInputStreamAirPlay::DemuxFlush()
{
}

/* ------------------------------------------------------------- addon glue */

class ATTR_DLL_LOCAL CAirPlayAddon : public kodi::addon::CAddonBase
{
public:
  CAirPlayAddon() = default;

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    if (!instance.IsType(ADDON_INSTANCE_INPUTSTREAM))
      return ADDON_STATUS_NOT_IMPLEMENTED;

    hdl = new CInputStreamAirPlay(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CAirPlayAddon)
