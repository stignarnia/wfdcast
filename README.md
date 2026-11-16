# WFD Cast

> [!WARNING]
> This project was written with the help of AI.

A small KDE app that casts your screen and sound to a Wi-Fi Display
(Miracast) TV from Linux, with KDE's own screen picker choosing what to share:
a screen, a window, or a new virtual screen.

## Why

Miracast sinks such as smart TVs often host their own Wi-Fi Direct network
(they are the "group owner") and only accept senders that *join* it. The stock
Linux stack can't do that:

- **NetworkManager** only does Wi-Fi Direct *negotiation*, never "join an
  existing group". A TV that won't negotiate makes it time out every time.
  GNOME Network Displays and FluxCast both go through this path.
- **wpa_supplicant's Wi-Fi Direct join** is strict: some TVs (seen on a
  MediaTek-based Sony BRAVIA) drop the Wi-Fi Direct tag from their network
  right after you press Accept, and wpa_supplicant then refuses to join.
  Windows ignores the missing tag and connects.

WFD Cast does what Windows does, without touching your normal Wi-Fi:

1. It adds a second, virtual Wi-Fi interface and joins the TV's network as a
   regular client (WPS push-button the first time, the saved passphrase after
   that).
2. Its join request carries a Wi-Fi Direct tag with your computer's Wi-Fi
   Direct address, so the TV treats it as the Miracast sender that asked to
   join.
3. The TV connects back on TCP 7236. WFD Cast speaks Miracast's RTSP and
   streams H.264 + AAC in MPEG-TS over RTP.

It never asks the TV for HDCP, which some TVs advertise but can't complete,
costing about 45 seconds per connection on Windows.

## Features

- KDE's picker chooses the source: screen, window or new virtual screen.
- Sound: on the TV only (a virtual output becomes the default while casting),
  on the laptop and the TV, or none.
- Hardware encoding on AMD/Intel (VA-API) or NVIDIA (NVENC), or software (x264).
- 720p/1080p at 30/60 fps. The bitrate can be changed while casting.
- Remembers the TV: after the first Accept, later casts connect without it.
- Every tunable (ports, timeouts, names, encoder speed/quality…) is in the UI.

## Requirements

- KDE Plasma 6 on Wayland, NetworkManager with wpa_supplicant, PipeWire.
- A Wi-Fi card that supports a second client interface next to Wi-Fi Direct
  (`iw phy` → `#{ managed, P2P-client } <= 2`).
- Build: CMake, extra-cmake-modules, Qt 6, Kirigami, KI18n, KConfig,
  KCoreAddons, GStreamer development files.
- Run: GStreamer good/bad/ugly/libav plugins and the PipeWire plugin,
  python-gobject, `iw`, `pactl`, polkit.

## Install

```bash
sudo ./install.sh
```

This keeps a copy of the source in `/usr/local/src/wfdcast` and adds a pacman
hook that rebuilds WFD Cast after Qt, KDE Frameworks or GStreamer updates (on
other distributions, run `sudo /usr/local/lib/wfdcast/rebuild` after those
updates). It puts `wfdcast` and its helper in `/usr/local`, adds a polkit action (the
app asks for your password once per session to set up the Wi-Fi link), and,
if firewalld is running, a `wfdcast` zone. That zone allows only DHCP replies;
the RTSP port is opened at runtime, only while casting, only on the virtual
interface.

Remove it with `sudo ./uninstall.sh` (add `--purge` to also delete settings
and the saved TV pairing).

## Use

1. On the TV, open Screen mirroring. If it was already open, close and reopen
   it.
2. Click **Cast to TV**, pick what to share in KDE's dialog, and authenticate.
3. The first time, choose your TV, then press **Accept** on the TV.

## How it is split

- `src/`: the app (C++ / Kirigami). Portal picker, audio routing, GStreamer
  pipeline, Miracast RTSP server, and the controller.
- `helper/wfdcast-helper.py`: the root part (Python), started through pkexec. It
  creates and removes the virtual interface, does the join, DHCP and firewall,
  and talks to the app with JSON lines. It never disconnects or reconfigures
  your main Wi-Fi.

## License

MIT
