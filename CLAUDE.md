# WFD Cast: notes for contributors and coding agents

## Project rules

- License **MIT**. Every source file starts with
  `SPDX-FileCopyrightText: 2026 stignarnia` + `SPDX-License-Identifier: MIT`.
- This file is tracked in git. **Never put personal data in it or in the code**: no names, hostnames,
  Wi-Fi network names, MAC/IP addresses of real devices, e-mails or home paths. Use placeholders like
  `<tv-p2p-addr>` and `<tv-lan-ip>`.
- **No hardcoded tunables.** Anything a user might want to change is a key in `Settings`
  (`src/settings.cpp` defaults) and is editable in `src/qml/Main.qml`. Code reads values from `Settings`.
- The helper must **never act on the main Wi-Fi**. Only call `Disconnect`, `Reassociate`, WPS or network
  methods on the helper's own wpa_supplicant interface object, after checking its `Ifname`.
  `P2PDevice.Disconnect` on the main interface's object deauthenticates the home Wi-Fi
  (`reason=3 locally_generated=1`). It happened once during development.
- Build: `cmake -S . -B build -G Ninja && cmake --build build`, run `./build/bin/wfdcast`. To test a
  modified helper without installing it, use `WFDCAST_HELPER=$PWD/helper/wfdcast-helper.py ./build/bin/wfdcast`
  (pkexec then uses its generic admin action), but the firewalld zone from `install.sh` must exist.
- Install with `sudo ./install.sh`:
  - It copies the source to `/usr/local/src/wfdcast`, installs `packaging/rebuild` and the pacman hook
    `packaging/90-wfdcast.hook`, which rebuilds after `qt6-base`, `qt6-declarative`, `kirigami`, `ki18n`,
    `kconfig`, `kcoreaddons` or `gstreamer` updates.
  - Then it builds, and installs to `/usr/local`, the polkit policy in `/usr/share/polkit-1/actions`, and
    the firewalld zone `/etc/firewalld/zones/wfdcast.xml`.
  - `sudo ./uninstall.sh [--purge]` removes all of it.

## Architecture

```
Main.qml ── cast (CastController) ── settings (Settings, ~/.config/wfdcastrc, mode 600; TV passphrase in KWallet via QtKeychain, service "wfdcast", key = TV address)
                │
                ├─ Portal       xdg-desktop-portal ScreenCast → KDE picker (screen | window | virtual) → PipeWire fd + node
                ├─ AudioRouter  pactl: "tv" = null sink "wfdcast" made default (monitor recorded) | "mirror" = default sink monitor | "none"
                ├─ WfdServer    RTSP source side on TCP <rtspPort>: M1 OPTIONS, M3 GET_PARAMETER, M4 SET_PARAMETER, M5 trigger SETUP,
                │               answers M2/M6/M7, M16 keep-alive. Never asks for wfd_content_protection (no HDCP).
                ├─ Pipeline     GStreamer in-process: pipewiresrc → (vapostproc+vah264enc | cudaupload+cudaconvertscale+nvh264enc | x264enc)
                │               CBP H.264 + pulsesrc/silence → fdkaacenc|avenc_aac → mpegtsmux alignment=7 → rtpmp2tpay → udpsink
                └─ QProcess "pkexec /usr/local/lib/wfdcast/wfdcast-helper.py"  (JSON lines: commands on stdin, events on stdout)
                        helper (root, Python + Gio): creates <iface> (default wfd0) next to the main Wi-Fi, NM-unmanaged,
                        adds it to wpa_supplicant, adds the P2P IE to its association requests (VendorElemAdd frame 13),
                        sets global WFD IEs (source, RTSP port), puts it in firewalld zone "wfdcast" + opens the RTSP port at runtime,
                        joins the TV (saved passphrase, or P2P join request → TV Accept → legacy WPS-PBC on <iface>),
                        own minimal DHCP client (UDP, broadcast flag), renews the lease, undoes everything on exit/EOF.
```

Helper commands: `start {iface,name,rtsp_port,throughput,*_timeout}`, `scan`, `connect {mac,creds?}`, `reassociate`, `quit`.
Helper events: `ready`, `peer {mac,name,sink,wfd}`, `status`, `accept`, `credentials {ssid,psk,bssid}`, `forget`,
`connected {ip,prefix,tv_ip}`, `disconnected`, `error`, `log`, `bye`.

Flow: Cast → picker → audio setup → RTSP listen (0.0.0.0, the firewall limits it to the helper's interface) →
pkexec helper → remembered TV: `connect` with saved creds (no Accept), else `scan` and the user picks → join → DHCP →
the TV opens RTSP to us (if it doesn't within `tvWaitSeconds`, the helper reassociates; max 3 nudges) → PLAY → pipeline.
If the TV ends a session it usually reconnects by itself; the controller keeps waiting.

Video mode in M4: **no resolution setting**. `WfdServer::chooseFormat` matches the size KDE's picker hands over
(portal stream `size`): an exact mode if the TV advertises it (CEA or VESA bitmap, full WFD tables in
`src/wfdserver.cpp`), else the smallest mode the source fits in, else the biggest; the preferred frame rate (setting)
wins ties, then the closest. The pipeline scales with borders only when there is no exact match. H.264 level is
computed from macroblocks (3.1/3.2/4.1/4.2) and capped to the TV's max level. The reference TV advertises
CEA `0001ffff` and VESA `0f3fffff` (includes 1440×900, 1366×768, 1600×900, 1680×1050; not 1920×1200/1600×1200).
Automatic encoder: the GPU KWin composites on, from kwin-gpuswitch's D-Bus property (`org.kde.KWin /GpuSwitch
org.kde.KWin.GpuSwitch.Current`, PCI address) or else the GPU driving the internal panel / first enabled screen
(`/sys/class/drm/card*-*`); vendor from `/sys/bus/pci/devices/<pci>/vendor`. NVIDIA → NVENC first, AMD/Intel → VA-API
first (per-device `varenderD*` elements when that GPU isn't the default VA device), then the others, then x264. An
encoder that fails at start or within 3 s is replaced by the next one (logged). Reference laptop: KWin on AMD
0000:65:00.0 (renderD128, eDP-1); NVIDIA is 0000:64:00.0 (renderD129, no VA driver).

The encoder speed/quality slider (1..7) maps to x264 `speed-preset` ultrafast..slow, VA-API `target-usage` 7..1,
NVENC `preset` p1..p7. Video bitrate changes live (`bitrate` property on the encoder).

## Lessons from the first real-TV runs (all handled in the code; don't regress them)

- **NetworkManager grabs the new interface** and registers it with wpa_supplicant before it can be marked unmanaged; a
  new device also *briefly shows as unmanaged* before NM takes it. The helper always runs `nmcli device set <if> managed no`
  and requires `unmanaged` to hold for 1 s, then drops NM's leftover wpa_supplicant registration and creates its own.
  If NM keeps managing it, NM scans on it and the Wi-Fi Direct join can't scan (formation failure after ~10 s).
- **wpa_supplicant's search does one full scan, then only social channels 1/6/11**; a TV hosting on 5 GHz is missed.
  Search in short rounds (setting `searchRound`) with `DiscoveryType: progressive`, restarted on `FindStopped`.
- **No D-Bus signal for the TV's answer during a join**: `ProvisionDiscoveryPBCResponse` is not emitted on the join path,
  and wpa_supplicant's temporary group interface can't come up (wlan0 + our interface use both client slots:
  `Could not set interface p2p-wlan0-N flags (UP): Device or resource busy`). WPS on our interface starts when the join
  scan ends (main interface `Scanning` true→false, ≥2 s after the request) + 1 s, on `GroupFormationFailure`, or after
  `answerTimeout`. Starting WPS earlier starves the join scan.
- **DHCP replies must be read at link level** (AF_PACKET): on an interface without an address the kernel input path drops
  the TV's broadcast OFFER before a UDP socket sees it. Requests still go out over UDP.
- **The WPS `Credentials` signal is not emitted** on this path. Read SSID/passphrase/BSSID from the network WPS created
  (`CurrentNetwork` → `Properties`, `CurrentBSS`). This TV's key is a 64-hex PSK (`tvPskHex=true`): reuse it as `ay`
  (strings are quoted by wpa_supplicant = passphrase).
- **Frame pacing**: KWin sends frames only on damage. `pipewiresrc keepalive-time` = one frame interval, else the TV gets
  bursts/gaps and its player stalls ("stuck", not "late"). Pipeline uses the **system clock** (pipewiresrc's clock ticks
  irregularly) and `audiorate`; `pulsesrc` without `do-timestamp` (arrival-time stamps jitter → choppy audio).
- **Live bitrate works** on all three encoders (measured on the wire: 1.4 → ~30 Mbit/s within a second with VA-API/NVENC CBR;
  x264 treats it as an average ceiling, ~10–14 Mbit/s at 17 on desktop content).
- **Audio leftovers**: a killed app left the null sink loaded. The app quits through the event loop on SIGTERM/SIGINT/SIGHUP
  (KSignalHandler) and removes stale `wfdcast` sinks at startup and before each cast.
- **GPU memory**: each encoder first takes KWin's frames as DMA-BUF when it runs on the GPU KWin composites on (VA-API
  imports directly; NVENC via `glupload` → `cudaupload`), then falls back to system memory, then to the next encoder
  (logged; status shows e.g. "vaapi encoder, GPU memory"). A GPU can't import another GPU's buffers.
- **GPU memory verified on both GPUs** (real casts): AMD — KWin DMA-BUF `AR24:0x0200000010401b04` → vapostproc, app at
  ~3 % of one core; NVIDIA (KWin moved with `kwin-gpu nvidia`) — DMA-BUF `AR24:0x0300000000e08014` → glupload →
  cudaupload → NVENC, ~18.5 % of one core, 60 fps, black bars, two send gaps up to 123 ms in 4 s. Why GL on NVIDIA: in
  GStreamer 1.28.9 neither `cudaupload` nor `nvh264enc` accepts DMA-BUF (both accept GLMemory); `vulkanupload` takes
  only system memory / VulkanBuffer (no DMA-BUF import), so a Vulkan route would need a copy. Not tried: a pure GL
  route (glcolorscale + glvideomixer → nvh264enc GLMemory), which might cost less CPU than GL→CUDA interop.
- **KWin offers tiled DMA-BUFs** (`DMA_DRM`, AMD modifier `0x0200000010401b04`) when the caps after `pipewiresrc` allow
  them; vapostproc accepted them and the stream died with "Internal data stream error". That diagnosis was wrong:
  the same error happened with system memory; the cause was vacompositor + a leaky queue. GPU memory is tried again
  (with system-memory fallback).
- **The TV fell minutes behind** (12 min after ~30 min) when scaling/letterboxing ran on the CPU: 2880×1800 → 1080p
  managed ~42 fps while frames were stamped for 60, so the stream ran at ~60% of real time. Now each encoder path
  scales and letterboxes on its own device (AMD: vapostproc to the fitted shape + a second vapostproc add-borders **in BGRx** (its borders are zeros: green in NV12, black in
  RGB) + a vapostproc to NV12 + videorate, because
  vacompositor failed with PipeWire input (cause found later: no input picture yet, see below); cudaconvertscale+cudacompositor ~400 fps,
  multithreaded videoconvert/videoscale + compositor for x264) and the **compositor outputs exactly <fps> frames per
  second of real time** (reuses the last picture, drops extras); no leaky queue in front: with a PipeWire source, leaky
  queue + compositor dies with "Internal data stream error" (reproduced via pipewiresink → pipewiresrc); PipeWire
  itself skips frames when the consumer is busy. videorate is gone. Tested: 20 fps live input → steady 60 fps at 100% real-time speed on all three paths.
- **Letterboxing is live**: the scaler (`fitscale`) asks for the source's own shape so KWin renders straight at that size
  (no stretch-then-squeeze), the compositor (`comp`) places it on a black frame. A pad probe on the scaler's input re-fits caps (`fitcaps`) and
  the picture position (compositor pad `sink_0` xpos/ypos) when the delivered shape changes (resized shared window), without restarting the stream. KWin delivers the
  screen at its native size (2880×1800 on a 2× panel), and reports `pixel-aspect-ratio=1/2147483647` (unset
  placeholder): implausible PARs are treated as 1/1, otherwise the fit collapses to a 2 px line (happened once).
  Unverified: whether KWin delivers a resized window at its new size or keeps rendering into the negotiated one.
- **KWin renders the screencast at whatever size downstream negotiates**: without a caps filter pinning the source
  size right after `pipewiresrc`, KWin delivered 1600×900 already stretched and `add-borders` had nothing to do
  (verified on a frame captured from the wire). The pipeline pins the size the portal reported.
- **The TV stretches every mode to its panel** (its Miracast player's video surface is full-screen and ignores the
  picture's shape): an exact 1440×900 stream filled the 16:9 panel distorted. The app picks the panel shape
  automatically from the biggest HD CEA mode the TV advertises, sends only modes of that shape and letterboxes here
  (1440×900 → 1600×900 with pillar bars, no scaling). No setting. Panel resolution (3840/4096) is not exposed by the
  Miracast reply nor the Cast `eureka_info`; ADB `wm size` would show it.
- **Window leftovers = alpha**: KWin window frames (AR24) have transparent shadow margins with stale colour values.
  NVENC/x264 paths blend onto black (compositor) and are clean (user-confirmed). AMD window path: KWin DMA-BUF →
  vapostproc (scale, BGRA keeps alpha) → `vacompositor` (window on sink_0 over a 16×16 live black picture stretched to
  the frame on sink_1) → BGRA → vapostproc NV12 → vah264enc. Verified offline with the app's own Pipeline class
  (opaque red / transparent stale-white source via pipewiresink): 60 fps, bars and transparent parts black.
  Real casts (user-confirmed): AMD window incl. resizes clean; NVIDIA and x264 window resizes still clean after the
  re-fit change (pad width/height set too).
- **vacompositor fails whenever it has no input picture to compose** (`vaEndPicture: operation failed`, reproduced with
  no inputs at all). With PipeWire the caps arrive before the first frame, so it failed at the first output: that was the
  real cause behind "vacompositor fails with PipeWire input". It also doesn't clear its canvas (bars came out white
  without the black input). A live black picture underneath fixes both; a single black frame with `repeat-after-eos`
  was unreliable, a 1 fps black source added 1 s latency.
- **Never import KWin's DMA-BUF into GL on AMD**: `glupload` of the tiled AR24 buffer logged
  `gst_video_frame_map_id: assertion 'info->finfo->format == meta->format' failed`, then
  `amdgpu: The CS has cancelled because the context is lost. This context is guilty of a hard recovery.` and Mesa
  aborted the app (the TV's player crashed with the stream). The CPU compositor variant reached only 76 fps max and
  the TV fell behind.
- **Window audio**: for a shared window (portal `source_type` 2) the app id comes from KWin's stream name
  (`media.name` = `kwin-screencast-<app id>`, via `pw-dump`). The window's process ids come from a one-shot KWin script
  (`workspace.windowList()`, `desktopFileName` == app id → `pid`, read back from the journal); sink-inputs whose
  `application.process.id` is that pid or a descendant (`/proc/*/status` PPid chain) are moved — like Plasma's audio
  applet. Name matching (binary/name/app id + `.desktop` Exec/Name) is only the fallback when KWin can't be asked.
  Verified on the TV: Firefox PWA window plays only its own sound, Konsole window sends none, "Laptop and TV" works.
  Untested: the name-matching fallback (needs a desktop without KWin). "TV only" →
  null sink `wfdcast` without changing the default; "Laptop and TV" → combine sink `wfdcast_both` (wfdcast + previous
  default). New streams are picked up every 1.5 s; all moved streams go back to the previous default on stop.
- **60 fps helps audio**: the muxer releases audio with each video frame, so 60 fps halves the clump size; TV overflows
  went from ~120 to ~96 per 10 s and the user heard the difference. Default fps is 60. Not fully fixed.
- **Audio on the wire** (20 s captured on the helper's interface): AAC PTS exactly 21.33 ms apart, no gaps, decoded audio
  has no dropouts, audio PTS leads PCR by 126–190 ms, PCR every 67 ms. Sending is bursty: 18 pauses > 50 ms (max 63 ms)
  without any RTP packet → blocky playback on the TV. Fix to do: pace sending (udpsink sync) and don't hold TS packets
  for 7-packet alignment.
- The TV's Accept dialog shows the sender name only after a first successful join (it learns it from the P2P IE in our
  association request); wpa_supplicant's own P2P `DeviceName` is empty on this machine, so the request itself carries none.

## Why the design looks like this: reference TV (Sony BRAVIA DE1, Android TV 14, firmware UKN7.260716.030, MediaTek mt9690)

### TV bugs

1. **Wi-Fi driver drops the Wi-Fi Direct tag after Accept.** When the user presses Accept (`WPS-PBC-ACTIVE`), the driver
   logs `p2pRoleFsmRunEventBeaconUpdate: (P2P ERROR) Update extra IEs for asso resp fail!` (preceded by
   `p2pFuncParseBeaconContent: (RSN ERROR) bcn mfpc:0 …`), and from then on the TV's group is broadcast without its P2P IE.
   Spec-following clients can't join. Windows tolerates it. The driver error fires on every beacon update and is harmless
   for Windows. Not proven: the message says "asso resp" while the missing tag was seen in beacons/probe responses.
2. **Miracast HDCP is advertised but always fails.** The TV offers `wfd_content_protection: HDCP2.1 port=4990`, then its
   own HDCP trusted app in the TEE fails: `TEEC_InvokeCommand failed, FFFF0000` (`ori=4`) → `HCS_ComputeH failed` →
   `HDCP fail in state: 30.6`. It costs **~46 s per connection** (the sender waits ~38 s, the TV waits 8 s in
   `waitForHDCPFinish`), and the stream ends up unencrypted anyway. Seen on every connection. Software `hdcp2x` DRM
   plugin, "HDCP Rx Key is read before", key format 2.2. HDMI-input HDCP is separate hardware (untested). Netflix working
   proves nothing (Widevine, no HDCP link). WFD Cast avoids it by never asking for content protection.
3. **Wi-Fi Direct negotiation is broken.** As autonomous GO the TV answers GO Negotiation with `Status: 1` ("information
   currently unavailable") and never follows up with its own request. Its negotiation mode (`Settings.Global
   enable_p2p_nego=1`) stops the autonomous GO but makes the TV **undiscoverable**. The property
   `vendor.mtk.wifi.DisableAutoGo` effectively does nothing (it only resets the state flag `bCreatego`).
4. **No Microsoft WFD extensions.** It ignores `microsoft_rtcp_capability`, `microsoft_max_bitrate`,
   `microsoft_format_change_capability` etc., so Windows gets no RTCP feedback for bitrate adaptation.

### TV facts

- Receiver app `com.mediatek.miracast` (`/system_ext/app/WFDSinkTest/WFDSinkTest.apk`), opened via Input → Screen mirroring.
  On open it **immediately creates its own persistent group as GO**:
  `P2P-GROUP-STARTED p2p0 GO ssid="DIRECT-xx-<tv name>" freq=5180 … [PERSISTENT]`. GO IP 192.168.49.1, DHCP 192.168.49.x.
- The Accept prompt (`com.android.wifi.dialog`) reliably appears only after the receiver app was (re)started.
  Over ADB: `am force-stop com.mediatek.miracast` + `monkey -p com.mediatek.miracast -c android.intent.category.LEANBACK_LAUNCHER 1`.
  By hand: leave Screen mirroring and reopen it.
- The TV stores a single persistent group for all senders (`numPersistentGroup=1`). Its passphrase survived receiver
  restarts (Windows reconnected without WPS). After a full TV reboot it's unverified.
- The receiver only starts RTSP for a group client with `status == 0` (a connected Wi-Fi Direct device): see
  `MiracastService.updateMacAddress` (decompiled with dexdump). A regular client gets `status: 4`, `mWfdDeviceMac is null`.
  The P2P IE in the association request (Capability attr + Device Info with our P2P device address) makes the TV log
  `AP-STA-CONNECTED <iface addr> p2p_dev_addr=<pc-p2p-addr>` and accept us.
- After association the TV tries RTSP to `<our ip>:<port>` for ~10 s, then stops. Reassociating the helper's interface
  makes it try again.
- M3 answer: `wfd_audio_codecs: LPCM 00000003 00, AAC 00000001 00`,
  `wfd_video_formats: 00 01 01 08 0001ffff 0f3fffff 00000fff 00 0040 0003 13 0500 02D0` (CBP, max level 4.1, all CEA up to
  1080p60), `wfd_client_rtp_ports: RTP/AVP/UDP;unicast 19900 0 mode=play`. Asked for R2/vendor extras it also returns
  `wfd2_video_formats: 00 01 FF 00FF 00000001FFFF 00000F3FFFFF 000000000FFF 00 0000 0000 11 00`,
  `wfd2_audio_codecs: LPCM 00000003 00, AAC 00000001 00`, connector 07, `wfd_uibc_capability`.
- Chromecast receiver at `<tv-lan-ip>:8009`. Its DIAL server is MediaTek's generic one (`modelName: Mediatek_MTXXXX`) and
  answers 404 for YouTube, Netflix and ~80 other app names, so Chromium greys out the "available for specific video sites" entry.

### Why the stock Linux senders fail on this TV

- **NetworkManager:** only does GO Negotiation (Wi-Fi P2P connection type), never "join an existing group". This TV always
  hosts and won't negotiate (bug 3), so NM fails after 45 s (`supplicant-timeout`).
- **wpa_supplicant P2P join** (`P2PDevice.Connect` with `join: true`): it refuses to update the GO's scan entry without the
  P2P IE (`BSS: No P2P IE - skipping BSS <tv-p2p-addr> update for P2P interface`). Before Accept, WPS isn't active; after
  Accept, the tag is gone (bug 1). Failure after 60 s: `P2P-GROUP-FORMATION-FAILURE`. Windows instead associates for WPS
  117 ms after Accept, using the BSSID it already knows.
- **GNOME Network Displays:** uses NM, so it hits the negotiation failure. Its Miracast mode also dies at its own firewalld
  check: it needs zone `P2P-WiFi-Display`, and reading the permanent config needs polkit
  `org.fedoraproject.FirewallD1.config` (Arch ships the server policy; firewalld always passes `AllowUserInteraction`),
  so a password prompt appears and GND gives up after 0.5 s. Its "Chromecast" mode is not mirroring: it uses the
  Default Media Receiver (`CC1AD845`, LOAD of a live Matroska URL) and shows a buffered player that crashes on retry.
- **FluxCast:** negotiation only (`nm.py` AddAndActivateConnection2 `wifi-p2p`; `wpas.py` `Connect` with `go_intent`).
- **MiracleCast:** its own wpa_supplicant, `P2P_CONNECT <mac> pbc` without `join`/`auto`. Its sender side is "not implemented yet".
- **WDS (Intel):** ConnMan-based, unmaintained; whether ConnMan joins is unverified.

### Windows (built-in Win+K) on the same TV: works, but slowly

- First pairing: `P2P-PROV-DISC-PBC-REQ` → Accept → `WPS-PBC-ACTIVE` + the driver error → still `WPS-SUCCESS` →
  `AP-STA-CONNECTED … p2p_dev_addr=…` within 0.6 s. Reconnects reuse the saved persistent-group passphrase (no Accept, no WPS).
- Timeline: ~4 s pairing, RTSP M1–M3 in 0.3 s, then ~38 s waiting on the failing HDCP, 8 s `waitForHDCPFinish`, then video.
  Up to ~2 min from Win+K, because Windows also spends time searching before the TV hears anything.
- Windows' M4 switches between 1024×768p60 (VESA bit 3) and 1920×1080p60 (CEA bit 8) right after connecting.
  Quality was poor in one session and fine after a reconnect: no RTCP feedback (bug 4). Windows has no sender-bitrate setting.
- The test laptop's MT7925 Windows driver reports Wireless Display supported. `netsh` WLAN trace scenarios don't exist on that build.
- To forget the TV on Windows: Settings → Bluetooth & devices → the TV → Remove device. The TV can't forget one sender
  (single persistent group, no root) short of a network reset.

### Other casting findings

- **Chromecast from Linux:** a Chromium-based browser's Cast → Cast screen is real Cast Streaming (low latency). It finds
  the TV via **SSDP**: the TV answers from **UDP source port 1900** (and random ports) to the browser's temporary port. firewalld
  treats those unicast replies to a multicast search as new traffic and drops them. The firewalld service `upnp-client`
  (source-port 1900/udp) fixes it.
- **Miracast vs Chromecast latency:** the Miracast route (this app) felt much better than Chromecast in practice.

## Manual procedure (for debugging the helper), placeholders in `<>`

```bash
sudo iw dev <main-iface> interface add wfd0 type managed
sudo nmcli device set wfd0 managed no
sudo busctl call fi.w1.wpa_supplicant1 /fi/w1/wpa_supplicant1 fi.w1.wpa_supplicant1 CreateInterface a{sv} 2 Ifname s wfd0 Driver s nl80211
# → F = returned object path (verify Ifname "wfd0"); W = main interface's object (has P2PDevice)
sudo busctl set-property fi.w1.wpa_supplicant1 /fi/w1/wpa_supplicant1 fi.w1.wpa_supplicant1 WFDIEs ay 9 0 0 6 0 144 28 68 0 200
#   WFD subelement 0: device info 0x0090 (source, session available), RTSP port 0x1c44 = 7236, throughput 200
# P2P IE for association requests (frame 13): dd <len> 50 6f 9a 09 | 02 02 00 25 00 (Capability) |
#   0d <len16 LE> <pc-p2p-addr 6 bytes> 11 08 00 01 00 50 f2 04 00 01 00 10 11 <len16 BE> <device name>
sudo busctl call fi.w1.wpa_supplicant1 $F fi.w1.wpa_supplicant1.Interface VendorElemAdd iay 13 <n> <bytes…>
# per connection (TV receiver freshly (re)opened):
sudo busctl call fi.w1.wpa_supplicant1 $W fi.w1.wpa_supplicant1.Interface.P2PDevice Find a{sv} 1 Timeout i 30
#   wait for a FRESH P2P-DEVICE-FOUND <tv-p2p-addr> (a stale peer object → "Did not receive correct message arguments")
sudo busctl call fi.w1.wpa_supplicant1 $W fi.w1.wpa_supplicant1.Interface.P2PDevice Connect a{sv} 3 peer o $W/Peers/<tvaddr-no-colons> wps_method s pbc join b true
#   wait for "Received Provision Discovery Response" (D-Bus: ProvisionDiscoveryPBCResponse); the TV shows Accept.
#   Start WPS only now: earlier, wfd0's scans block the join ("Failed to start scan for join").
sudo busctl call fi.w1.wpa_supplicant1 $F fi.w1.wpa_supplicant1.Interface.WPS Start a{sv} 3 Role s enrollee Type s pbc Bssid ay 6 <tv-p2p-addr bytes>
#   → wfd0 "completed" ~10 s after Accept; DHCP; the TV connects to <wfd0 ip>:7236.
```

Shell gotchas: zsh doesn't word-split `$VAR`. `pkill -f <pattern>` can kill its own shell; use `ps -eo pid,args | grep '[p]attern'`.

The first working sender was a Python prototype (RTSP + `gst-launch-1.0`, KDE virtual monitor via the portal with
SourceType 4, 1920×1080). `src/wfdserver.cpp` and `src/pipeline.cpp` are its 1:1 port. The prototype's working RTSP exchange:
`OPTIONS → 200 (Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER)` · sink `OPTIONS` → 200 · `GET_PARAMETER` (M3) →
caps · `SET_PARAMETER` (M4 `00 00 01 08 00000080 …`, AAC, presentation URL, client port) → 200 · `wfd_trigger_method: SETUP`
→ 200 · sink `SETUP …streamid=0 client_port=19900` → 200 (`server_port=5000`) · sink `PLAY` → 200 → stream →
TV logs `Miracast_MediaCodecBridge_Java: onOutputBufferAvailable queue video…`.

## Reference machine state (owner's setup, kept on purpose)

- firewalld zones `home` and `work` have the service `upnp-client` (needed for browser Cast discovery). Undo:
  `firewall-cmd --permanent --zone=<zone> --remove-service=upnp-client && firewall-cmd --reload`.
- GNOME Network Displays was removed. TV ADB is off (re-enable in Developer options for TV logs; laptop keys may still be
  authorized, revoke via "Revoke USB debugging authorizations").
- On the TV, keep **My BRAVIA (`com.sony.dtv.promos`) enabled**: Sony Pictures Core needs it to detect the TV's included
  offer (credits appeared only after enabling it and restarting Core).
- All other debugging changes (temporary polkit rules, zone `P2P-WiFi-Display`, rich rules, `enable_p2p_nego`, virtual
  interfaces, debug log levels, systemd-networkd) were undone and verified.

## Open items

- Real-TV test of the app end to end (first pairing, saved-pairing reconnect, all three encoders, audio modes, live bitrate).
- Modes needing level 4.2 (1080p60, 1440×900p60…) are announced as 4.1 because the TV caps at 4.1, while the encoder may emit 4.2. Check that the TV accepts it.
- Optional: LPCM audio for sinks without AAC. Optional: UIBC (TV remote → mouse/keyboard).
