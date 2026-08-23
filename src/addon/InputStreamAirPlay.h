/*
 *  inputstream.airplay — see InputStreamAirPlay.cpp.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <kodi/AddonBase.h>
#include <kodi/addon-instance/Inputstream.h>

#include "../proto.h"

class ATTR_DLL_LOCAL CInputStreamAirPlay : public kodi::addon::CInstanceInputStream
{
public:
  explicit CInputStreamAirPlay(const kodi::addon::IInstanceInfo& instance);
  ~CInputStreamAirPlay() override;

  bool Open(const kodi::addon::InputstreamProperty& props) override;
  void Close() override;
  void GetCapabilities(kodi::addon::InputstreamCapabilities& capabilities) override;

  bool GetStreamIds(std::vector<unsigned int>& ids) override;
  bool GetStream(int streamid, kodi::addon::InputstreamInfo& stream) override;
  bool OpenStream(int streamid) override;

  DEMUX_PACKET* DemuxRead() override;
  void DemuxReset() override;
  void DemuxAbort() override;
  void DemuxFlush() override;

  bool IsRealTimeStream() override;

  int GetTotalTime() override;
  int GetTime() override;

private:
  /* Reads exactly len bytes, or returns false on timeout/error. */
  bool ReadExact(void* buf, size_t len, int timeoutMs);
  bool Connect();
  bool TryReconnect();
  std::string VideoCodecName() const;

  int m_fd{-1};
  bool m_haveInfo{false};
  bool m_hasAudio{false};
  bool m_hasVideo{false};
  bool m_eof{false};
  apx_streaminfo m_info{};
  unsigned long long m_packetsRead{0};
  bool m_sessionEnded{false};
  /* Set from Kodi's thread when playback is being torn down, read on the
   * demux thread. */
  std::atomic<bool> m_aborted{false};
  /* Where the sender is in the track it is streaming, milliseconds. Only an
   * audio session reports it; zero means unknown, which is how Kodi is told
   * there is no duration to show. Written on the demux thread, read from
   * Kodi's. */
  std::atomic<unsigned int> m_positionMs{0};
  std::atomic<unsigned int> m_durationMs{0};
  /* Presentation time of the audio packet the position was current for, so
   * the time between reports can be filled in. */
  std::atomic<unsigned int> m_positionAnchorMs{0};
  std::atomic<unsigned int> m_lastAudioMs{0};
  int m_reconnectAttempts{0};
  /* Per-packet logging, off unless the receiver was asked for extra logging. */
  const bool m_verbose{std::getenv("AIRPLAY_DEBUG") != nullptr};
  std::string m_socketPath;
};
