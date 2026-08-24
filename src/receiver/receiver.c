/*
 *  airplay-receiver — runs UxPlay's AirPlay protocol library and hands the
 *  decrypted elementary streams to Kodi over a unix socket.
 *
 *  UxPlay's lib/ is used unmodified. Everything GStreamer did in UxPlay
 *  (decode, sinks, sync) is Kodi's job here, so this process never touches
 *  a codec: it forwards Annex-B access units and raw audio frames.
 *
 *  Session events are written to stdout ("EVENT PLAY"/"EVENT STOP") for the
 *  Python service that spawned us; it starts playback, and inputstream.airplay
 *  then connects back to the socket here.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Darwin has no MSG_NOSIGNAL; SIGPIPE is ignored at startup instead. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>

#include "dnssd.h"
#include "raop.h"
#include "stream.h"

#include "../proto.h"

/*
 * How long to wait for room before giving up on a frame.
 *
 * This is deliberately short. Kodi stops reading for about a second whenever
 * it rebuilds a decoder, and anything buffered during that window is stale by
 * the time it is decoded. For a live mirror it is far better to discard those
 * frames and resync from a recent keyframe than to play catch-up for the rest
 * of the session.
 */
#define APX_WRITE_WAIT_MS 150

/*
 * Priming sends a whole buffered run in one go, so it needs more room than a
 * single live frame. Worth waiting for: the alternative is dropping the
 * client and starting the run again from scratch.
 */
#define APX_PRIME_WRITE_WAIT_MS 1000
/* Once a message is part-written the framing is committed, so finishing it
 * gets a longer grace period than starting one. */
#define APX_WRITE_FINISH_MS 3000

#define LOGI(...)                                                                                  \
  do                                                                                               \
  {                                                                                                \
    fprintf(stderr, "[airplay-receiver] " __VA_ARGS__);                                            \
    fprintf(stderr, "\n");                                                                         \
    fflush(stderr);                                                                                \
  } while (0)

/* ------------------------------------------------------------------ state */

static raop_t* g_raop;
static dnssd_t* g_dnssd;
static volatile sig_atomic_t g_running = 1;

/*
 * What the sender is doing right now. Behaviour differs in three ways --
 * whether a keyframe is needed before video can be forwarded, whether audio
 * stands alone, and whether a RAOP teardown means the session is over -- and
 * all three key off this single value. It replaces a set of overlapping
 * booleans whose combinations were the source of most of the transition bugs
 * between one mode and the next.
 */
typedef enum
{
  MODE_IDLE, /* no session, or nothing being received yet */
  MODE_MIRROR, /* screen mirroring: the sender encodes and sends frames */
  MODE_AUDIO, /* music or podcast: RAOP audio with no video */
  MODE_VIDEO, /* the sender nominates a URL and Kodi fetches it itself */
} session_mode_t;

static const char* mode_name(session_mode_t mode)
{
  switch (mode)
  {
    case MODE_MIRROR:
      return "mirroring";
    case MODE_AUDIO:
      return "audio";
    case MODE_VIDEO:
      return "video streaming";
    default:
      return "idle";
  }
}

/* Set when the library says the mirror stream is stopping in order to hand
 * over to video streaming, so its stop is not mistaken for the session
 * ending. */
static bool g_mirror_to_video;

/* The last track announced, so repeats of it can be ignored. */
static char g_meta_previous[800];

/* So waiting for a keyframe is said once per wait rather than once per run. */
static bool g_said_waiting_keyframe;

/*
 * When mirroring was left with no way to start a player: no client attached
 * and no keyframe buffered to start one from. Zero when that is not the case.
 */
static uint64_t g_unstartable_since_ns;

/*
 * When the mirror stream stopped without the library telling us a video
 * handoff was coming. Zero when no stop is pending.
 */
static uint64_t g_mirror_stop_pending_ns;

/*
 * Whether Kodi has been seen actually playing the video the sender handed
 * over. Once it has, it no longer being played means the video is over, and
 * the start-up grace below does not apply -- that grace is only for the
 * window before Kodi has got going.
 */
static bool g_video_seen_playing;

/*
 * How long to hold that stop before acting on it.
 *
 * A sender switching from mirroring to streaming a video usually says so
 * first, and that is handled directly. When it does not -- which is what a
 * second and third attempt at the same video look like -- the stop arrives on
 * its own and the video request follows a few hundred milliseconds later.
 * Ending the session in that gap tears down the player Kodi is about to be
 * asked to reuse, and the sender gives up on the video and falls back to
 * mirroring, which is the loop this avoids.
 */
#define APX_MIRROR_STOP_GRACE_NS (800ull * 1000000ull)

/*
 * How long to sit in that state before ending the session. Short, because
 * nothing is expected to change: the sender sends one keyframe when mirroring
 * starts and, in practice, never another.
 */
#define APX_UNSTARTABLE_GRACE_NS (3ull * 1000000000ull)

/*
 * When the sender handed us a video, so its first polls can be answered with
 * "still loading" rather than another stream's numbers.
 */
static uint64_t g_video_start_ns;

/*
 * How long Kodi is given to have the video playing before we admit defeat.
 * Switching player is not instant -- the previous stream has to be torn down
 * first -- and the sender starts polling within about 100ms of handing over.
 */
#define APX_VIDEO_START_GRACE_NS (10ull * 1000000000ull)

/*
 * The session state, and g_mirror_to_video above it, are written from
 * whichever UxPlay thread happens to notice the change -- httpd, mirror and
 * RTP all do -- and read from the others. Every access is under g_lock; the
 * _locked suffix on the helpers below is a reminder of which side of it they
 * belong on.
 */
static struct
{
  session_mode_t mode;
  bool player_open; /* Kodi has been asked to play */
  bool notify_video_ended; /* the sender still needs telling its video is done */
} g_session;

/* Every mode change is logged: which mode the sender is in, and when it
 * changes, is the single most useful thing to know when a transition
 * misbehaves. */
static void session_set_mode_locked(session_mode_t mode)
{
  if (g_session.mode == mode)
    return;
  LOGI("session: %s -> %s", mode_name(g_session.mode), mode_name(mode));
  g_session.mode = mode;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_sent_streaminfo;
static bool g_sent_params; /* parameter sets pushed to this client yet? */

static struct apx_streaminfo g_info;
static uint64_t g_pts_base; /* first pts seen, so Kodi starts near zero */
static bool g_have_base;

/* Cached SPS/PPS (H.264) or VPS/SPS/PPS (HEVC), in Annex-B form, so a client
 * that attaches mid-session can configure its decoder without waiting for the
 * sender to resend configuration. */
static uint8_t g_params[4096];
static size_t g_params_len;

/* Most recent IDR access unit. A client attaching mid-session (which is the
 * normal case, since Kodi only opens the stream after we ask it to) would
 * otherwise start at a non-keyframe and the decoder could never produce a
 * picture until the sender happened to emit another IDR. */
/*
 * Frames held from the most recent keyframe onwards. Replaying just the
 * keyframe and then resuming live skips every delta in between -- which are
 * precisely the references the live frames are built on -- so the picture
 * breaks up and the timeline gains a hole as long as the keyframe is old.
 * Keeping the whole run and flushing it intact avoids both.
 */
#define APX_GOP_MAX_FRAMES 240
#define APX_GOP_MAX_BYTES (6 * 1024 * 1024)

struct gop_frame
{
  uint8_t* data;
  size_t len;
  uint64_t ntp;
};

static struct gop_frame g_gop[APX_GOP_MAX_FRAMES];
static size_t g_gop_count;
static size_t g_gop_bytes;
static bool g_primed; /* has this client been given a keyframe yet? */

/*
 * Set once this client has been primed at least once. Audio waits for the
 * first picture, but not for every later resync: g_primed is cleared whenever
 * the consumer stalls, and gating audio on that muted it until the next
 * keyframe -- seconds of silence, and a badly out of sync clock afterwards.
 */
static bool g_video_started;

/*
 * Inside the priming burst, where a skipped message leaves a hole in the run
 * and makes everything after the keyframe undecodable.
 */
static bool g_priming;

/* When audio last arrived, so a session that never delivers can be spotted. */
static uint64_t g_last_audio_ns;

/*
 * A sender can negotiate an audio session and then send nothing at all -- it
 * has picked the receiver but is not playing. Kodi opens the stream and sits
 * there with the clock running on an empty stream, which looks exactly like
 * playback that has got stuck. Track whether a session ever delivered
 * anything, so one that never starts can be closed again.
 */
#define APX_AUDIO_NOSTART_NS (10ull * 1000000000ull)
static uint64_t g_audio_session_ns;  /* when the audio format was negotiated */
static uint64_t g_audio_session_frames;


/* diagnostics */
static uint64_t g_frames_in;   /* handed to us by lib/ */
static uint64_t g_frames_out;  /* forwarded to the add-on */
static uint64_t g_audio_out;   /* audio frames forwarded */
static uint64_t g_audio_in;    /* audio frames handed to us by lib/ */
static uint64_t g_last_video_ntp;
static uint64_t g_dropped_msgs; /* guarded by g_out_lock */

/* A session is either screen mirroring or plain audio streaming. The sender
 * tells us which via usingScreen on the audio SETUP. Audio-only sessions have
 * no video stream at all, so they must not wait for a keyframe. */

static bool g_hls_enabled;

/* Client-access password, empty for none. Held for the life of the process
 * because the library asks for it on every unauthenticated request. */
static char g_password[128];

/* Whether the sender's volume slider is allowed to move Kodi's volume. */
static bool g_volume_control;

/* When a real frame was last handed over, so a screen that has stopped
 * changing can be told apart from a source that has died. */
static uint64_t g_last_video_send_ns;

/*
 * Kodi treats an empty video queue on a real time stream as the source having
 * failed: it starts buffering, and playback stalls. Mirroring legitimately
 * sends nothing while the screen is still, so keep a discardable unit flowing
 * to say the stream is alive without touching the picture. It has to be well
 * inside the frametime * 10 that CVideoPlayerVideo waits before declaring a
 * stillframe -- 167ms at the 60fps this stream declares.
 *
 * A Kodi carrying the patch that tolerates such a stall does not need this,
 * but one without it does, so it stays.
 */
#define APX_KEEPALIVE_AFTER_NS (100ull * 1000000ull)
#define APX_WATCHDOG_TICK_US 50000
/* How long to sleep between checks when there is no session at all. */
#define APX_WATCHDOG_IDLE_US 500000

/*
 * Whether a mirroring session may carry sound. Mirror audio only flows while
 * the phone is making a noise, so its queue empties in the gaps -- and Kodi
 * reads a stalled audio stream on a live source as a fault and rebuffers,
 * which costs seconds of latency that a live stream never wins back. Turning
 * it off leaves video alone, which the keepalive already covers.
 */
static bool g_mirror_audio = true;

/*
 * Whether screen mirroring is offered at all. A current iPhone will not list
 * a receiver for photos unless this is advertised, so turning it off is a way
 * to see what a sender falls back to -- which, for photos, is nothing.
 */
static bool g_offer_mirroring = true;

/*
 * Pairing. With this on the sender is shown a four digit code the first time
 * it connects, and once it has been entered the device is remembered and is
 * not asked again -- which is how an Apple TV behaves. The library only
 * enforces the remembered list when a PIN is in use, so the two go together.
 */
/* Extra logging, for working out why a session is not behaving. */
static bool g_verbose;

static bool g_require_pairing;
static char g_registry_path[256];

/*
 * Where cover art is written. The add-on's profile, which the service passes
 * in; /run is root's on an ordinary distribution. Falls back to the socket's
 * directory when the receiver is run by hand.
 */
static char g_art_dir[192];
static uint64_t g_last_play_request_ns;

/* Playback is driven by the Python service, not from here: Kodi's JSON-RPC
 * Player.Open runs a VFS existence check that rejects "airplay://", and only
 * the Python API can attach the inputstream listitem property. Events are
 * therefore written to stdout, which the service reads from the pipe. */
static pthread_mutex_t g_event_lock = PTHREAD_MUTEX_INITIALIZER;


/* ---------------------------------------------------------------- helpers */

static void send_streaminfo_locked(void); /* defined with the raop callbacks */

/* The library timestamps packets against CLOCK_REALTIME, so anything compared
 * with ntp_time_local has to read the same clock. */
static uint64_t now_realtime_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * Write everything or fail.
 *
 * Both of these block, which is why they run on the writer thread and nowhere
 * else. Wait only briefly for room; the caller drops the message instead,
 * because for a live mirror a frame that arrives late is worth less than the
 * one behind it.
 */
static bool wait_writable(int fd, int timeout_ms)
{
  for (;;)
  {
    struct pollfd pfd = {fd, POLLOUT, 0};
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc > 0)
      return true;
    if (rc < 0 && errno == EINTR)
      continue;
    return false;
  }
}

static bool write_all(int fd, const void* buf, size_t len)
{
  const uint8_t* p = buf;
  while (len)
  {
    if (!wait_writable(fd, APX_WRITE_FINISH_MS))
      return false;

    ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n <= 0)
    {
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      return false;
    }
    p += n;
    len -= (size_t)n;
  }
  return true;
}

/*
 * Everything the client is sent goes through one writer thread.
 *
 * Every message is produced on a thread UxPlay wants back promptly: the
 * mirror thread reading from the phone, the RTP thread, and the httpd thread
 * answering a sender that has its own timeouts. Writing from those threads
 * meant waiting for a busy Kodi while holding g_lock, so one slow consumer
 * stalled volume, progress and format negotiation along with the picture.
 * Now a slow consumer only backs up this queue.
 *
 * The socket has a single owner. Only the writer thread closes it, and a new
 * client arrives as an item in the queue rather than through a second lock --
 * otherwise a drop on one thread could close the descriptor another was in
 * the middle of send()ing to, and the fd number would already have been
 * handed to something else by the time it noticed.
 */
enum out_kind
{
  OUT_MESSAGE,
  OUT_NEW_CLIENT,
};

struct out_msg
{
  struct out_msg* next;
  enum out_kind kind;
  int fd; /* OUT_NEW_CLIENT only */
  uint32_t gen; /* OUT_NEW_CLIENT only */
  uint64_t queued_ns;
  bool priming;
  struct apx_hdr hdr;
  uint32_t size;
  uint8_t payload[];
};

/*
 * How much may be waiting. The live limit is small on purpose: it is what
 * bounds how far behind the live edge a struggling consumer can put us, and
 * it takes over the job the old per-message write timeout did. Priming is
 * allowed the whole queue, because a run replayed with a hole in it is worse
 * than useless -- the frames after the hole reference what was lost.
 */
#define APX_OUT_LIVE_MAX_MSGS 24
#define APX_OUT_LIVE_MAX_BYTES (2 * 1024 * 1024)
#define APX_OUT_MAX_MSGS (APX_GOP_MAX_FRAMES + 32)
#define APX_OUT_MAX_BYTES (APX_GOP_MAX_BYTES + APX_OUT_LIVE_MAX_BYTES)

/*
 * A live video frame the writer only reaches this long after it was produced
 * is thrown away rather than written. Once a stall clears, what is queued is
 * history: showing it puts the session permanently that far behind, and the
 * next frames repair the picture anyway.
 */
#define APX_OUT_STALE_NS (200ull * 1000000ull)

static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_out_cv = PTHREAD_COND_INITIALIZER;
static struct out_msg* g_out_head;
static struct out_msg* g_out_tail;
static size_t g_out_count;
static size_t g_out_bytes;

/* Whether a client is attached, for the callbacks that only need to know
 * that much. The descriptor itself belongs to the writer thread. */
static bool g_client_connected;

/*
 * Bumped for every client. The writer carries the generation of the socket it
 * is writing to, so a failure on one that has already been replaced cannot
 * disown its replacement.
 */
static uint32_t g_client_gen;

/* Caller holds g_out_lock. */
static void out_push_locked(struct out_msg* msg)
{
  msg->next = NULL;
  if (g_out_tail)
    g_out_tail->next = msg;
  else
    g_out_head = msg;
  g_out_tail = msg;
  g_out_count++;
  g_out_bytes += msg->size;
  pthread_cond_signal(&g_out_cv);
}

/* Caller holds g_out_lock. */
static void out_clear_locked(void)
{
  struct out_msg* msg = g_out_head;
  while (msg)
  {
    struct out_msg* next = msg->next;
    if (msg->kind == OUT_NEW_CLIENT && msg->fd >= 0)
      close(msg->fd);
    free(msg);
    msg = next;
  }
  g_out_head = g_out_tail = NULL;
  g_out_count = 0;
  g_out_bytes = 0;
}

/*
 * Forget everything derived from the client that just went away. The
 * descriptor is not touched here; the writer thread owns it.
 *
 * Caller holds g_lock.
 */
static void reset_client_state_locked(void)
{
  g_client_connected = false;
  g_sent_streaminfo = false;
  g_sent_params = false;
  g_primed = false;
  g_video_started = false;
  /* Kodi may simply have been stopped while the sender keeps streaming, so
   * allow playback to be offered again rather than latching it off for the
   * rest of the session. */
  g_session.player_open = false;
}

/*
 * Hand a message to the writer. Returns false if there is nobody to send it
 * to, or if the queue is too full to take it.
 *
 * Caller holds g_lock.
 */
static bool send_msg_locked(uint32_t type, uint32_t flags, uint64_t pts_ns, const void* payload,
                            uint32_t size)
{
  if (!g_client_connected)
    return false;

  const bool priming = g_priming;

  pthread_mutex_lock(&g_out_lock);

  /*
   * Only live video is held to the tight limit. It is the bulk of what goes
   * through here and the only thing worth dropping, whereas audio frames are
   * small and dropping them is audible -- letting a video stall squeeze the
   * audio out would turn a moment of visual artefacts into a dropout.
   */
  const bool bulk = type == APX_MSG_VIDEO && !priming;
  const size_t max_msgs = bulk ? APX_OUT_LIVE_MAX_MSGS : APX_OUT_MAX_MSGS;
  const size_t max_bytes = bulk ? APX_OUT_LIVE_MAX_BYTES : APX_OUT_MAX_BYTES;
  if (g_out_count >= max_msgs || g_out_bytes + size > max_bytes)
  {
    /*
     * Deliberately does not give up on being primed. Dropping a frame leaves
     * the decoder referencing something it never got, so the picture is wrong
     * until the next keyframe -- but waiting for one is worse: a phone only
     * sends a keyframe every few minutes while mirroring, and until it does,
     * every frame is swallowed and the picture stops entirely. Visible
     * artefacts that heal themselves beat a freeze that does not.
     */
    const unsigned long long dropped = ++g_dropped_msgs;
    pthread_mutex_unlock(&g_out_lock);
    if ((dropped % 50) == 1)
      LOGI("consumer is behind, skipped %llu messages", dropped);
    return false;
  }

  struct out_msg* msg = malloc(sizeof(*msg) + size);
  if (!msg)
  {
    pthread_mutex_unlock(&g_out_lock);
    return false;
  }

  msg->kind = OUT_MESSAGE;
  msg->fd = -1;
  msg->queued_ns = now_ns();
  msg->priming = priming;
  msg->hdr.magic = APX_MAGIC;
  msg->hdr.type = type;
  msg->hdr.size = size;
  msg->hdr.flags = flags;
  msg->hdr.pts_ns = pts_ns;
  msg->size = size;
  if (size)
    memcpy(msg->payload, payload, size);

  out_push_locked(msg);
  pthread_mutex_unlock(&g_out_lock);
  return true;
}

/* Count a message the consumer never got, and say so occasionally. */
static void note_dropped(void)
{
  pthread_mutex_lock(&g_out_lock);
  const unsigned long long dropped = ++g_dropped_msgs;
  pthread_mutex_unlock(&g_out_lock);
  if ((dropped % 50) == 1)
    LOGI("consumer is behind, skipped %llu messages", dropped);
}

/*
 * The only thread that touches the client socket, which is why nothing else
 * has to be locked against it.
 */
static void* writer_thread(void* arg)
{
  int fd = -1;
  uint32_t gen = 0;

  while (g_running)
  {
    pthread_mutex_lock(&g_out_lock);
    while (!g_out_head && g_running)
    {
      /* Timed, so shutting down is noticed without needing to be woken. */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 200000000;
      if (ts.tv_nsec >= 1000000000)
      {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&g_out_cv, &g_out_lock, &ts);
    }
    struct out_msg* msg = g_out_head;
    if (msg)
    {
      g_out_head = msg->next;
      if (!g_out_head)
        g_out_tail = NULL;
      g_out_count--;
      g_out_bytes -= msg->size;
    }
    pthread_mutex_unlock(&g_out_lock);

    if (!msg)
      continue;

    if (msg->kind == OUT_NEW_CLIENT)
    {
      if (fd >= 0)
        close(fd);
      fd = msg->fd;
      gen = msg->gen;
      free(msg);
      continue;
    }

    if (fd < 0)
    {
      free(msg); /* queued for a client that has since gone */
      continue;
    }

    /*
     * Live video that waited too long is history. Writing it would put the
     * session permanently that far behind the phone, and the frames after it
     * repair the picture soon enough. A keyframe is never thrown away: it is
     * what the repair is made of.
     */
    if (!msg->priming && msg->hdr.type == APX_MSG_VIDEO &&
        !(msg->hdr.flags & APX_FLAG_KEYFRAME) && now_ns() - msg->queued_ns > APX_OUT_STALE_NS)
    {
      note_dropped();
      free(msg);
      continue;
    }

    bool drop_client = false;

    if (!wait_writable(fd, msg->priming ? APX_PRIME_WRITE_WAIT_MS : APX_WRITE_WAIT_MS))
    {
      /*
       * Nothing has been committed yet, so the message can simply be skipped
       * -- except during priming, where a hole cannot be skipped over because
       * the frames after it reference what was lost. Drop the client instead,
       * so it reconnects and is primed cleanly rather than being replayed at
       * on every frame from here on while it is already behind.
       */
      if (msg->priming)
      {
        LOGI("consumer stalled while priming, dropping client to resync");
        drop_client = true;
      }
      else
      {
        note_dropped();
      }
    }
    else if (!write_all(fd, &msg->hdr, sizeof(msg->hdr)) ||
             (msg->size && !write_all(fd, msg->payload, msg->size)))
    {
      /* Part-written: the framing is indeterminate and cannot be recovered. */
      LOGI("write failed mid-message, dropping client");
      drop_client = true;
    }

    free(msg);

    if (drop_client)
    {
      close(fd);
      fd = -1;
      LOGI("client disconnected");
      pthread_mutex_lock(&g_lock);
      /* Only if it has not already been replaced by a newer connection. */
      if (g_client_gen == gen)
        reset_client_state_locked();
      pthread_mutex_unlock(&g_lock);
    }
  }

  if (fd >= 0)
    close(fd);
  pthread_mutex_lock(&g_out_lock);
  out_clear_locked();
  pthread_mutex_unlock(&g_out_lock);
  return NULL;
}

/*
 * A filler-data NAL: valid Annex-B that every decoder is required to discard,
 * so it occupies the queue and decodes to nothing.
 */
static void send_keepalive_locked(void)
{
  static const uint8_t filler_h264[] = {0x00, 0x00, 0x00, 0x01, 0x0c, 0x80};
  static const uint8_t filler_hevc[] = {0x00, 0x00, 0x00, 0x01, 0x4c, 0x01, 0x80};

  const bool hevc = g_info.video_codec == APX_VCODEC_HEVC;
  const uint8_t* filler = hevc ? filler_hevc : filler_h264;
  const uint32_t len = hevc ? (uint32_t)sizeof(filler_hevc) : (uint32_t)sizeof(filler_h264);

  const uint64_t now = now_realtime_ns();
  const uint64_t pts = (now > g_pts_base) ? now - g_pts_base : 0;

  if (send_msg_locked(APX_MSG_VIDEO, 0, pts, filler, len))
    g_last_video_send_ns = now_ns();
}

/* ------------------------------------------------- parameter set caching */

/* Walk an Annex-B buffer and cache any parameter-set NALs we find, so a late
 * client can be primed. UxPlay hands us start-code delimited access units. */
static bool cache_parameter_sets(const uint8_t* data, size_t len, bool is_h265)
{
  size_t i = 0;
  size_t nal_start = 0;
  int nal_type = -1;
  uint8_t collected[4096];
  size_t collected_len = 0;
  bool got_any = false;

  while (i + 3 < len)
  {
    /* find a 3- or 4-byte start code */
    if (data[i] == 0 && data[i + 1] == 0 &&
        ((data[i + 2] == 1) || (i + 4 < len && data[i + 2] == 0 && data[i + 3] == 1)))
    {
      size_t sc_len = (data[i + 2] == 1) ? 3 : 4;

      if (nal_type >= 0)
      {
        /* close previous NAL */
        size_t nl = i - nal_start;
        if (collected_len + nl <= sizeof(collected))
        {
          memcpy(collected + collected_len, data + nal_start, nl);
          collected_len += nl;
          got_any = true;
        }
      }

      size_t hdr = i + sc_len;
      if (hdr >= len)
        break;

      int type;
      bool is_param;
      if (is_h265)
      {
        type = (data[hdr] >> 1) & 0x3f;
        is_param = (type == 32 || type == 33 || type == 34); /* VPS/SPS/PPS */
      }
      else
      {
        type = data[hdr] & 0x1f;
        is_param = (type == 7 || type == 8); /* SPS/PPS */
      }

      nal_start = is_param ? i : 0;
      nal_type = is_param ? type : -1;
      i = hdr;
      continue;
    }
    i++;
  }

  if (nal_type >= 0)
  {
    size_t nl = len - nal_start;
    if (collected_len + nl <= sizeof(collected))
    {
      memcpy(collected + collected_len, data + nal_start, nl);
      collected_len += nl;
      got_any = true;
    }
  }

  if (!got_any || !collected_len)
    return false;

  /* The sender emits a new SPS/PPS whenever the mirrored geometry changes,
   * e.g. when the phone is rotated. That invalidates both the extradata Kodi
   * opened the decoder with and any keyframe cached for the old geometry. */
  bool changed = (collected_len != g_params_len) || memcmp(g_params, collected, collected_len) != 0;

  memcpy(g_params, collected, collected_len);
  g_params_len = collected_len;
  return changed;
}

/* Does this access unit already carry SPS/PPS (H.264) or VPS/SPS/PPS (HEVC)? */
static bool au_has_parameter_sets(const uint8_t* data, size_t len, bool is_h265)
{
  for (size_t i = 0; i + 4 < len; i++)
  {
    if (data[i] != 0 || data[i + 1] != 0)
      continue;
    size_t hdr;
    if (data[i + 2] == 1)
      hdr = i + 3;
    else if (data[i + 2] == 0 && data[i + 3] == 1)
      hdr = i + 4;
    else
      continue;
    if (hdr >= len)
      break;

    if (is_h265)
    {
      int type = (data[hdr] >> 1) & 0x3f;
      if (type == 32 || type == 33 || type == 34)
        return true;
    }
    else
    {
      int type = data[hdr] & 0x1f;
      if (type == 7 || type == 8)
        return true;
    }
  }
  return false;
}

/* Does this Annex-B access unit carry an IDR (H.264 type 5, HEVC 16-21)? */
static bool au_has_keyframe(const uint8_t* data, size_t len, bool is_h265)
{
  for (size_t i = 0; i + 4 < len; i++)
  {
    if (data[i] != 0 || data[i + 1] != 0)
      continue;
    size_t hdr;
    if (data[i + 2] == 1)
      hdr = i + 3;
    else if (data[i + 2] == 0 && data[i + 3] == 1)
      hdr = i + 4;
    else
      continue;
    if (hdr >= len)
      break;

    if (is_h265)
    {
      int type = (data[hdr] >> 1) & 0x3f;
      if (type >= 16 && type <= 21)
        return true;
    }
    else if ((data[hdr] & 0x1f) == 5)
    {
      return true;
    }
  }
  return false;
}

static void gop_clear(void)
{
  for (size_t i = 0; i < g_gop_count; i++)
    free(g_gop[i].data);
  g_gop_count = 0;
  g_gop_bytes = 0;
}

/* Append a frame to the pending run; drops the run if it grows too large. */
static void gop_append(const uint8_t* data, size_t len, uint64_t ntp)
{
  if (g_gop_count >= APX_GOP_MAX_FRAMES || g_gop_bytes + len > APX_GOP_MAX_BYTES)
  {
    LOGI("no keyframe within %zu frames, dropping the buffered run", g_gop_count);
    gop_clear();
    return;
  }

  uint8_t* buf = malloc(len);
  if (!buf)
    return;
  memcpy(buf, data, len);
  /* There is something to start from again. */
  g_said_waiting_keyframe = false;
  g_unstartable_since_ns = 0;
  g_gop[g_gop_count].data = buf;
  g_gop[g_gop_count].len = len;
  g_gop[g_gop_count].ntp = ntp;
  g_gop_count++;
  g_gop_bytes += len;
}

/* Metadata can contain anything, and the event channel is line based. */
static char* base64_encode(const uint8_t* in, size_t len)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char* out = malloc(4 * ((len + 2) / 3) + 1);
  if (!out)
    return NULL;
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3)
  {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < len)
      v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < len)
      v |= in[i + 2];
    out[o++] = tbl[(v >> 18) & 0x3f];
    out[o++] = tbl[(v >> 12) & 0x3f];
    out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 0x3f] : '=';
    out[o++] = (i + 2 < len) ? tbl[v & 0x3f] : '=';
  }
  out[o] = 0;
  return out;
}

/* ------------------------------------------- querying Kodi's player state */

/*
 * The HLS path is a pull interface: the sender asks us how playback is going
 * so it can drive its own scrubber. Kodi's JSON-RPC server (enabled by default
 * on 9090) is the only way to answer from this process. Queries only; opening
 * playback still goes through the service, which can attach list-item
 * properties and skips the VFS existence check that rejects our URLs.
 */
static bool kodi_rpc(const char* request, char* reply, size_t reply_size)
{
  struct sockaddr_in addr;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9090);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  struct timeval tv = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  bool ok = false;
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
      send(fd, request, strlen(request), MSG_NOSIGNAL) > 0)
  {
    ssize_t n = recv(fd, reply, reply_size - 1, 0);
    if (n > 0)
    {
      reply[n] = 0;
      ok = true;
    }
  }
  close(fd);
  return ok;
}

/* Pull "hours"/"minutes"/"seconds"/"milliseconds" out of a named time object. */
static bool json_time_seconds(const char* json, const char* field, double* out)
{
  const char* p = strstr(json, field);
  if (!p)
    return false;
  const char* end = strchr(p, '}');
  if (!end)
    return false;

  int h = 0, m = 0, sec = 0, ms = 0;
  const char* q;
  if ((q = strstr(p, "\"hours\":")) && q < end)
    h = atoi(q + 8);
  if ((q = strstr(p, "\"minutes\":")) && q < end)
    m = atoi(q + 10);
  if ((q = strstr(p, "\"seconds\":")) && q < end)
    sec = atoi(q + 10);
  if ((q = strstr(p, "\"milliseconds\":")) && q < end)
    ms = atoi(q + 15);

  *out = h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
  return true;
}

/* Returns false when nothing is playing. */
static bool kodi_playback_state(double* position, double* duration, float* rate)
{
  char reply[1024];

  /*
   * Whether anything is playing at all, first.
   *
   * Player.GetProperties answers for a player that does not exist with a
   * successful result full of zeros rather than an error, so on its own it
   * cannot tell "a video that has just started" from "nothing is playing".
   * Reading it as the former when the latter is true is what left a sender
   * being told its video was still going after playback had been stopped
   * here: it never learned the video had ended, so pressing play on the phone
   * only asked to resume something Kodi no longer had.
   */
  static const char* actives =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Player.GetActivePlayers\"}";
  if (!kodi_rpc(actives, reply, sizeof(reply)) || !strstr(reply, "\"playerid\""))
    return false;

  static const char* req =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Player.GetProperties\","
      "\"params\":{\"playerid\":1,\"properties\":[\"time\",\"totaltime\",\"speed\"]}}";

  if (!kodi_rpc(req, reply, sizeof(reply)) || strstr(reply, "\"error\""))
    return false;

  const char* totals = strstr(reply, "\"totaltime\"");
  if (!json_time_seconds(reply, "\"time\"", position))
    return false;
  if (!totals || !json_time_seconds(totals, "\"totaltime\"", duration))
    *duration = 0.0;

  const char* sp = strstr(reply, "\"speed\":");
  *rate = sp ? (float)atof(sp + 8) : 1.0f;
  return true;
}

/* --------------------------------------------------- events to the service */

static void emit_event(const char* event)
{
  pthread_mutex_lock(&g_event_lock);
  printf("EVENT %s\n", event);
  fflush(stdout);
  pthread_mutex_unlock(&g_event_lock);
}

static void emit_event_arg(const char* event, const char* arg)
{
  pthread_mutex_lock(&g_event_lock);
  printf("EVENT %s %s\n", event, arg);
  fflush(stdout);
  pthread_mutex_unlock(&g_event_lock);
}

/*
 * Claim the right to ask Kodi for a player, returning the event that says so
 * or NULL if a request is already outstanding. The caller emits it after
 * dropping g_lock: writing an event blocks until the service reads it, and
 * nothing else should have to wait behind that.
 *
 * The two events are distinguished because only an audio session needs
 * holding back to see whether a video handoff follows. Mirroring deferred by
 * the same grace period just buffers frames the sender is already producing,
 * and every one of them becomes startup latency.
 */
static const char* request_playback_locked(void)
{
  if (g_session.player_open)
    return NULL;
  g_session.player_open = true;
  return g_session.mode == MODE_AUDIO ? "PLAYAUDIO" : "PLAY";
}

/*
 * True when the sender is still producing but nothing is listening, i.e. Kodi
 * was stopped locally. Offer playback again so picking a new track on the
 * phone brings the receiver back, rate limited so a deliberate stop is not
 * fought over.
 */
static bool should_reoffer_locked(void)
{
  if (g_client_connected || g_session.player_open)
    return false;

  /*
   * Only when there is something to start it with. A mirror stream can only
   * be joined at a keyframe, and the sender decides when to send one -- there
   * is no way to ask, the mirror channel only ever carries data one way. So
   * opening the player before a keyframe is buffered gives Kodi a stream with
   * nothing in it, and it sits on the buffering spinner until the sender
   * happens to send one, which while mirroring can be minutes.
   *
   * Waiting instead means the screen stays where it was and mirroring comes
   * back by itself at the next keyframe. An audio session has nothing to wait
   * for.
   */
  if (g_session.mode != MODE_AUDIO && !g_gop_count)
  {
    if (!g_said_waiting_keyframe)
    {
      g_said_waiting_keyframe = true;
      g_unstartable_since_ns = now_ns();
      LOGI("nothing to start playback from yet");
    }
    return false;
  }

  uint64_t now = now_ns();
  if (g_last_play_request_ns && now - g_last_play_request_ns < 2000000000ull)
    return false;
  g_last_play_request_ns = now;
  return true;
}

/* ------------------------------------------------------- raop callbacks */

static void cb_conn_init(void* cls)
{
  LOGI("client session started");
}

/*
 * Wind the session up: tell the add-on the stream is over and forget
 * everything derived from it, so the next session starts from scratch.
 */
static void end_session(void)
{
  pthread_mutex_lock(&g_lock);
  send_msg_locked(APX_MSG_EOS, 0, 0, NULL, 0);
  g_have_base = false;
  g_params_len = 0;
  gop_clear();
  g_primed = false;
  g_video_started = false;
  g_last_audio_ns = 0;
  g_audio_session_ns = 0;
  g_audio_session_frames = 0;
  session_set_mode_locked(MODE_IDLE);
  g_session.notify_video_ended = false;
  g_meta_previous[0] = '\0';
  g_said_waiting_keyframe = false;
  g_unstartable_since_ns = 0;
  g_mirror_stop_pending_ns = 0;
  g_video_seen_playing = false;
  memset(&g_info, 0, sizeof(g_info));
  g_info.sample_rate = 44100;
  g_info.channels = 2;
  /*
   * Deliberately not gated on player_open: that flag is cleared whenever the
   * add-on disconnects, which is how a re-offer becomes possible, so gating
   * on it meant a session that had lost its client could never ask Kodi to
   * stop and playback outlived the session. The service only stops a stream
   * it recognises as its own, so a redundant stop costs nothing.
   */
  g_session.player_open = false;
  pthread_mutex_unlock(&g_lock);

  emit_event("STOP");
}

/*
 * Mirroring's own lifecycle, reported by the thread that reads the mirror
 * data socket. This is the only trustworthy end-of-mirroring signal: HTTP
 * connections come and go throughout a session for reasons of their own.
 */
static void cb_mirror_video_running(void* cls, bool running)
{
  LOGI("mirroring %s", running ? "started" : "stopped");
  if (running)
    return;

  pthread_mutex_lock(&g_lock);
  const bool ours = g_session.mode == MODE_MIRROR;
  const bool handover = g_mirror_to_video;
  g_mirror_to_video = false;
  pthread_mutex_unlock(&g_lock);

  if (!ours)
    return;
  if (handover)
  {
    /* The sender is switching to video streaming and will tell us what to
     * play; stopping here would just make the screen flash. */
    LOGI("mirroring handed over to video streaming");
    return;
  }

  /*
   * Hold it briefly in case a video request is on its way; see the grace
   * above. The watchdog ends the session if nothing arrives.
   */
  pthread_mutex_lock(&g_lock);
  g_mirror_stop_pending_ns = now_ns();
  pthread_mutex_unlock(&g_lock);
}

static void cb_conn_destroy(void* cls)
{
  LOGI("client session ended");

  /*
   * Fires for every HTTP connection the library closes, which includes the
   * sender's control connections and Kodi's own loopback fetches from the
   * local HLS server -- none of which imply the session is over. Mirroring
   * ends through cb_mirror_video_running and video streaming through its own
   * stop callbacks, so the only mode still relying on this is audio, where a
   * RAOP teardown is all the library gives us.
   */
  pthread_mutex_lock(&g_lock);
  const bool audio = g_session.mode == MODE_AUDIO;
  pthread_mutex_unlock(&g_lock);

  if (audio)
    end_session();
}

static void cb_conn_reset(void* cls, int reason)
{
  LOGI("connection reset (reason %d)", reason);
  end_session();
}

static void cb_video_reset(void* cls, reset_type_t reset_type)
{
  LOGI("video reset (type %d)", (int)reset_type);

  /*
   * Only tear the library's playlist state down for the resets that mean it:
   * doing it on every reset frees structures the httpd thread is still using
   * to serve /action, and it faults in strcmp inside http_handler_action.
   * ON_VIDEO_PLAY in particular arrives while a video request is in flight.
   */
  if (reset_type == RESET_TYPE_RTP_TO_HLS_TEARDOWN)
  {
    /* Sent just before the mirror stream is stopped, so the stop that follows
     * is a protocol switch rather than the end of the session. */
    pthread_mutex_lock(&g_lock);
    g_mirror_to_video = true;
    pthread_mutex_unlock(&g_lock);
  }

  if (g_hls_enabled &&
      (reset_type == RESET_TYPE_NOHOLD || reset_type == RESET_TYPE_HLS_SHUTDOWN))
  {
    raop_destroy_airplay_video(g_raop, -1);
    if (reset_type == RESET_TYPE_HLS_SHUTDOWN)
      raop_remove_hls_connections(g_raop);
  }

  pthread_mutex_lock(&g_lock);
  /* The sender restarts its encoder, so the cached configuration and keyframe
   * are stale; wait to be re-primed from the next IDR. */
  g_params_len = 0;
  gop_clear();
  g_primed = false;
  pthread_mutex_unlock(&g_lock);
}

static int cb_video_set_codec(void* cls, video_codec_t codec)
{
  LOGI("mirroring codec: %s", codec == VIDEO_CODEC_H265 ? "hevc" : "h264");

  pthread_mutex_lock(&g_lock);
  g_info.video_codec = (codec == VIDEO_CODEC_H265) ? APX_VCODEC_HEVC : APX_VCODEC_H264;
  g_sent_streaminfo = false;

  /* Mirroring replaces any video streaming session that came before it.
   * Clearing the latch matters as much as the flags: it is what lets a new
   * PLAY be emitted, and a video session leaves it set, so without this the
   * request is swallowed and mirroring never opens a player.
   *
   * The sender also has to be told the video is finished. Stopping playback
   * locally is invisible to it: it keeps polling, sees no progress, and
   * simply starts the video again, which tears the new mirroring session
   * straight back down. */
  if (g_session.mode == MODE_VIDEO)
    g_session.notify_video_ended = true;
  /* A fresh mirror stream: any pending handover belonged to the last one, and
   * so did any stop waiting to be acted on. */
  g_mirror_to_video = false;
  g_mirror_stop_pending_ns = 0;
  session_set_mode_locked(MODE_MIRROR);
  g_session.player_open = false;

  /* Mirroring is starting: ask Kodi to open the stream so it connects back. */
  const char* play = request_playback_locked();
  pthread_mutex_unlock(&g_lock);

  if (play)
    emit_event(play);
  return 0;
}

static void cb_video_report_size(void* cls, float* width_source, float* height_source, float* width,
                                 float* height)
{
  pthread_mutex_lock(&g_lock);
  /* Record the geometry but do not force a republish here: a rotation already
   * brings new parameter sets, and triggering off both made Kodi rebuild the
   * decoder twice for one rotation. */
  g_info.width = (uint32_t)*width;
  g_info.height = (uint32_t)*height;
  pthread_mutex_unlock(&g_lock);
  LOGI("source %.0fx%.0f, display %.0fx%.0f", *width_source, *height_source, *width, *height);
}

static void cb_audio_get_format(void* cls, unsigned char* ct, unsigned short* spf,
                                bool* usingScreen, bool* isMedia, uint64_t* audioFormat)
{
  static const uint8_t eld[] = APX_ASC_AAC_ELD;
  static const uint8_t lc[] = APX_ASC_AAC_LC;
  static const uint8_t alac[] = APX_ALAC_COOKIE;

  const uint8_t* extra = NULL;
  uint32_t extra_len = 0;
  if (*ct == APX_ACT_AAC_ELD)
  {
    extra = eld;
    extra_len = (uint32_t)sizeof(eld);
  }
  else if (*ct == APX_ACT_AAC_LC)
  {
    extra = lc;
    extra_len = (uint32_t)sizeof(lc);
  }
  else if (*ct == APX_ACT_ALAC)
  {
    extra = alac;
    extra_len = (uint32_t)sizeof(alac);
  }

  if (*usingScreen && !g_mirror_audio)
  {
    static bool said;
    if (!said)
    {
      said = true;
      LOGI("mirroring audio is switched off, not declaring the stream");
    }
    return;
  }

  pthread_mutex_lock(&g_lock);

  /* Start the stall clock here too, so a stream that is negotiated but never
   * actually delivered is withdrawn rather than stalling the player. */
  g_last_audio_ns = now_ns();
  if (g_info.audio_ct != *ct)
  {
    g_audio_session_ns = g_last_audio_ns;
    g_audio_session_frames = 0;
  }

  /*
   * The sender re-negotiates the format every few seconds even when nothing
   * has changed. Each republish reaches the add-on as a demux stream change,
   * which makes Kodi tear down and rebuild its audio pipeline, so only act on
   * a format that is genuinely different.
   */
  const bool changed = g_info.audio_ct != *ct || g_info.audio_extradata_size != extra_len ||
                       (extra_len && memcmp(g_info.audio_extradata, extra, extra_len) != 0);

  if (changed)
  {
    g_info.audio_ct = *ct;
    g_info.sample_rate = 44100;
    g_info.channels = 2;
    g_info.audio_extradata_size = extra_len;
    if (extra_len)
      memcpy(g_info.audio_extradata, extra, extra_len);

    /* The add-on turns a new stream info message into a demux stream change,
     * which is how the audio track appears mid-session. */
    g_sent_streaminfo = false;
    send_streaminfo_locked();
    LOGI("audio format ct=%u spf=%u usingScreen=%d isMedia=%d", (unsigned)*ct, (unsigned)*spf,
         (int)*usingScreen, (int)*isMedia);
  }

  /* usingScreen distinguishes mirroring audio from a plain music stream. */
  if (!*usingScreen && g_info.video_codec == APX_VCODEC_NONE)
    session_set_mode_locked(MODE_AUDIO);
  /* Nothing else will ask Kodi to open the stream for a music session. */
  const char* play = g_session.mode == MODE_AUDIO ? request_playback_locked() : NULL;
  pthread_mutex_unlock(&g_lock);

  if (play)
    emit_event(play);
}

static void send_streaminfo_locked(void)
{
  if (g_sent_streaminfo || !g_client_connected)
    return;

  /* Kodi will not open an H.264/HEVC codec without extradata, so hold the
   * stream info back until the sender has given us the parameter sets. An
   * audio-only session has no video stream to describe. */
  if (g_session.mode != MODE_AUDIO && !g_params_len)
    return;

  g_info.video_extradata_size =
      (uint32_t)(g_params_len > APX_MAX_VIDEO_EXTRADATA ? APX_MAX_VIDEO_EXTRADATA : g_params_len);
  memcpy(g_info.video_extradata, g_params, g_info.video_extradata_size);

  if (send_msg_locked(APX_MSG_STREAMINFO, 0, 0, &g_info, (uint32_t)sizeof(g_info)))
    g_sent_streaminfo = true;
}

static void cb_video_process(void* cls, raop_ntp_t* ntp, video_decode_struct* data)
{
  if (!data || data->data_len <= 0 || !data->data)
    return;

  /* lib/ signals a decrypt/parse failure by setting the first byte non-zero */
  if (data->data[0] != 0)
  {
    LOGI("dropping corrupt video frame");
    return;
  }

  pthread_mutex_lock(&g_lock);

  g_frames_in++;

  /* A pause from the sender shows up here; Kodi will stall for its duration
   * because a realtime stream only leaves its buffering state once the audio
   * queue refills. Worth seeing explicitly in the log. */
  if (g_last_video_ntp && data->ntp_time_local > g_last_video_ntp)
  {
    uint64_t gap_ms = (data->ntp_time_local - g_last_video_ntp) / 1000000ull;
    if (gap_ms >= 500)
    {
      LOGI("sender paused for %llu ms before frame %llu", (unsigned long long)gap_ms,
           (unsigned long long)g_frames_in);

      /*
       * Deliberately no resync here. iOS stops sending whenever the screen is
       * static, so pauses of several seconds are routine, and re-priming on
       * each one dumped the whole buffered run into the player and moved the
       * timeline underneath it -- the queue filled and latency grew the
       * longer a session ran. The corruption this once guarded against came
       * from replaying a lone stale keyframe, which buffering the full run
       * fixed at source.
       */
    }
  }
  g_last_video_ntp = data->ntp_time_local;

  if (cache_parameter_sets(data->data, (size_t)data->data_len, data->is_h265))
  {
    LOGI("video configuration changed (%zu byte parameter sets), resyncing", g_params_len);
    /* Republish the stream info so Kodi reopens the decoder with the new
     * extradata, and wait for a keyframe in the new geometry before sending
     * anything else. */
    g_sent_streaminfo = false;
    g_primed = false;
    gop_clear();
  }

  /* Keep the current run buffered at all times, not just while unprimed: a
   * resync can then always replay from the last keyframe. iOS stops sending
   * entirely when the screen is static, so waiting for a fresh keyframe after
   * a pause can wait for ever. */
  if (au_has_keyframe(data->data, (size_t)data->data_len, data->is_h265))
    gop_clear(); /* a keyframe makes everything before it redundant */
  if (g_gop_count || au_has_keyframe(data->data, (size_t)data->data_len, data->is_h265))
    gop_append(data->data, (size_t)data->data_len, data->ntp_time_local);

  if (g_verbose && (g_frames_in <= 5 || (g_frames_in % 120) == 0))
    LOGI("video: in=%llu out=%llu audio_out=%llu skipped=%llu last=%d bytes params=%zu client=%s",
         (unsigned long long)g_frames_in, (unsigned long long)g_frames_out,
         (unsigned long long)g_audio_out, (unsigned long long)g_dropped_msgs, data->data_len,
         g_params_len, g_client_connected ? "yes" : "no");

  if (!g_client_connected)
  {
    const char* play = should_reoffer_locked() ? request_playback_locked() : NULL;
    pthread_mutex_unlock(&g_lock);
    if (play)
      emit_event(play);
    return;
  }

  send_streaminfo_locked();

  /* Until the decoder has had a keyframe, anything we send is undecodable.
   * Flush the buffered run so it gets the keyframe and every delta since,
   * with their original timing, then carry on live. */
  if (!g_primed)
  {
    if (!g_gop_count)
    {
      /* Every frame here is being thrown away, so say so: it looks exactly
       * like a freeze from the outside. */
      static uint64_t waiting;
      if ((waiting++ % 120) == 0)
        LOGI("no keyframe to resync from yet, %llu frames dropped waiting",
             (unsigned long long)waiting);
      pthread_mutex_unlock(&g_lock);
      return;
    }

    /*
     * Present the backlog over a few milliseconds rather than the seconds it
     * originally spanned. Every frame is still decoded, so references hold,
     * but playback resumes at the live edge instead of however long ago the
     * keyframe was. Anchoring on the newest frame keeps the live frames that
     * follow on the wall clock.
     */
    const uint64_t replay_step = 1000000ull; /* 1ms per backlog frame */
    const uint64_t span = (uint64_t)(g_gop_count - 1) * replay_step;

    /*
     * Only the first run of a connection sets the time base, placing the
     * keyframe at zero. Later runs -- a resync after the consumer fell
     * behind -- must keep the timeline they are joining: re-anchoring here
     * restarted it near zero, so time jumped backwards by however long had
     * passed since the last resync, and Kodi reported audio sync errors of
     * several seconds for the rest of the session.
     */
    if (!g_have_base)
    {
      g_pts_base = g_gop[g_gop_count - 1].ntp - span;
      g_have_base = true;
    }

    /* Land the run just before where the newest buffered frame belongs, so
     * the live frames that follow carry on from it. */
    const uint64_t newest = (g_gop[g_gop_count - 1].ntp > g_pts_base)
                                ? g_gop[g_gop_count - 1].ntp - g_pts_base
                                : span;
    const uint64_t first_pts = (newest > span) ? newest - span : 0;

    const bool need_params =
        g_params_len && !au_has_parameter_sets(g_gop[0].data, g_gop[0].len, data->is_h265);

    bool ok = true;
    g_priming = true;
    for (size_t i = 0; i < g_gop_count && ok; i++)
    {
      const uint64_t pts = first_pts + (uint64_t)i * replay_step;
      const uint32_t flags = (i == 0) ? APX_FLAG_KEYFRAME : 0;

      if (i == 0 && need_params)
      {
        uint8_t* tmp = malloc(g_params_len + g_gop[i].len);
        if (!tmp)
          break;
        memcpy(tmp, g_params, g_params_len);
        memcpy(tmp + g_params_len, g_gop[i].data, g_gop[i].len);
        ok = send_msg_locked(APX_MSG_VIDEO, flags, pts, tmp,
                             (uint32_t)(g_params_len + g_gop[i].len));
        free(tmp);
      }
      else
      {
        ok = send_msg_locked(APX_MSG_VIDEO, flags, pts, g_gop[i].data, (uint32_t)g_gop[i].len);
      }
      if (ok)
        g_frames_out++;
    }

    g_priming = false;

    if (ok)
    {
      LOGI("primed client with %zu buffered frames (%zu bytes, %s parameter sets)", g_gop_count,
           g_gop_bytes, need_params ? "added" : "inline");
      g_primed = true;
      g_video_started = true;
      g_sent_params = true;
    }
    /* The run stays buffered so the next resync has something to replay. */

    pthread_mutex_unlock(&g_lock);
    return; /* the current frame was part of the run just sent */
  }

  if (!g_have_base)
  {
    g_pts_base = data->ntp_time_local;
    g_have_base = true;
  }
  uint64_t pts = (data->ntp_time_local > g_pts_base) ? data->ntp_time_local - g_pts_base : 0;
  g_last_video_send_ns = now_ns();

  if (send_msg_locked(APX_MSG_VIDEO, 0, pts, data->data, (uint32_t)data->data_len))
    g_frames_out++;
  pthread_mutex_unlock(&g_lock);
}

static void cb_audio_process(void* cls, raop_ntp_t* ntp, audio_decode_struct* data)
{
  if (!data || data->data_len <= 0 || !data->data)
    return;

  /*
   * Not just undeclared but not sent either: a packet for a stream Kodi was
   * never told about can have the demuxer bring that stream into being, which
   * would restore the stalling this setting exists to avoid.
   */
  pthread_mutex_lock(&g_lock);

  if (g_session.mode == MODE_MIRROR && !g_mirror_audio)
  {
    pthread_mutex_unlock(&g_lock);
    return;
  }

  g_audio_in++;
  g_audio_session_frames++;
  if (g_verbose && (g_audio_in <= 3 || (g_audio_in % 400) == 0))
    LOGI("audio: in=%llu out=%llu ct=%u mode=%s primed=%d client=%s",
         (unsigned long long)g_audio_in, (unsigned long long)g_audio_out, g_info.audio_ct,
         mode_name(g_session.mode), (int)g_primed, g_client_connected ? "yes" : "no");

  if (!g_client_connected)
  {
    const char* play = should_reoffer_locked() ? request_playback_locked() : NULL;
    pthread_mutex_unlock(&g_lock);
    if (play)
      emit_event(play);
    return;
  }

  g_last_audio_ns = now_ns();
  /* For a mirroring session wait for the video keyframe, so audio and video
   * share one time base and Kodi does not start its audio clock before there
   * is a picture. An audio-only session has nothing to wait for. */
  if (g_session.mode != MODE_AUDIO && !g_video_started)
  {
    pthread_mutex_unlock(&g_lock);
    return;
  }

  send_streaminfo_locked();

  if (!g_have_base)
  {
    g_pts_base = data->ntp_time_local;
    g_have_base = true;
  }
  uint64_t pts = (data->ntp_time_local > g_pts_base) ? data->ntp_time_local - g_pts_base : 0;

  if (send_msg_locked(APX_MSG_AUDIO, 0, pts, data->data, (uint32_t)data->data_len))
    g_audio_out++;
  pthread_mutex_unlock(&g_lock);
}

static void cb_audio_flush(void* cls)
{
}
static void cb_video_flush(void* cls)
{
}
static void cb_video_pause(void* cls)
{
}
static void cb_video_resume(void* cls)
{
}
static void cb_conn_feedback(void* cls)
{
}
/*
 * AirPlay carries volume as attenuation in dB: 0 is full, -30 is the quiet
 * end of the sender's slider, and -144 means mute.
 */
#define APX_VOL_DB_MIN (-30.0)

static int volume_db_to_percent(double db)
{
  if (db <= APX_VOL_DB_MIN)
    return 0;
  if (db > 0.0)
    db = 0.0;
  int percent = (int)((1.0 + db / -APX_VOL_DB_MIN) * 100.0 + 0.5);
  return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

static void cb_audio_set_volume(void* cls, float volume)
{
  if (!g_volume_control)
    return;

  const int percent = volume_db_to_percent(volume);
  char request[160];
  snprintf(request, sizeof(request),
           "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Application.SetVolume\","
           "\"params\":{\"volume\":%d}}",
           percent);
  char reply[256];
  if (!kodi_rpc(request, reply, sizeof(reply)))
    LOGI("volume: setting Kodi to %d%% failed", percent);
}

/*
 * Reported to the sender as its starting slider position, so it opens where
 * Kodi actually is rather than jumping the volume on the first drag.
 */
static double cb_audio_set_client_volume(void* cls)
{
  if (!g_volume_control)
    return 0.0; /* full: we are not going to act on changes anyway */

  static const char* req =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Application.GetProperties\","
      "\"params\":{\"properties\":[\"volume\",\"muted\"]}}";
  char reply[512];
  if (!kodi_rpc(req, reply, sizeof(reply)))
    return 0.0;

  if (strstr(reply, "\"muted\":true"))
    return -144.0;

  const char* v = strstr(reply, "\"volume\":");
  if (!v)
    return 0.0;
  const int percent = atoi(v + 9);
  if (percent <= 0)
    return -144.0;
  return (percent >= 100) ? 0.0 : (percent / 100.0 - 1.0) * -APX_VOL_DB_MIN;
}
/*
 * The sender describes the current track with a DMAP blob: a sequence of
 * four-character codes, each with a big-endian length. Only the three fields
 * Kodi shows are pulled out.
 */
static void cb_audio_set_metadata(void* cls, const void* buffer, int buflen)
{
  const uint8_t* p = buffer;
  char title[256] = "", artist[256] = "", album[256] = "";

  if (!p || buflen < 8)
    return;

  /* skip the outer container header */
  size_t pos = 8;
  while (pos + 8 <= (size_t)buflen)
  {
    const char* code = (const char*)(p + pos);
    uint32_t len = ((uint32_t)p[pos + 4] << 24) | ((uint32_t)p[pos + 5] << 16) |
                   ((uint32_t)p[pos + 6] << 8) | (uint32_t)p[pos + 7];
    pos += 8;
    if (len > (uint32_t)buflen - pos)
      break;

    char* dst = NULL;
    if (!memcmp(code, "minm", 4))
      dst = title;
    else if (!memcmp(code, "asar", 4))
      dst = artist;
    else if (!memcmp(code, "asal", 4))
      dst = album;

    if (dst)
    {
      size_t n = len < 255 ? len : 255;
      memcpy(dst, p + pos, n);
      dst[n] = 0;
    }
    pos += len;
  }

  if (!*title && !*artist && !*album)
    return;

  /* The sender repeats the current track every few seconds; only pass on real
   * changes so the service is not woken for nothing. Cleared when a session
   * ends, or replaying a track in the next one would be swallowed. */
  char current[800];
  snprintf(current, sizeof(current), "%s|%s|%s", title, artist, album);
  if (!strcmp(current, g_meta_previous))
    return;
  snprintf(g_meta_previous, sizeof(g_meta_previous), "%s", current);

  char joined[800];
  int n = snprintf(joined, sizeof(joined), "%s\n%s\n%s", title, artist, album);
  if (n <= 0)
    return;

  char* encoded = base64_encode((const uint8_t*)joined, (size_t)n);
  if (!encoded)
    return;

  pthread_mutex_lock(&g_event_lock);
  printf("EVENT META %s\n", encoded);
  fflush(stdout);
  pthread_mutex_unlock(&g_event_lock);
  free(encoded);

  LOGI("now playing: %s - %s (%s)", artist, title, album);
}
static void cb_audio_set_coverart(void* cls, const void* buffer, int buflen)
{
  if (!buffer || buflen <= 0)
    return;

  /* Alternate between two names so Kodi reloads the image rather than serving
   * the previous track's artwork from its texture cache. */
  static int slot;
  char path[256];
  snprintf(path, sizeof(path), "%s/art%d.jpg", g_art_dir[0] ? g_art_dir : "/tmp", slot);
  slot ^= 1;

  FILE* f = fopen(path, "wb");
  if (!f)
    return;
  size_t written = fwrite(buffer, 1, (size_t)buflen, f);
  fclose(f);
  if (written != (size_t)buflen)
    return;

  emit_event_arg("ART", path);
}
/*
 * The sender's position within the track, as RTP sample positions: start and
 * end bracket the track, curr is where it has got to. Converting them to
 * milliseconds here is what lets the add-on give Kodi an elapsed time and a
 * duration, so a streamed track gets a real progress bar instead of running
 * as an endless stream.
 */
static void cb_audio_set_progress(void* cls, uint32_t* start, uint32_t* curr, uint32_t* end)
{
  if (!start || !curr || !end)
    return;

  pthread_mutex_lock(&g_lock);
  const uint32_t rate = g_info.sample_rate ? g_info.sample_rate : 44100;

  struct apx_progress progress = {0, 0};
  /* Unsigned and sender-supplied: a curr before start, or an end before
   * either, is a malformed report rather than a negative time. */
  if (*end > *start)
    progress.duration_ms = (uint32_t)(((uint64_t)(*end - *start) * 1000) / rate);
  if (*curr > *start)
    progress.position_ms = (uint32_t)(((uint64_t)(*curr - *start) * 1000) / rate);
  if (progress.duration_ms && progress.position_ms > progress.duration_ms)
    progress.position_ms = progress.duration_ms;

  static uint32_t last_logged;
  if (progress.duration_ms != last_logged)
  {
    last_logged = progress.duration_ms;
    LOGI("track progress: %u/%u ms", progress.position_ms, progress.duration_ms);
  }

  send_msg_locked(APX_MSG_PROGRESS, 0, 0, &progress, (uint32_t)sizeof(progress));
  pthread_mutex_unlock(&g_lock);
}
static void cb_report_client_request(void* cls, char* deviceid, char* model, char* name, bool* admit)
{
  LOGI("connection request from %s (%s)", name ? name : "?", model ? model : "?");
  *admit = true;
}
/* Shown on screen so the person holding the phone can read it. */
static void cb_display_pin(void* cls, char* pin)
{
  if (!pin)
    return;
  LOGI("pairing: PIN is %s", pin);
  emit_event_arg("PIN", pin);
}

/*
 * A device that has just paired. Kept by public key, which is what the
 * library offers back on the next connection.
 */
static bool cb_check_register(void* cls, const char* pk);

static void cb_register_client(void* cls, const char* device_id, const char* pk, const char* name)
{
  if (!pk || !*pk || !g_registry_path[0])
    return;

  /* Senders re-pair for reasons of their own, and the file is only ever
   * appended to, so without this it grows a duplicate line each time. */
  if (cb_check_register(cls, pk))
  {
    LOGI("pairing: %s is already known", name && *name ? name : "device");
    return;
  }

  FILE* f = fopen(g_registry_path, "a");
  if (!f)
  {
    LOGI("pairing: cannot record %s in %s", name ? name : "device", g_registry_path);
    return;
  }
  /* The name is only for the person reading the file, so keep it on one line. */
  fprintf(f, "%s %s\n", pk, name && *name ? name : "unnamed");
  fclose(f);

  LOGI("pairing: remembered %s", name && *name ? name : "device");
  emit_event_arg("PAIRED", name && *name ? name : "device");
}

/* Whether this device has paired before. False sends it back to the PIN. */
static bool cb_check_register(void* cls, const char* pk)
{
  if (!pk || !*pk || !g_registry_path[0])
    return false;

  FILE* f = fopen(g_registry_path, "r");
  if (!f)
    return false;

  char line[512];
  bool found = false;
  const size_t len = strlen(pk);
  while (!found && fgets(line, sizeof(line), f))
    found = !strncmp(line, pk, len) && (line[len] == ' ' || line[len] == '\n' || !line[len]);
  fclose(f);

  if (!found)
    LOGI("pairing: unknown device, asking for the PIN again");
  return found;
}

static const char* cb_passwd(void* cls, int* len)
{
  /* len 0 means no password wanted; the library then lets the client in
   * without a digest challenge. A non-empty one is challenged per RFC 2617. */
  *len = (int)strlen(g_password);
  return *len ? g_password : NULL;
}
/*
 * The sender's own remote-control endpoint. Both values are needed to reach
 * it: the id names its _dacp._tcp service, and the token authorises requests.
 * Handing them to the service lets Kodi's transport controls drive the phone,
 * which is the only way to pause the thing actually producing the stream.
 */
static void cb_export_dacp(void* cls, const char* active_remote, const char* dacp_id)
{
  if (!active_remote || !*active_remote || !dacp_id || !*dacp_id)
    return;

  LOGI("sender remote control available (dacp id %s)", dacp_id);
  char arg[256];
  snprintf(arg, sizeof(arg), "%s %s", dacp_id, active_remote);
  emit_event_arg("DACP", arg);
}
/*
 * The library logs per packet at debug level, and one burst of audio
 * retransmission requests runs to thousands of lines in a fraction of a
 * second. Each is a synchronous write from the thread receiving RTP, so an
 * unthrottled burst stalls the pipeline it is reporting on -- which loses
 * more packets, which asks for more retransmissions. Cap the rate instead,
 * and say how much was dropped so the gap is visible.
 */
#define APX_LOG_MAX_PER_SEC 200

static void cb_log(void* cls, int level, const char* msg)
{
  static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
  static uint64_t window_ns;
  static unsigned in_window;
  static unsigned suppressed;

  if (!msg || !*msg)
    return; /* the library logs a lot of blank lines */

  const uint64_t now = now_ns();
  unsigned dropped = 0;

  pthread_mutex_lock(&log_lock);
  if (now - window_ns >= 1000000000ull)
  {
    window_ns = now;
    dropped = suppressed;
    suppressed = 0;
    in_window = 0;
  }
  const bool emit = in_window < APX_LOG_MAX_PER_SEC;
  if (emit)
    in_window++;
  else
    suppressed++;
  pthread_mutex_unlock(&log_lock);

  if (dropped)
    LOGI("lib: suppressed %u further messages in the previous second", dropped);
  if (emit)
    LOGI("lib: %s", msg);
}

/* ------------------------------------------- AirPlay video (HLS) callbacks */

/*
 * Non-mirror video: the sender hands over a playlist URL instead of encoded
 * frames. lib/airplay_video.c has already done the hard part -- pulling the
 * playlists back over the reverse channel and reserving them on its own local
 * http server -- so all that is left is to point Kodi at the result and report
 * back on how playback is going. This path does not involve
 * inputstream.airplay at all; it is an ordinary HLS stream.
 */
static void cb_on_video_play(void* cls, const char* location, const float start_position)
{
  if (!location)
    return;

  LOGI("airplay video: %s (from %.1fs)", location, start_position);

  char* encoded = base64_encode((const uint8_t*)location, strlen(location));
  if (!encoded)
    return;

  char arg[2048];
  snprintf(arg, sizeof(arg), "%s %.3f", encoded, start_position);
  free(encoded);

  pthread_mutex_lock(&g_lock);
  /* This is the handoff the pending stop was waiting for. */
  g_mirror_stop_pending_ns = 0;
  g_video_seen_playing = false;
  session_set_mode_locked(MODE_VIDEO);
  g_video_start_ns = now_ns();
  g_session.player_open = true; /* so a later teardown still asks for a stop */
  pthread_mutex_unlock(&g_lock);

  emit_event_arg("HLS", arg);
}

static void cb_on_video_scrub(void* cls, const float position)
{
  char arg[64];
  snprintf(arg, sizeof(arg), "%.3f", position);
  emit_event_arg("SCRUB", arg);
}

static void cb_on_video_rate(void* cls, const float rate)
{
  /* Only 0 (pause) and 1 (resume) are used in practice. */
  if (rate != 0.0f && rate != 1.0f)
  {
    LOGI("airplay video: ignoring rate %.2f", rate);
    return;
  }
  emit_event_arg("RATE", rate == 0.0f ? "0" : "1");
}

static void cb_on_video_stop(void* cls)
{
  LOGI("airplay video: stopped by sender");
  pthread_mutex_lock(&g_lock);
  session_set_mode_locked(MODE_IDLE);
  pthread_mutex_unlock(&g_lock);
  emit_event("HLSSTOP");
}

static float cb_on_video_playlist_remove(void* cls)
{
  double position = 0.0, duration = 0.0;
  float rate = 0.0f;
  emit_event_arg("RATE", "0");
  kodi_playback_state(&position, &duration, &rate);
  LOGI("airplay video: playlist removed at %.1fs", position);
  return (float)position;
}

static void cb_on_video_acquire_playback_info(void* cls, playback_info_t* info)
{
  if (!info)
    return;

  double position = 0.0, duration = 0.0;
  float rate = 0.0f;
  bool playing = kodi_playback_state(&position, &duration, &rate);

  /* One consistent look at the session; the polls come in every few hundred
   * milliseconds from the httpd thread while others are changing it. */
  pthread_mutex_lock(&g_lock);
  const bool video_ended = g_session.notify_video_ended;
  if (video_ended)
  {
    g_session.notify_video_ended = false;
    session_set_mode_locked(MODE_IDLE);
  }
  const session_mode_t mode = g_session.mode;
  const uint64_t video_start_ns = g_video_start_ns;
  if (playing && mode == MODE_VIDEO)
    g_video_seen_playing = true;
  const bool seen_playing = g_video_seen_playing;
  pthread_mutex_unlock(&g_lock);

  /* Something else has taken the player; report the video as finished so the
   * sender closes its session instead of restarting it. */
  if (video_ended)
  {
    LOGI("airplay video: telling the sender the video is finished");
    info->position = -1.0;
    info->duration = -1.0;
    info->rate = 0.0f;
    return;
  }

  /*
   * Kodi cannot be playing the video yet: the player it had open has to be
   * stopped first, which takes long enough that the sender's first polls all
   * land before ours is running. Answering those with whatever the previous
   * stream reported says "ready to play, zero duration", and the sender drops
   * the item and reverts to mirroring. Say we are still loading instead.
   *
   * A duration is what tells the two apart: mirroring is a live stream and
   * has none, a video does. That does mean a genuinely live video stays
   * "loading" until the grace period runs out.
   */
  if (mode == MODE_VIDEO && !seen_playing && duration <= 0.0 &&
      now_ns() - video_start_ns < APX_VIDEO_START_GRACE_NS)
  {
    static uint64_t logged_for;
    if (logged_for != video_start_ns)
    {
      logged_for = video_start_ns;
      LOGI("airplay video: not playing yet, telling the sender we are still loading");
    }

    info->position = 0.0;
    info->duration = 0.0;
    info->rate = 0.0f;
    info->ready_to_play = false;
    info->playback_likely_to_keep_up = false;
    info->playback_buffer_empty = true;
    info->playback_buffer_full = false;
    info->stallcount = 0;
    info->num_loaded_time_ranges = 0;
    info->num_seekable_time_ranges = 0;
    info->loadedTimeRanges = NULL;
    info->seekableTimeRanges = NULL;
    return;
  }

  if (!playing && mode == MODE_VIDEO)
  {
    /* Negative values are how the sender is told the video finished. */
    info->position = -1.0;
    info->duration = -1.0;
    info->rate = 0.0f;
    pthread_mutex_lock(&g_lock);
    session_set_mode_locked(MODE_IDLE);
    pthread_mutex_unlock(&g_lock);
    LOGI("airplay video: playback finished");
    return;
  }

  static unsigned poll_count;
  if (g_verbose && (poll_count++ % 30) == 0)
    LOGI("playback-info: playing=%d pos=%.1f dur=%.1f rate=%.1f", (int)playing, position, duration,
         rate);

  info->duration = duration;
  info->position = position;
  info->rate = rate;
  info->seek_start = 0.0;
  info->seek_duration = duration;
  info->ready_to_play = true;
  info->playback_likely_to_keep_up = true;
  info->playback_buffer_empty = false;
  info->playback_buffer_full = true;
  info->stallcount = 0;
  info->num_loaded_time_ranges = 0;
  info->num_seekable_time_ranges = 0;
  info->loadedTimeRanges = NULL;
  info->seekableTimeRanges = NULL;
}

/* -------------------------------------------------------- socket server */

static int make_listener(const char* path)
{
  struct sockaddr_un addr;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
  {
    LOGI("socket() failed: %s", strerror(errno));
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path))
  {
    LOGI("socket path is too long (%zu, limit %zu): %s", strlen(path),
         sizeof(addr.sun_path) - 1, path);
    close(fd);
    return -1;
  }
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

  /*
   * The mode the socket is created with, rather than fixing it afterwards:
   * between bind() and chmod() it would otherwise carry 0777 & ~umask, and a
   * permissive umask would leave a window where anyone could connect.
   */
  const mode_t old_umask = umask(0177);

  /*
   * A socket left behind by a hard kill has to go, but a live one means
   * another receiver is already running: taking it over would leave two
   * daemons advertising the same device and fighting over the add-on. Tell
   * them apart by trying to connect to it.
   */
  if (access(path, F_OK) == 0)
  {
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0)
    {
      const bool live = connect(probe, (struct sockaddr*)&addr, sizeof(addr)) == 0;
      close(probe);
      if (live)
      {
        LOGI("another receiver is already listening on %s", path);
        umask(old_umask);
        close(fd);
        return -1;
      }
    }
    unlink(path);
  }

  const int bound = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
  const int bind_errno = errno;
  umask(old_umask);
  if (bound < 0)
  {
    LOGI("bind(%s) failed: %s", path, strerror(bind_errno));
    close(fd);
    return -1;
  }
  if (listen(fd, 1) < 0)
  {
    LOGI("listen failed: %s", strerror(errno));
    close(fd);
    return -1;
  }
  /*
   * Owner only, in case the umask above was not honoured -- some platforms
   * ignore it for sockets. The stream here is the mirror of someone's phone
   * screen, and any process that connects also displaces the real client, so
   * it is not something to leave open to every local user.
   */
  chmod(path, 0600);
  LOGI("listening on %s", path);
  return fd;
}

static void* accept_thread(void* arg)
{
  int listener = *(int*)arg;
  while (g_running)
  {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
    {
      if (errno == EINTR)
        continue;
      break;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    /* Keep the socket buffer small on purpose: it bounds how far behind the
     * live edge the consumer can drift. Roughly two keyframes' worth. */
    int sndbuf = 128 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /*
     * Anything still queued was meant for the client being replaced, and
     * would otherwise be written to this one, which has not been told what
     * the stream is yet. The handover goes in behind it so the writer closes
     * the old descriptor at the right point in the sequence.
     */
    struct out_msg* handover = malloc(sizeof(*handover));
    if (!handover)
    {
      close(fd);
      continue;
    }
    handover->kind = OUT_MESSAGE; /* filled in below, under the lock */
    handover->fd = fd;
    handover->size = 0;
    handover->priming = false;
    handover->queued_ns = now_ns();

    /*
     * All of this under g_lock, so nothing can start producing for the new
     * client until the handover is behind it in the queue -- otherwise its
     * first frames would be written to the socket being replaced.
     */
    pthread_mutex_lock(&g_lock);
    reset_client_state_locked();
    handover->kind = OUT_NEW_CLIENT;
    handover->gen = ++g_client_gen;

    pthread_mutex_lock(&g_out_lock);
    out_clear_locked();
    out_push_locked(handover);
    pthread_mutex_unlock(&g_out_lock);

    g_client_connected = true;
    g_have_base = false; /* timeline restarts from the priming keyframe */
    g_last_audio_ns = 0;
    LOGI("client connected");
    /* Publish the format now if it is already known, so the add-on can declare
     * its streams without waiting for the next frame. The call decides for
     * itself whether there is enough to describe; an audio-only session has no
     * video codec, which the old check here mistook for "not ready". */
    send_streaminfo_locked();
    pthread_mutex_unlock(&g_lock);
  }
  return NULL;
}

/* ------------------------------------------------------------------- main */

static void on_signal(int sig)
{
  g_running = 0;
}

static int get_mac(const char* iface_hint, char mac[6])
{
  struct ifaddrs *ifaddr, *ifa;
  int found = 0;
  if (getifaddrs(&ifaddr) != 0)
    return -1;

  for (ifa = ifaddr; ifa && !found; ifa = ifa->ifa_next)
  {
    if (!ifa->ifa_addr)
      continue;
#ifndef __APPLE__
    if (ifa->ifa_addr->sa_family != AF_INET)
      continue;
#endif
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
      continue;
    if (iface_hint && *iface_hint && strcmp(ifa->ifa_name, iface_hint) != 0)
      continue;

#ifdef __APPLE__
    /* The hardware address is a link level entry of its own, listed
     * separately from the interface's IP addresses. */
    if (ifa->ifa_addr->sa_family != AF_LINK)
      continue;
    const struct sockaddr_dl* sdl = (const struct sockaddr_dl*)ifa->ifa_addr;
    if (sdl->sdl_alen != 6)
      continue;
    memcpy(mac, LLADDR(sdl), 6);
    LOGI("using interface %s", ifa->ifa_name);
    found = 1;
#else
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
      continue;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifa->ifa_name);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0)
    {
      memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
      LOGI("using interface %s", ifa->ifa_name);
      found = 1;
    }
    close(s);
#endif
  }
  freeifaddrs(ifaddr);
  return found ? 0 : -1;
}

int main(int argc, char** argv)
{
  /* Read first: it decides how much the library itself logs. */
  g_verbose = getenv("AIRPLAY_DEBUG") != NULL;

  const char* name = getenv("AIRPLAY_NAME");
  const char* sock_path = getenv("AIRPLAY_SOCKET");

  if (!name || !*name)
    name = "Kodi";
  if (!sock_path || !*sock_path)
    sock_path = APX_DEFAULT_SOCKET;

  const char* art_dir = getenv("AIRPLAY_ART_DIR");
  if (art_dir && *art_dir)
  {
    snprintf(g_art_dir, sizeof(g_art_dir), "%s", art_dir);
  }
  else
  {
    const char* slash = strrchr(sock_path, '/');
    if (slash && (size_t)(slash - sock_path) < sizeof(g_art_dir))
      snprintf(g_art_dir, sizeof(g_art_dir), "%.*s", (int)(slash - sock_path), sock_path);
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  memset(&g_info, 0, sizeof(g_info));
  g_info.sample_rate = 44100;
  g_info.channels = 2;

  int listener = make_listener(sock_path);
  if (listener < 0)
    return 1;

  pthread_t th;
  pthread_create(&th, NULL, accept_thread, &listener);

  pthread_t writer;
  if (pthread_create(&writer, NULL, writer_thread, NULL) != 0)
  {
    LOGI("could not start the writer thread");
    return 1;
  }

  char mac[6];
  if (get_mac(getenv("AIRPLAY_IFACE"), mac) != 0)
  {
    LOGI("could not determine a MAC address");
    return 1;
  }

  int dnssd_error = 0;
  g_dnssd = dnssd_init(name, (int)strlen(name), mac, 6, 0, &dnssd_error);
  if (!g_dnssd || dnssd_error)
  {
    LOGI("dnssd_init failed: %d", dnssd_error);
    return 1;
  }

  raop_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.cls = NULL;
  cbs.audio_process = cb_audio_process;
  cbs.video_process = cb_video_process;
  cbs.conn_init = cb_conn_init;
  cbs.conn_destroy = cb_conn_destroy;
  cbs.conn_reset = cb_conn_reset;
  cbs.conn_feedback = cb_conn_feedback;
  cbs.audio_flush = cb_audio_flush;
  cbs.video_flush = cb_video_flush;
  cbs.video_pause = cb_video_pause;
  cbs.video_resume = cb_video_resume;
  cbs.video_reset = cb_video_reset;
  cbs.video_set_codec = cb_video_set_codec;
  cbs.mirror_video_running = cb_mirror_video_running;
  cbs.video_report_size = cb_video_report_size;
  cbs.audio_get_format = cb_audio_get_format;
  cbs.audio_set_volume = cb_audio_set_volume;
  cbs.audio_set_client_volume = cb_audio_set_client_volume;
  cbs.audio_set_metadata = cb_audio_set_metadata;
  cbs.audio_set_coverart = cb_audio_set_coverart;
  cbs.audio_set_progress = cb_audio_set_progress;
  cbs.on_video_play = cb_on_video_play;
  cbs.on_video_scrub = cb_on_video_scrub;
  cbs.on_video_rate = cb_on_video_rate;
  cbs.on_video_stop = cb_on_video_stop;
  cbs.on_video_playlist_remove = cb_on_video_playlist_remove;
  cbs.on_video_acquire_playback_info = cb_on_video_acquire_playback_info;
  cbs.report_client_request = cb_report_client_request;
  cbs.passwd = cb_passwd;
  cbs.display_pin = cb_display_pin;
  cbs.register_client = cb_register_client;
  cbs.check_register = cb_check_register;
  cbs.export_dacp = cb_export_dacp;

  g_raop = raop_init(&cbs);
  if (!g_raop)
  {
    LOGI("raop_init failed");
    return 1;
  }

  raop_set_log_callback(g_raop, cb_log, NULL);
  /* lib/logger.h: ERR=3, WARNING=4, NOTICE=5, INFO=6, DEBUG=7. Anything above
   * the level set here is dropped, so INFO is the minimum needed to see the
   * library explain its decisions (e.g. refusing a video streaming request). */
  raop_set_log_level(g_raop, g_verbose ? 7 /* LOGGER_DEBUG */ : 6 /* LOGGER_INFO */);

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", (unsigned char)mac[0],
           (unsigned char)mac[1], (unsigned char)mac[2], (unsigned char)mac[3],
           (unsigned char)mac[4], (unsigned char)mac[5]);

  if (raop_init2(g_raop, 1 /* nohold */, mac_str, ""))
  {
    LOGI("raop_init2 failed");
    return 1;
  }

  /*
   * Off unless asked for. With it enabled, apps that support the video
   * streaming protocol stop mirroring and hand over a playlist instead, so it
   * changes behaviour for anyone who was happy with mirroring.
   */
  /*
   * Empty or unset means the receiver is open to anyone on the network, which
   * is what it was before this was configurable.
   *
   * The add-on sends the password on stdin rather than in the environment: a
   * process environment stays readable through /proc for as long as it runs,
   * and this is a secret someone typed. AIRPLAY_PASSWORD remains for running
   * the receiver by hand.
   */
  if (getenv("AIRPLAY_PASSWORD_STDIN"))
  {
    if (fgets(g_password, sizeof(g_password), stdin))
    {
      char* end = strpbrk(g_password, "\r\n");
      if (end)
        *end = '\0';
      else if (strlen(g_password) == sizeof(g_password) - 1)
        LOGI("password is longer than %zu characters and has been cut short; "
             "the sender will not accept the rest of it",
             sizeof(g_password) - 1);
    }
    fclose(stdin);
  }
  else
  {
    const char* password = getenv("AIRPLAY_PASSWORD");
    if (password)
      snprintf(g_password, sizeof(g_password), "%s", password);
  }
  if (g_password[0])
    LOGI("client access password set");

  const char* reg = getenv("AIRPLAY_REGISTRY");
  if (reg && *reg)
    snprintf(g_registry_path, sizeof(g_registry_path), "%s", reg);
  const char* pairing = getenv("AIRPLAY_PAIRING");
  g_require_pairing = pairing && *pairing && *pairing != '0' && g_registry_path[0];

  const char* mirror = getenv("AIRPLAY_MIRRORING");
  g_offer_mirroring = !(mirror && *mirror && *mirror == '0');

  const char* mira = getenv("AIRPLAY_MIRROR_AUDIO");
  g_mirror_audio = !(mira && *mira && *mira == '0');

  const char* vol = getenv("AIRPLAY_VOLUME");
  g_volume_control = vol && *vol && *vol != '0';

  const char* hls = getenv("AIRPLAY_HLS");
  const bool hls_enabled = hls && *hls && *hls != '0';
  g_hls_enabled = hls_enabled;
  if (hls_enabled)
  {
    /* Without this the library rejects AirPlay video requests outright. */
    raop_set_plist(g_raop, "hls", 1);

    /*
     * Accepting the protocol is not enough: the sender decides what to offer
     * from the advertised feature bits, and the defaults leave both of these
     * clear. Without them an app hands over only the audio track of the video
     * it is playing, which is what "AirPlay video" looks like when it silently
     * degrades. Must be set before the service is registered.
     */
    /*
     * Bit 0 claims general AirPlay video support, bit 4 HTTP live streaming.
     * Both are needed for an app to hand over a video stream; bit 4 alone
     * leaves YouTube sending audio only.
     *
     * A sender was once seen negotiating a RAOP audio session and tearing it
     * down again immediately with these advertised, but that turned out to
     * follow a mirroring session in the same process rather than the feature
     * bits: audio streams correctly with both set from a clean start.
     */
    dnssd_set_airplay_features(g_dnssd, 0, 1); /* AirPlay video */
    dnssd_set_airplay_features(g_dnssd, 4, 1); /* HTTP live streaming */

    /* Used when selecting audio and subtitle renditions from a master playlist. */
    const char* lang = getenv("AIRPLAY_LANG");
    raop_set_lang(g_raop, lang && *lang ? lang : "en", "", "");
  }

  /*
   * Accept H.265 as well as H.264 for mirroring.
   *
   * A sender picks the codec from what the screen it is drawing on can take,
   * and for a 4K one it picks H.265. Without this bit it is not allowed to,
   * so rather than fall back it sends an empty packet and gives up -- which
   * is mirroring failing outright on any 4K display. Kodi decodes both, and
   * everything here already handles either: VPS as well as SPS and PPS, the
   * HEVC keyframe types, and the right filler NAL for keepalives.
   */
  dnssd_set_airplay_features(g_dnssd, 42, 1); /* SupportsScreenMultiCodec */

  if (!g_offer_mirroring)
  {
    /* Bit 7 is what a sender looks at to decide it can mirror a screen here. */
    dnssd_set_airplay_features(g_dnssd, 7, 0);
    LOGI("screen mirroring not offered");
  }

  /*
   * Tell the sender what it is actually drawing on. Left unset, the library
   * claims 1920x1080 at 60Hz, and a sender told the wrong refresh rate or
   * size encodes for a screen that is not there.
   */
  if (g_require_pairing)
  {
    /* Anything below 10000 means "make up a new one each time". */
    raop_set_plist(g_raop, "pin", 0);
    LOGI("pairing required; known devices in %s", g_registry_path);
  }

  const char* w = getenv("AIRPLAY_WIDTH");
  const char* h = getenv("AIRPLAY_HEIGHT");
  const char* hz = getenv("AIRPLAY_REFRESH");
  if (w && h && atoi(w) > 0 && atoi(h) > 0)
  {
    raop_set_plist(g_raop, "width", atoi(w));
    raop_set_plist(g_raop, "height", atoi(h));
    if (hz && atoi(hz) > 0)
      raop_set_plist(g_raop, "refreshRate", atoi(hz));
    LOGI("display reported as %sx%s@%sHz", w, h, hz && *hz ? hz : "60");
  }

  unsigned short tcp[3] = {0, 0, 0};
  unsigned short udp[3] = {0, 0, 0};
  raop_set_tcp_ports(g_raop, tcp);
  raop_set_udp_ports(g_raop, udp);

  unsigned short raop_port = raop_get_port(g_raop);
  raop_start_httpd(g_raop, &raop_port);
  raop_set_port(g_raop, raop_port);
  raop_set_dnssd(g_raop, g_dnssd);

  if (dnssd_register_raop(g_dnssd, raop_port) != 0)
    LOGI("warning: dnssd_register_raop failed");
  if (dnssd_register_airplay(g_dnssd, raop_port) != 0)
    LOGI("warning: dnssd_register_airplay failed");

  LOGI("receiver \"%s\" ready on port %u (features 0x%llx, video streaming %s)", name,
       (unsigned)raop_port, (unsigned long long)dnssd_get_airplay_features(g_dnssd),
       hls_enabled ? "enabled" : "disabled");

  uint64_t last_slow_ns = 0;
  while (g_running)
  {
    pthread_mutex_lock(&g_lock);
    const bool busy = g_session.mode != MODE_IDLE || g_client_connected;
    if (g_session.mode == MODE_MIRROR && g_client_connected && g_primed && g_have_base &&
        g_last_video_send_ns && now_ns() - g_last_video_send_ns > APX_KEEPALIVE_AFTER_NS)
    {
      send_keepalive_locked();
    }
    const bool stop_now = g_mirror_stop_pending_ns &&
                          now_ns() - g_mirror_stop_pending_ns > APX_MIRROR_STOP_GRACE_NS;
    if (stop_now)
      g_mirror_stop_pending_ns = 0;
    pthread_mutex_unlock(&g_lock);

    if (stop_now)
    {
      LOGI("mirroring stopped and no video followed, ending the session");
      end_session();
    }

    /*
     * The keepalive has to be checked often, but only while there is a stream
     * to keep alive. With no session there is nothing here worth waking a
     * sleeping box twenty times a second for.
     */
    usleep(busy ? APX_WATCHDOG_TICK_US : APX_WATCHDOG_IDLE_US);

    /* Everything below only needs looking at about once a second. */
    const uint64_t now = now_ns();
    if (now - last_slow_ns < 1000000000ull)
      continue;
    last_slow_ns = now;

    pthread_mutex_lock(&g_lock);

    /*
     * An audio session that has never delivered a frame is not playing, it
     * just picked us. Close it rather than leave Kodi running a clock over an
     * empty stream; the sender pressing play offers it again.
     */
    /*
     * Mirroring that cannot be started again. A stream can only be joined at
     * a keyframe, and the sender sends one when mirroring starts and then, in
     * practice, never another -- thousands of frames later there is still
     * only the first. So once the buffered run has been dropped and the
     * player has gone, which is what stopping playback locally does, there is
     * no way back into the stream and waiting achieves nothing.
     *
     * Dropping the sender's connections ends the session, which at least
     * leaves the phone agreeing that it is no longer mirroring -- rather than
     * showing that it is while nothing arrives. Starting it again from the
     * phone gets a fresh keyframe and works.
     */
    const bool unstartable = g_session.mode == MODE_MIRROR && !g_client_connected &&
                             !g_session.player_open && g_unstartable_since_ns &&
                             now_ns() - g_unstartable_since_ns > APX_UNSTARTABLE_GRACE_NS;
    if (unstartable)
    {
      g_unstartable_since_ns = 0;
      pthread_mutex_unlock(&g_lock);
      LOGI("mirroring cannot be rejoined without a keyframe, ending the session");
      raop_remove_known_connections(g_raop);
      continue;
    }

    const bool never_started = g_session.mode == MODE_AUDIO && g_session.player_open &&
                               !g_audio_session_frames && g_audio_session_ns &&
                               now_ns() - g_audio_session_ns > APX_AUDIO_NOSTART_NS;
    if (never_started)
    {
      LOGI("audio session delivered nothing, closing it");
      pthread_mutex_unlock(&g_lock);
      end_session();
      continue;
    }

    pthread_mutex_unlock(&g_lock);
  }

  LOGI("shutting down");
  pthread_cond_broadcast(&g_out_cv);
  pthread_join(writer, NULL);
  dnssd_unregister_raop(g_dnssd);
  dnssd_unregister_airplay(g_dnssd);
  raop_stop_httpd(g_raop);
  raop_destroy(g_raop);
  dnssd_destroy(g_dnssd);
  close(listener);
  unlink(sock_path);
  return 0;
}
