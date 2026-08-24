# inputstream.airplay

An AirPlay receiver for Kodi: screen mirroring, video streamed from apps, and
audio, played through Kodi's own pipeline so hardware decoding, the normal
renderer and the usual playback controls all apply.

Video protected by DRM cannot be received. The sender blanks protected frames
before they leave the phone, so no receiver gets them.

## How it fits together

Three pieces, because a binary add-on alone cannot do the job:

| | |
|---|---|
| `airplay-receiver` | A daemon linking UxPlay's protocol library. It publishes the Zeroconf records, terminates the AirPlay session, and hands decrypted access units over a unix socket. |
| `inputstream.airplay.so` | An inputstream add-on that reads that socket, so mirroring reaches VideoPlayer as an ordinary live stream. |
| `default.py` | A service add-on. A binary add-on can neither start at boot nor start playback, so this keeps the daemon alive and drives the player from its events. |

The protocol runs in a separate process on purpose: it parses hostile network
input, and a fault there stops a helper rather than taking Kodi down.

## Settings

| Setting | Default | |
|---|---|---|
| Stream video from apps instead of mirroring | off | Apps that support it hand over a playlist rather than mirroring the screen. Video then plays at full quality and the phone's screen can switch off, but only some apps offer it. |
| Let the sender change the volume | on | The phone's volume slider moves Kodi's, and starts where Kodi already is. |
| Password | empty | Required before a sender may connect. |
| Ask new devices to pair | off | Shows a four digit code the first time a device connects and remembers it afterwards, as an Apple TV does. |
| Play sound from mirroring | on | Sound only reaches Kodi while the phone is making some, and the gaps can make the player rebuffer on a Kodi without the patches below. |
| Offer screen mirroring | on | Mainly a diagnostic. A phone will not offer the device at all for photos without it. |
| Start mirroring at the live edge | off | Needs a Kodi that supports it; ignored otherwise. |
| How far behind the phone to stay | 100 ms | Smaller follows the phone more closely but leaves less room for sound to arrive unevenly. |

## Latency, and the Kodi patches

Out of the box on an unmodified Kodi this works, but mirroring runs about a
second to a second and a half behind the phone, and mirroring *with sound* can
drift further, because Kodi treats a live stream that goes quiet as a stream in
trouble and pauses to rebuffer -- time a live stream never wins back.

Four changes to Kodi bring that down to roughly a quarter of a second. They are
not required, and the add-on works without them:

* `INPUTSTREAM_LOW_LATENCY_LIVE` -- start a live stream at its live edge rather
  than at the first frame that decoded.
* A `liveedgemargin` stream property, so the source says how much room it wants.
* Do not buffer a stalled stream that asked for the live edge. A mirrored screen
  nobody is touching legitimately sends nothing.
* Do not leave the clock slowed once an audio stream goes away.

## Building

Kodi's standard add-on build:

```sh
git clone https://github.com/xbmc/xbmc
git clone https://github.com/popcornmix/inputstream.airplay

mkdir -p xbmc/cmake/addons/addons/inputstream.airplay
echo "inputstream.airplay $PWD/inputstream.airplay" \
  > xbmc/cmake/addons/addons/inputstream.airplay/inputstream.airplay.txt

cmake -B build \
      -DADDONS_TO_BUILD=inputstream.airplay \
      -DADDON_SRC_PREFIX=$PWD \
      -DADDONS_DEFINITION_DIR=$PWD/xbmc/cmake/addons/addons \
      -DCMAKE_INSTALL_PREFIX=$PWD/xbmc/addons \
      xbmc/cmake/addons
cmake --build build
```

Needs OpenSSL and libplist. UxPlay is downloaded during configure; to build
without network access, unpack it yourself and pass
`-DUXPLAY_SOURCE_DIR=/path/to/UxPlay`.

A Debian package can be built with `dpkg-buildpackage`.

## Troubleshooting

Turn on **Verbose logging** in the add-on's settings for the daemon's full
protocol logging plus per-packet accounting. It is verbose enough to swamp a
log, so turn it off again afterwards. The daemon's output is folded into
`kodi.log`, so a log attached to a bug report carries it.

Useful lines: `session: X -> Y` on every mode change, `primed client with N
buffered frames` when mirroring starts, and `consumer is behind` if Kodi is not
keeping up.

The daemon keeps its socket, cover art and list of paired devices in the
add-on's profile directory (`userdata/addon_data/inputstream.airplay/`).

Kodi's transport controls drive the phone over DACP, which needs
`avahi-browse` on the box to find the sender; without it everything else still
works and the log says so once. Pausing from the phone, and the progress
reported back during app video, go through JSON-RPC -- over the loopback
server if "allow remote control from applications on this system" is on, and
in-process if it is not.

## Licence

GPL-3.0-or-later, as installed.

The two licences in this tree are not a contradiction. Every file written for
this add-on is GPL-2.0-or-later, which is what its SPDX header says and what
lets the code be reused in Kodi itself. `addon.xml` and `LICENSE.md` describe
the add-on as built and distributed, which is GPL-3.0-or-later because of what
it links.

The build fetches UxPlay's protocol library, pinned by commit and checksum in
`CMakeLists.txt`, and uses only `lib/` from it -- nothing is patched, so moving
to a newer UxPlay is a two line change. Most of that library is
LGPL-2.1-or-later, but its `playfair` component is GPL-3.0-or-later, which is
what makes the combined work GPL-3.0-or-later.

`playfair` replays key material authored by Apple in order to complete the
FairPlay handshake. That is what makes an AirPlay receiver possible at all, and
it is worth being aware of before redistributing binaries.

Upstream UxPlay: https://github.com/FDH2/UxPlay
