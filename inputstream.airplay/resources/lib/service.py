# The AirPlay service: keeps the receiver daemon alive and drives Kodi's
# player on its behalf. default.py is the entry point that runs it.
#
# Binary add-ons cannot autostart and an inputstream instance only exists
# while something is playing, so this keeps the AirPlay daemon alive. It is
# the same shape LibreELEC uses for its own nqptp add-on.
#
# It also starts and stops playback on the daemon's behalf. That cannot be
# done from the daemon itself: JSON-RPC Player.Open runs a VFS existence
# check which rejects "airplay://", and only the Python API can attach the
# inputstream listitem property that routes the URL to this add-on.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import base64
import os
import queue
import subprocess
import threading
import time

import json
import socket
import urllib.error
import urllib.request
from urllib.parse import urlparse

import xbmc
import xbmcaddon
import xbmcgui
import xbmcvfs

ADDON = xbmcaddon.Addon()
ADDON_ID = ADDON.getAddonInfo('id')
ADDON_PATH = xbmcvfs.translatePath(ADDON.getAddonInfo('path'))
PROFILE = xbmcvfs.translatePath(ADDON.getAddonInfo('profile'))

# A unix socket path is limited to 104 bytes on macOS, and a profile directory
# under a long home directory name can use most of that on its own, so the
# socket goes somewhere short instead. Both halves resolve this same rule
# independently; see the note in proto.h. Everything that has no length limit
# -- cover art, the list of paired devices -- stays in the profile.
SOCKET = xbmcvfs.translatePath('special://temp/airplay.sock')


def receiver_path():
    """Where the daemon binary is.

    Under bin/, because extracting the zip does not carry the execute bit and
    Kodi restores it only for files in that directory.

    A zip install keeps everything in one directory, but a distribution
    package splits the add-on: the library and the daemon go to libdir while
    addon.xml and this script -- which is what getAddonInfo('path') points at
    -- go to datadir. special://xbmcbinaddons is the binary half, so try there
    first and fall back to the one-directory layout.
    """
    for root in (xbmcvfs.translatePath('special://xbmcbinaddons/{}'.format(ADDON_ID)),
                 ADDON_PATH):
        binary = os.path.join(root, 'bin', 'airplay-receiver')
        if os.path.exists(binary):
            return binary
    # Nothing found: name the place it should have been, so the failure that
    # follows says something useful.
    return os.path.join(ADDON_PATH, 'bin', 'airplay-receiver')

STREAM_URL = 'airplay://mirror'


def read_rpc_reply(sock, want_id):
    """Read until the reply to this request arrives, ignoring announcements.

    Kodi subscribes every new JSON-RPC connection to all announcements and
    offers no way to opt out, so what comes back can be a notification -- or
    several -- interleaved with, or ahead of, the answer. The request that
    causes one is the awkward case: asking the player to resume produces
    Player.OnResume, which can arrive first.

    A reply carries the request's id; a notification carries a method and no
    id. raw_decode reads one value at a time, so both cases fall out of it, and
    a reply split across two segments simply needs another read rather than
    looking like malformed JSON.
    """
    decoder = json.JSONDecoder()
    raw = ''
    while len(raw) < 1024 * 1024:
        chunk = sock.recv(65536)
        if not chunk:
            break
        raw += chunk.decode('utf-8', 'replace')
        while True:
            raw = raw.lstrip()
            if not raw:
                break
            try:
                value, end = decoder.raw_decode(raw)
            except ValueError:
                break  # not a whole value yet; read more
            raw = raw[end:]
            if isinstance(value, dict) and value.get('id') == want_id:
                return value
    raise ValueError('no reply to id {}'.format(want_id))


# Whether to call JSON-RPC in-process rather than over the loopback socket.
# Decided once at startup; see use_inprocess_rpc().
_INPROC_RPC = [False]


def kodi_rpc(method, params=None):
    """Call Kodi's JSON-RPC.

    Over the loopback socket by preference. Deliberately not
    xbmc.getCondVisibility() or other GUI-side helpers: those take locks that
    deadlock against the player being torn down. The JSON-RPC server runs on
    its own thread, so a call to it cannot wedge this one -- and if it ever
    did, the timeout below ends it.

    xbmc.executeJSONRPC() reaches the same handlers without the server, and is
    the fallback for a user who has turned remote control off. It runs them on
    this thread, which is why it is not the first choice.
    """
    request = {'jsonrpc': '2.0', 'id': 1, 'method': method}
    if params is not None:
        request['params'] = params
    payload = json.dumps(request)

    if _INPROC_RPC[0]:
        try:
            return json.loads(xbmc.executeJSONRPC(payload))
        except ValueError as error:
            log('json-rpc {} failed: {}'.format(method, error), xbmc.LOGDEBUG)
            return None

    try:
        with socket.create_connection(('127.0.0.1', 9090), timeout=2) as sock:
            sock.sendall(payload.encode('utf-8'))
            return read_rpc_reply(sock, request['id'])
    except (OSError, ValueError) as error:
        log('json-rpc {} failed: {}'.format(method, error), xbmc.LOGDEBUG)
        return None


def kodi_setting(name):
    """Read one of Kodi's own settings, in-process.

    Deliberately not through kodi_rpc: this is used to decide how kodi_rpc
    should work.
    """
    try:
        reply = json.loads(xbmc.executeJSONRPC(json.dumps(
            {'jsonrpc': '2.0', 'id': 1, 'method': 'Settings.GetSettingValue',
             'params': {'setting': name}})))
        return (reply.get('result') or {}).get('value')
    except (ValueError, AttributeError):
        return None


def use_inprocess_rpc():
    """Decide how to reach JSON-RPC.

    The loopback server is off unless "allow remote control from applications
    on this system" is on. Without this check, turning that off silently cost
    play/pause, volume and the video-streaming progress reports. Re-checked
    whenever any setting changes, since it can be turned off mid-session.
    """
    enabled = kodi_setting('services.esenabled')
    if enabled is None:
        return
    inproc = enabled is False
    if inproc != _INPROC_RPC[0]:
        _INPROC_RPC[0] = inproc
        log('remote control is {}, so JSON-RPC will be called {}'.format(
            'off' if inproc else 'on', 'in-process' if inproc else 'over the loopback socket'))


def warn_about_core_airplay():
    """Say something if Kodi's own AirPlay receiver is also running.

    Both advertise under the same device name, so the sender shows two
    identical targets and picking the wrong one does nothing recognisable.
    Nothing here can turn it off -- that is the user's call -- but an
    unexplained duplicate is worth a line in the log.
    """
    for name, what in (('services.airplay', 'AirPlay'), ('services.airtunes', 'AirTunes')):
        if kodi_setting(name) is True:
            log("Kodi's own {} receiver is enabled as well as this add-on; the sender "
                'will show two devices with the same name. Turn it off under '
                'Settings / Services / AirPlay.'.format(what), xbmc.LOGWARNING)


def active_player_id():
    reply = kodi_rpc('Player.GetActivePlayers')
    players = (reply or {}).get('result') or []
    return players[0]['playerid'] if players else None


# A play or pause from the sender that arrived before there was a player to
# apply it to, kept so it can be applied once there is one.
PENDING_RATE = {'play': None}

# Where the sender asked the video to start, applied once the stream is up.
PENDING_SEEK = {'position': 0.0}


def apply_pending_seek():
    """Move to where the sender asked the video to start.

    Waiting for isPlaying() was not enough: it goes true while the stream is
    still opening, and a seek issued then is silently dropped -- a video handed
    over half way through started from the beginning instead. onAVStarted means
    a frame has been rendered, so the stream is really there to seek in.
    """
    position = PENDING_SEEK['position']
    if position <= 1.0:
        return
    PENDING_SEEK['position'] = 0.0
    try:
        log('seeking to {:.1f}s, where the sender handed the video over'.format(position))
        xbmc.Player().seekTime(position)
    except RuntimeError as error:
        log('could not seek to {:.1f}s: {}'.format(position, error), xbmc.LOGWARNING)


def set_playing(play):
    """Set play/pause absolutely.

    Kodi's Python API only offers a toggle, so tracking the current state here
    and flipping it drifts out of step the moment an event is missed -- which
    is how a skip on the phone ended up leaving playback paused. JSON-RPC
    takes the state as a value.

    A sender hands over a video and sets its rate in the same breath, so these
    routinely arrive in the moment before Kodi has the stream open. Dropping
    them then left the phone showing a state Kodi was not in, with no way back
    into step, so one is held over instead.
    """
    playerid = active_player_id()
    if playerid is None:
        PENDING_RATE['play'] = bool(play)
        log('no player for "{}" yet, holding it until there is'.format(
            'play' if play else 'pause'), xbmc.LOGDEBUG)
        return
    PENDING_RATE['play'] = None
    kodi_rpc('Player.PlayPause', {'playerid': playerid, 'play': bool(play)})


def apply_pending_rate():
    """Apply a rate the sender asked for before playback had started."""
    play = PENDING_RATE['play']
    if play is None:
        return
    PENDING_RATE['play'] = None
    playerid = active_player_id()
    if playerid is None:
        return
    log('applying the {} the sender asked for while the stream was opening'.format(
        'play' if play else 'pause'))
    guard_echo()
    kodi_rpc('Player.PlayPause', {'playerid': playerid, 'play': play})


def log(message, level=xbmc.LOGINFO):
    xbmc.log('[{}] {}'.format(ADDON_ID, message), level)


def setting_bool(name, default=False):
    try:
        return xbmcaddon.Addon().getSettingBool(name)
    except (RuntimeError, TypeError):
        return default


def setting_int(name, default=0):
    try:
        return xbmcaddon.Addon().getSettingInt(name)
    except (RuntimeError, TypeError):
        return default


def setting_text(name):
    try:
        return xbmcaddon.Addon().getSetting(name) or ''
    except RuntimeError:
        return ''


def hls_enabled():
    return setting_bool('hls')


_FRIENDLY_NAME = []


def friendly_name():
    """Device name for the AirPlay picker, resolved once and cached.

    This is another GUI info lookup, so it is kept off any path that can run
    while playback is being torn down.
    """
    if not _FRIENDLY_NAME:
        try:
            _FRIENDLY_NAME.append(xbmc.getInfoLabel('System.FriendlyName') or 'Kodi')
        except RuntimeError:
            _FRIENDLY_NAME.append('Kodi')
    return _FRIENDLY_NAME[0]


_DISPLAY = []


def display_mode():
    """The screen the sender is really drawing on, resolved once and cached.

    Another GUI info lookup, so kept off any path that can run while playback
    is being torn down.
    """
    if not _DISPLAY:
        width = height = refresh = ''
        try:
            width = xbmc.getInfoLabel('System.ScreenWidth') or ''
            height = xbmc.getInfoLabel('System.ScreenHeight') or ''
            # "1920x1080 @ 50.000000 Hz"
            mode = xbmc.getInfoLabel('System.ScreenMode') or ''
            if '@' in mode:
                refresh = str(int(round(float(mode.split('@')[1].split('Hz')[0].strip()))))
        except (RuntimeError, ValueError, IndexError):
            pass
        _DISPLAY.append((width, height, refresh))
    return _DISPLAY[0]


def receiver_settings():
    """Settings baked into the receiver's environment when it starts.

    Anything here only takes effect on a restart, so a change to one has to
    bring the receiver back. Not included: the low latency setting, which the
    inputstream side reads afresh each time playback opens.
    """
    return (hls_enabled(),
            setting_bool('pairing', False),
            setting_bool('volumecontrol', True),
            setting_bool('mirroraudio', True),
            setting_bool('mirroring', True),
            setting_bool('debuglog', False),
            setting_text('password'))


def receiver_env():
    env = os.environ.copy()
    xbmcvfs.mkdirs(PROFILE)
    env['AIRPLAY_SOCKET'] = SOCKET
    # Kept out of the environment on purpose; see start_receiver().
    env.pop('AIRPLAY_PASSWORD', None)
    env['AIRPLAY_PASSWORD_STDIN'] = '1'
    env['AIRPLAY_HLS'] = '1' if hls_enabled() else '0'
    env['AIRPLAY_VOLUME'] = '1' if setting_bool('volumecontrol', True) else '0'
    env['AIRPLAY_MIRROR_AUDIO'] = '1' if setting_bool('mirroraudio', True) else '0'
    env['AIRPLAY_MIRRORING'] = '1' if setting_bool('mirroring', True) else '0'
    env['AIRPLAY_PAIRING'] = '1' if setting_bool('pairing', False) else '0'
    env['AIRPLAY_REGISTRY'] = os.path.join(PROFILE, 'paired-devices')
    env['AIRPLAY_ART_DIR'] = PROFILE
    width, height, refresh = display_mode()
    env['AIRPLAY_WIDTH'] = width
    env['AIRPLAY_HEIGHT'] = height
    env['AIRPLAY_REFRESH'] = refresh
    # Advertise under the device's own name so it is recognisable in the
    # iOS AirPlay picker.
    env['AIRPLAY_NAME'] = friendly_name()
    if setting_bool('debuglog', False):
        env['AIRPLAY_DEBUG'] = '1'
        log('verbose receiver logging enabled')
    return env


def start_receiver():
    """Spawn the daemon, and hand it the password down its own stdin.

    Not through the environment: that stays readable through /proc for as long
    as the process runs, and the password is a secret the user typed. stderr
    is folded into stdout so the daemon's diagnostics reach the Kodi log
    instead of the system journal.
    """
    process = subprocess.Popen([receiver_path()], env=receiver_env(),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    try:
        process.stdin.write((setting_text('password') + '\n').encode('utf-8'))
        process.stdin.flush()
    except (OSError, ValueError):
        pass  # it died on the way up; the restart path deals with it
    finally:
        try:
            process.stdin.close()
        except OSError:
            pass
    return process


def forget_devices():
    """Drop every remembered pairing, so each device has to use a PIN again."""
    path = os.path.join(PROFILE, 'paired-devices')
    try:
        os.remove(path)
        removed = True
    except FileNotFoundError:
        removed = True
    except OSError as error:
        log('could not forget paired devices: {}'.format(error), xbmc.LOGERROR)
        removed = False
    if removed:
        log('forgot every paired device')
    xbmcgui.Dialog().notification(ADDON.getAddonInfo('name'),
                                  ADDON.getLocalizedString(30022 if removed else 30023))


# Current track, kept so artwork and tags can be applied together.
TRACK = {'title': '', 'artist': '', 'album': '', 'art': ''}

# The URL currently handed to the player. For mirroring and music that is
# STREAM_URL; for AirPlay video it is the playlist the sender nominated.
PLAYING = {'url': ''}

# An AirPlay video session also negotiates a RAOP audio channel, and it looks
# exactly like a music-only session at the point it is set up -- the video
# handoff only arrives a second or two later. Starting playback immediately
# would open the audio stream and then have the video supersede it, and the
# first one tearing down takes the second with it. So hold the audio start
# briefly and drop it if a video handoff turns up.
PENDING_AUDIO = {'due': 0.0}
AUDIO_START_GRACE = 3.0

# A track change arrives as a title and its artwork in separate events, and the
# sender sends a placeholder title a moment before the real one. Holding the
# push briefly coalesces all of that into one update, so the placeholder is
# usually overwritten before anything is shown.
PENDING_INFO = {'due': 0.0}
INFO_PUSH_GRACE = 0.3

# getCurrentWindowDialogId() reports this when no modal dialog is up.
WINDOW_INVALID = 9999

# How long to leave a dialog before looking again, for the work held back
# while one is open.
DIALOG_RETRY_GRACE = 0.5


# When a dialog was last seen. Kodi drops a dialog from its active list before
# the render loop that dialog was driving has unwound, so "no dialog" is not
# yet "safe" -- a stop landing in that gap is run from the loop all the same.
DIALOG_LAST_SEEN = {'at': 0.0}
DIALOG_SETTLE = 1.0


def dialog_open():
    """True while a modal dialog is up, closing ones included.

    GetTopmostModalDialog() counts a dialog that is animating closed, which is
    wanted here: it is still driving the render loop at that point.
    """
    try:
        found = xbmcgui.getCurrentWindowDialogId() != WINDOW_INVALID
    except Exception:
        # Not worth failing a stop over; treat an unknown GUI state as clear.
        return False
    if found:
        DIALOG_LAST_SEEN['at'] = time.time()
    return found


def dialog_blocking():
    """True while it is unsafe to make Kodi tear a player down.

    What this guards is described at PENDING_STOP. The settle period is what
    covers the gap after a dialog closes, which is where this deadlocked when
    only the dialog itself was checked for.
    """
    if dialog_open():
        return True
    return time.time() - DIALOG_LAST_SEEN['at'] < DIALOG_SETTLE


def build_item():
    item = xbmcgui.ListItem(TRACK['title'] or 'AirPlay')
    item.setProperty('inputstream', ADDON_ID)
    item.setProperty('isrealtimestream', 'true')
    # How far behind the source to sit. Small is responsive but leaves little
    # room for audio to arrive unevenly, which is what makes it drop out.
    item.setProperty('liveedgemargin', str(setting_int('liveedgemargin', 100)))
    item.setContentLookup(False)
    # Deliberately no music info tag. Setting one makes Kodi classify the
    # stream as music and hand it to PAPlayer, whose SoftStop fade-out holds a
    # lock that deadlocks against any add-on calling xbmc.getCondVisibility --
    # observed wedging Kodi solid on stop, with plugin.video.youtube's service
    # as the unlucky counterparty. VideoPlayer, which mirroring already uses
    # and which is what inputstream add-ons target, has no such problem.
    if TRACK['artist']:
        item.setLabel2(TRACK['artist'])
    if TRACK['art']:
        item.setArt({'thumb': TRACK['art'], 'fanart': TRACK['art']})
    return item


def info_item():
    """The details that can change without the stream changing.

    Carries the path deliberately: the update is applied only where it matches
    what is playing, so an item without one is dropped in silence rather than
    refused. No info tag, for the reason build_item() gives -- none is needed,
    because the fields below are merged into the item Kodi already has.

    Artwork is always set, empty included, so that a track without any clears
    the last one instead of inheriting it.
    """
    item = xbmcgui.ListItem(TRACK['title'] or 'AirPlay', path=PLAYING['url'])
    item.setLabel2(TRACK['artist'])
    item.setArt({'thumb': TRACK['art'], 'fanart': TRACK['art']})
    return item


def service_pending_info():
    """Show the track details the sender has reported since playback started.

    Runs on the service thread, where the Kodi API is safe, but that alone is
    not enough: updateInfoTag() sends its GUI message synchronously and
    deadlocks if it lands while playback is being torn down. The echo guard is
    set around every stop this add-on makes, so waiting it out is what keeps
    this off that path -- the push is held rather than dropped, since a track
    change that arrives during a stop is still worth showing afterwards.
    """
    due = PENDING_INFO['due']
    if not due or time.time() < due:
        return
    if echoing():
        return
    PENDING_INFO['due'] = 0.0
    if not PLAYING['url']:
        return
    player = xbmc.Player()
    try:
        if not player.isPlaying() or player.getPlayingFile() != PLAYING['url']:
            return
        player.updateInfoTag(info_item())
    except RuntimeError:
        # Stopped between the check and the call, which is exactly the race the
        # guard above cannot close on its own.
        return


def start_playback():
    player = xbmc.Player()

    # The receiver offers the stream again whenever the add-on's connection
    # goes away, and Kodi rebuilding the player is one of the ways that
    # happens -- so acting on every offer can restart the very player that
    # caused it, over and over. If our stream is already the one playing, the
    # add-on reconnects on its own and there is nothing to do here.
    if PLAYING['url'] == STREAM_URL and player.isPlaying():
        try:
            if player.getPlayingFile() == STREAM_URL:
                log('already playing this stream, ignoring the offer', xbmc.LOGDEBUG)
                return
        except RuntimeError:
            pass  # it stopped between the two calls; carry on and open it

    # Opening carries the same hazard as stopping: Kodi replaces a running
    # stream by stopping it first, on the main thread, and a modal dialog runs
    # that from inside its own render loop. See PENDING_STOP. Come back for it
    # rather than risk the deadlock -- there is nothing to gain by starting a
    # session on top of an error the user has not dismissed yet.
    if dialog_blocking():
        PENDING_AUDIO['due'] = time.time() + DIALOG_RETRY_GRACE
        log('a dialog is open, holding the session start until it closes', xbmc.LOGDEBUG)
        return

    log('session started, opening player')

    # Replacing a stream that is still running lets its teardown race the new
    # one and cancel it, so stop first and let it settle.
    if player.isPlaying():
        player.stop()
        # Every millisecond here is a frame the sender buffers before the
        # player attaches, and that backlog becomes startup latency.
        for _ in range(20):
            if not player.isPlaying():
                break
            xbmc.sleep(25)

    PENDING_RATE['play'] = None
    PENDING_SEEK['position'] = 0.0
    PLAYING['url'] = STREAM_URL
    player.play(STREAM_URL, build_item())


# A video handoff held back while a dialog is open, and when to give up on it.
PENDING_HLS = {'url': '', 'start': 0.0, 'due': 0.0, 'expires': 0.0}


def start_hls(url, start_position):
    """Play a playlist the sender nominated.

    This is ordinary HLS, so it bypasses inputstream.airplay entirely and lets
    Kodi demux and decode it the way it would any other stream.
    """
    # See PENDING_STOP. player.play() has Kodi stop whatever is playing first,
    # on the main thread, so it is as dangerous as a stop while a dialog is
    # driving the render loop. This is also the path a DRM failure arrives on
    # -- inputstream.adaptive raises its dialog from the OpenStream below --
    # which makes a second handoff, offered while the first one's dialog is
    # still up, the way this deadlocks in practice.
    if dialog_blocking():
        if not PENDING_HLS['due']:
            PENDING_HLS['expires'] = time.time() + STOP_DEFER_LIMIT
            log('a dialog is open, holding the video handoff until it closes')
        PENDING_HLS.update(url=url, start=start_position,
                           due=time.time() + DIALOG_RETRY_GRACE)
        return
    PENDING_HLS['due'] = 0.0

    item = xbmcgui.ListItem(TRACK['title'] or 'AirPlay')
    # No mime type here. Kodi turns one into a "?mimetype=..." URL option, and
    # the playlist server in the AirPlay library matches request paths exactly,
    # so the decorated URL comes back as "media playlist not found". The .m3u8
    # extension is enough for the stream to be recognised.
    item.setContentLookup(False)

    # Only adaptive manifests go to inputstream.adaptive; it cannot open a
    # plain media file and reports "Unable to determine type of manifest".
    # Senders use both: an app hands over a playlist, while Safari hands over
    # the video element's own URL, which Kodi plays perfectly well itself.
    path = urlparse(url).path.lower()
    if path.endswith('.m3u8') or path.endswith('.mpd'):
        # Left to ffmpeg's HLS demuxer this fails at the first segment: the
        # CDN is https and ffmpeg's TLS errors out here, whereas
        # inputstream.adaptive fetches over Kodi's own HTTP stack.
        item.setProperty('inputstream', 'inputstream.adaptive')
    # Replacing a stream that is still running lets its teardown race the new
    # one and cancel it, so stop first and let it settle.
    player = xbmc.Player()
    if player.isPlaying():
        player.stop()
        for _ in range(20):
            if not player.isPlaying():
                break
            xbmc.sleep(25)

    # Anything held over belongs to the stream being replaced; the sender sets
    # the rate for this one just after handing it over.
    PENDING_RATE['play'] = None
    # Seeking is left until the stream is actually up; see apply_pending_seek.
    PENDING_SEEK['position'] = start_position if start_position > 1.0 else 0.0
    PLAYING['url'] = url
    log('airplay video: playing {} from {:.1f}s'.format(url, start_position))
    player.play(url, item)


# A stop that could not be issued yet, and how long to keep trying.
#
# Kodi runs an add-on's stop from the render loop, and a modal dialog pumps
# that loop itself -- so a stop delivered while one is open is run inside it.
# If the dialog belongs to the player being stopped, the two wait on each
# other: the main thread blocks joining the player thread, and the player
# thread is blocked waiting for the main thread to show its dialog. That is
# reachable in practice, because inputstream.adaptive raises a modal dialog
# from OpenStream when it cannot set up a decryptor, and an AirPlay session
# ending at that moment is exactly when a stop arrives. Holding the stop until
# the dialog closes keeps us off that path; issuing it asynchronously would
# not, since the main thread would still run it from inside the dialog.
PENDING_STOP = {'wanted': False, 'expires': 0.0}
STOP_DEFER_LIMIT = 30.0


def stop_playback():
    player = xbmc.Player()
    if not player.isPlaying():
        PENDING_STOP['wanted'] = False
        return
    try:
        current = player.getPlayingFile()
    except RuntimeError:
        PENDING_STOP['wanted'] = False
        return
    if current and current == PLAYING['url']:
        if dialog_blocking():
            if not PENDING_STOP['wanted']:
                PENDING_STOP.update(wanted=True, expires=time.time() + STOP_DEFER_LIMIT)
                log('a dialog is open, holding the stop until it closes')
            return
        log('session ended, stopping player')
        guard_echo()
        player.stop()
    PENDING_STOP['wanted'] = False
    PLAYING['url'] = ''


def service_pending_hls():
    """Start a video handoff that was held back, once the dialog has gone."""
    due = PENDING_HLS['due']
    if not due or time.time() < due:
        return
    if time.time() >= PENDING_HLS['expires']:
        PENDING_HLS['due'] = 0.0
        log('dialog still open, abandoning the video handoff', xbmc.LOGWARNING)
        return
    start_hls(PENDING_HLS['url'], PENDING_HLS['start'])


def service_pending_stop():
    """Issue a stop that was held back, once the dialog blocking it has gone."""
    if not PENDING_STOP['wanted']:
        return
    if dialog_blocking():
        if time.time() < PENDING_STOP['expires']:
            return
        # Give up rather than force it: whatever is on screen has been there
        # long enough to be someone reading it, and the deadlock this avoids
        # takes Kodi out completely.
        PENDING_STOP['wanted'] = False
        log('dialog still open, abandoning the stop', xbmc.LOGWARNING)
        return
    stop_playback()


# The sender's own remote control. Kodi is only rendering a stream, so pausing
# it locally leaves the phone playing; these are what actually reach the phone.
# Populated by EVENT DACP, cleared when the session ends.
# 'warned' is sticky across sessions: the tool is either installed or not.
# 'port' is a string: it arrives that way from avahi and is only ever
# interpolated into a URL.
DACP = {'id': '', 'token': '', 'host': '', 'port': '', 'warned': False, 'retry_at': 0.0}

# How long to leave it before looking again for a sender that was not there.
DACP_RETRY_GRACE = 10.0

# Commands raised from Kodi's player callbacks, sent from the service thread:
# the callbacks run on a Kodi thread and must not block on the network.
DACP_QUEUE = queue.Queue()

# Playback changes we caused ourselves, acting on what the sender told us, must
# not be echoed back to it as a command. A short window is enough -- the
# callback follows our own JSON-RPC call almost immediately.
ECHO_GUARD = {'until': 0.0}
ECHO_GRACE = 1.5


def guard_echo():
    ECHO_GUARD['until'] = time.time() + ECHO_GRACE


def echoing():
    return time.time() < ECHO_GUARD['until']


def dacp_resolve():
    """Find the sender's DACP endpoint, which it advertises over mDNS.

    The service is named iTunes_Ctrl_<id> for the id the sender gave us. Kodi
    has no add-on API for browsing Zeroconf, so this shells out to avahi.
    """
    if DACP['host']:
        return True
    if not DACP['id']:
        return False
    # A sender that has gone is not going to be found by asking again straight
    # away, and each attempt costs five seconds.
    if time.time() < DACP['retry_at']:
        return False
    wanted = 'iTunes_Ctrl_{}'.format(DACP['id'])
    try:
        out = subprocess.run(['avahi-browse', '-rtp', '_dacp._tcp'],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             timeout=5).stdout.decode('utf-8', 'replace')
    except FileNotFoundError:
        # Not on every Kodi target, and not on LibreELEC by default. The
        # feature simply does not work without it, so say so once rather than
        # leave a dead remote control looking like a bug.
        if not DACP['warned']:
            DACP['warned'] = True
            log('avahi-browse is not installed, so the sender cannot be '
                'remote-controlled from Kodi')
        return False
    except (OSError, subprocess.SubprocessError) as error:
        log('dacp: browse failed: {}'.format(error), xbmc.LOGDEBUG)
        return False
    for line in out.splitlines():
        # =;iface;proto;name;type;domain;host;address;port;txt
        fields = line.split(';')
        if len(fields) < 9 or fields[0] != '=' or fields[3] != wanted:
            continue
        DACP['host'], DACP['port'] = fields[7], fields[8]
        log('dacp: sender remote at {}:{}'.format(DACP['host'], DACP['port']))
        return True
    log('dacp: {} not found on the network'.format(wanted), xbmc.LOGDEBUG)
    DACP['retry_at'] = time.time() + DACP_RETRY_GRACE
    return False


def dacp_send(command):
    """Issue one DACP command to the sender."""
    if not DACP['token'] or not dacp_resolve():
        return
    url = 'http://{}:{}/ctrl-int/1/{}'.format(DACP['host'], DACP['port'], command)
    request = urllib.request.Request(url, headers={'Active-Remote': DACP['token']})
    try:
        urllib.request.urlopen(request, timeout=3).close()
        log('dacp: sent {}'.format(command))
    except (urllib.error.URLError, OSError) as error:
        # The sender goes away without warning; a stale endpoint is normal.
        # Logged at info even so: these only happen on a transport action, so
        # they are rare, and a silent failure here looks like a dead feature.
        log('dacp: {} failed: {}'.format(command, error))
        DACP['host'], DACP['port'] = '', 0


def dacp_worker(monitor):
    """Send queued commands to the sender, on a thread of its own.

    Both halves of this can block for seconds: resolving the endpoint shells
    out to avahi-browse, which waits five seconds to decide nothing is there,
    and the request itself waits three. Doing that on the service loop stopped
    it draining the receiver's events for as long as it took -- long enough,
    with a sender that had gone away, for a video handoff to sit in the queue
    until the sender had given up on it.
    """
    while not monitor.abortRequested():
        try:
            command = DACP_QUEUE.get(timeout=0.5)
        except queue.Empty:
            continue
        try:
            dacp_send(command)
        except Exception as error:  # never let one bad command kill the thread
            log('dacp: {} failed: {}'.format(command, error), xbmc.LOGERROR)


# Transport commands the sender understands, keyed by the notification name a
# keymap can raise. Kodi gives an add-on no way to see a next/prev action --
# the player is not even notified of them for an audio session -- so these
# have to be asked for by name.
DACP_COMMANDS = {
    'next': 'nextitem',
    'previous': 'previtem',
    'volumeup': 'volumeup',
    'volumedown': 'volumedown',
    'mute': 'mutetoggle',
}


class AirPlayPlayer(xbmc.Player):
    """Relays Kodi's transport controls to the sender."""

    def _relay(self, command):
        # Only for our own stream, and not for changes we made ourselves.
        if echoing() or not DACP['token']:
            return
        try:
            if not self.isPlaying() or self.getPlayingFile() != PLAYING['url']:
                return
        except RuntimeError:
            return
        DACP_QUEUE.put(command)

    def onAVStarted(self):
        # The stream is really open now: a frame has been rendered. Both the
        # position the sender asked for and any rate it set while we were
        # getting there can be applied.
        apply_pending_seek()
        apply_pending_rate()

    def onPlayBackPaused(self):
        self._relay('pause')

    def onPlayBackResumed(self):
        self._relay('play')

    def onPlayBackStopped(self):
        # The file is gone by now, so this cannot check what stopped the way
        # the others do. PLAYING['url'] is cleared whenever we stop the stream
        # ourselves, so a stop while it is still set is the user stopping our
        # stream rather than something else finishing.
        if not echoing() and DACP['token'] and PLAYING['url']:
            DACP_QUEUE.put('stop')


# Events are queued here by the reader thread and acted on by the service
# thread. Nothing in the reader may touch the Kodi API: several of those calls
# are synchronous against the GUI, and issuing them from a bare Python thread
# deadlocks against the main thread while it tears playback down.
EVENTS = queue.Queue()


def read_events(process):
    """Pass the daemon's output on: events to the service thread, the rest to
    the log.

    Its diagnostics go to stderr, which start_receiver() folds into this pipe.
    Without that they end up in the journal, and the log a bug report carries
    says nothing about what the receiver was doing. xbmc.log is safe from any
    thread; no other Kodi API called from here would be.
    """
    for raw in iter(process.stdout.readline, b''):
        line = raw.decode('utf-8', 'replace').strip()
        if not line:
            continue
        if line.startswith('EVENT '):
            EVENTS.put(line)
        else:
            log('receiver: {}'.format(line))


def handle_event(line):
    """Act on one event. Runs on the service thread, where the API is safe."""
    if line == 'EVENT PLAY':
        # Mirroring: open immediately. Any wait here is buffered by the sender
        # and shows up as latency for the whole session.
        PENDING_AUDIO['due'] = 0.0
        start_playback()
    elif line == 'EVENT PLAYAUDIO':
        # Audio: hold briefly, since a video handoff may follow and would
        # otherwise be started on top of a session opened moments earlier.
        if hls_enabled():
            PENDING_AUDIO['due'] = time.time() + AUDIO_START_GRACE
        else:
            start_playback()
    elif line == 'EVENT STOP':
        PENDING_AUDIO['due'] = 0.0
        # Nothing left to describe, and the stop below is exactly what a push
        # must not race.
        PENDING_INFO['due'] = 0.0
        PENDING_HLS['due'] = 0.0
        TRACK.update(title='', artist='', album='', art='')
        DACP.update(id='', token='', host='', port='', retry_at=0.0)
        stop_playback()
    elif line.startswith('EVENT DACP '):
        payload = line[11:].split(' ')
        if len(payload) >= 2:
            DACP.update(id=payload[0], token=payload[1], host='', port=0)
    elif line.startswith('EVENT META '):
        try:
            fields = base64.b64decode(line[11:]).decode('utf-8', 'replace').split('\n')
        except (ValueError, TypeError):
            return
        fields += [''] * (3 - len(fields))
        if [TRACK['title'], TRACK['artist'], TRACK['album']] == fields[:3]:
            return
        TRACK.update(title=fields[0], artist=fields[1], album=fields[2])
        PENDING_INFO['due'] = time.time() + INFO_PUSH_GRACE
        log('now playing: {} - {}'.format(TRACK['artist'], TRACK['title']))
    elif line.startswith('EVENT PIN '):
        pin = line[10:]
        log('pairing PIN {}'.format(pin))
        # A notification rather than a modal: this arrives on the service
        # thread while the sender waits, and a dialog would block it.
        xbmcgui.Dialog().notification('AirPlay', 'Enter PIN {}'.format(pin),
                                      xbmcgui.NOTIFICATION_INFO, 30000)
    elif line.startswith('EVENT PAIRED '):
        name = line[13:]
        log('paired with {}'.format(name))
        xbmcgui.Dialog().notification('AirPlay', '{} paired'.format(name),
                                      xbmcgui.NOTIFICATION_INFO, 4000)
    elif line == 'EVENT ART':
        # No artwork for this track. Forget the last one rather than leaving it
        # up against whatever is playing now. The reader strips the line, so an
        # empty argument arrives as a bare event.
        TRACK['art'] = ''
        PENDING_INFO['due'] = time.time() + INFO_PUSH_GRACE
    elif line.startswith('EVENT ART '):
        if TRACK['art'] == line[10:]:
            return
        TRACK['art'] = line[10:]
        PENDING_INFO['due'] = time.time() + INFO_PUSH_GRACE
    elif line.startswith('EVENT HLS '):
        PENDING_AUDIO['due'] = 0.0  # this is a video session after all
        payload = line[10:].split(' ')
        try:
            url = base64.b64decode(payload[0]).decode('utf-8', 'replace')
        except (ValueError, TypeError, IndexError):
            return
        start = float(payload[1]) if len(payload) > 1 else 0.0
        start_hls(url, start)
    elif line == 'EVENT HLSSTOP':
        stop_playback()
    elif line.startswith('EVENT SCRUB '):
        try:
            xbmc.Player().seekTime(float(line[12:]))
        except (ValueError, RuntimeError) as error:
            log('scrub failed: {}'.format(error), xbmc.LOGDEBUG)
    elif line.startswith('EVENT RATE '):
        wants_play = line[11:].strip() == '1'
        log('sender asked to {}'.format('resume' if wants_play else 'pause'), xbmc.LOGDEBUG)
        guard_echo()
        set_playing(wants_play)
    else:
        log('receiver: {}'.format(line), xbmc.LOGDEBUG)


def service_pending_audio():
    due = PENDING_AUDIO['due']
    if due and time.time() >= due:
        PENDING_AUDIO['due'] = 0.0
        start_playback()


def drain_events():
    while True:
        try:
            line = EVENTS.get_nowait()
        except queue.Empty:
            return
        try:
            handle_event(line)
        except Exception as error:  # never let one bad event kill the service
            log('event "{}" failed: {}'.format(line, error), xbmc.LOGERROR)


class AirPlayMonitor(xbmc.Monitor):
    """Notices setting changes so the receiver can be restarted to apply them,
    and relays transport commands raised through NotifyAll()."""

    def onNotification(self, sender, method, data):
        """Relay a transport command raised by NotifyAll() to the sender.

        Bind a key to NotifyAll(inputstream.airplay,next) and it arrives here.
        """
        if sender != ADDON_ID:
            return
        # Kodi may deliver this as the bare message or namespaced as
        # "Other.<message>", depending on how it was raised.
        name = method.split('.')[-1].lower()
        if name == 'forget':
            # The settings screen's button. NotifyAll rather than RunScript:
            # Kodi will not run a script for an add-on that is not one, and
            # this service is already listening.
            forget_devices()
            return
        command = DACP_COMMANDS.get(name)
        if not command:
            log('ignoring unknown notification "{}"'.format(method), xbmc.LOGDEBUG)
        elif DACP['token']:
            DACP_QUEUE.put(command)
        else:
            log('no sender to send "{}" to'.format(name), xbmc.LOGDEBUG)

    def __init__(self):
        super().__init__()
        self.settings = receiver_settings()
        self.restart_wanted = False

    def onSettingsChanged(self):
        # Fires for any settings change, not just this add-on's, which is
        # exactly what makes it the right place to re-read Kodi's own.
        use_inprocess_rpc()
        current = receiver_settings()
        if current != self.settings:
            self.settings = current
            self.restart_wanted = True
            log('settings changed, restarting receiver')


# How long to wait before restarting a daemon that keeps exiting, and how long
# it has to stay up before it counts as healthy again.
RESTART_BACKOFF_MIN = 1.0
RESTART_BACKOFF_MAX = 60.0
RESTART_STABLE_AFTER = 30.0


def main():
    monitor = AirPlayMonitor()
    use_inprocess_rpc()
    warn_about_core_airplay()
    # Held for the life of the service: a Player that goes out of scope stops
    # receiving callbacks.
    player = AirPlayPlayer()
    threading.Thread(target=dacp_worker, args=(monitor,), daemon=True).start()
    process = None
    complained = False
    backoff = 0.0
    retry_at = 0.0
    started_at = 0.0

    while not monitor.abortRequested():
        if monitor.restart_wanted and process is not None and process.poll() is None:
            monitor.restart_wanted = False
            # Asked for, not a failure: the backoff exists to slow down a
            # receiver that keeps dying, and letting settings changes wind it
            # up delays recovery from a real crash later.
            backoff = 0.0
            retry_at = 0.0
            stop_playback()
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
            process = None

        if process is None or process.poll() is not None:
            if process is not None:
                # A daemon that stayed up was working; only a quick exit means
                # something is wrong, so only that should slow the next try.
                if time.time() - started_at > RESTART_STABLE_AFTER:
                    backoff = 0.0
                log('receiver exited with {}, restarting'.format(process.returncode),
                    xbmc.LOGWARNING if backoff < RESTART_BACKOFF_MAX else xbmc.LOGDEBUG)
                process = None

            # Something that fails on every start -- no binary, a socket it
            # cannot bind -- would otherwise be restarted twenty times a
            # second, with a log line each time, for as long as Kodi runs.
            if time.time() < retry_at:
                if monitor.waitForAbort(0.2):
                    break
                continue
            backoff = min(backoff * 2, RESTART_BACKOFF_MAX) if backoff else RESTART_BACKOFF_MIN
            retry_at = time.time() + backoff

            try:
                process = start_receiver()
                started_at = time.time()
                complained = False
                log('started receiver (pid {})'.format(process.pid))
                threading.Thread(target=read_events, args=(process,), daemon=True).start()
            except OSError as error:
                # Keep retrying rather than giving up: the binary can be
                # momentarily absent while the add-on is being updated.
                if not complained:
                    log('could not start receiver: {}'.format(error), xbmc.LOGERROR)
                    complained = True
                continue

        drain_events()
        service_pending_audio()
        service_pending_info()
        service_pending_hls()
        service_pending_stop()

        # waitForAbort specifically, because it is also what lets Kodi deliver
        # this add-on's monitor callbacks -- replacing it with an event of our
        # own silently stopped onNotification and onSettingsChanged arriving.
        #
        # The 0.05 is not what makes that work: waitForAbort loops internally
        # in 100ms slices and pumps callbacks after each one, whatever timeout
        # it is given. It is how quickly the queue above is drained, and the
        # event that starts a session arrives while nothing is playing -- so a
        # longer wait when idle, which is the tempting saving, would land
        # squarely on the session start latency this add-on exists to keep low.
        if monitor.waitForAbort(0.05):
            break

    if process is not None and process.poll() is None:
        log('stopping receiver')
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
