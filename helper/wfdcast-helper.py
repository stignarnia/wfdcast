#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 stignarnia
# SPDX-License-Identifier: MIT
"""wfdcast-helper.py: the privileged half of WFD Cast.

The app starts this through pkexec. It talks JSON lines: commands on stdin,
events on stdout. It never touches the main Wi-Fi connection: it adds a second
virtual Wi-Fi interface that joins the TV's own Wi-Fi Direct group as a regular
client (WPS push-button the first time, saved passphrase afterwards), while
announcing itself as a Wi-Fi Direct Miracast source in its association request.
Then it gets an address from the TV by DHCP. Everything is undone on exit.

Why not a normal Wi-Fi Direct join: some TVs (seen on a MediaTek-based Sony)
drop the Wi-Fi Direct tag from their group right after the user presses Accept,
and wpa_supplicant's P2P client then refuses to join. Windows ignores the
missing tag; this helper does the equivalent with a regular-client join.
"""
import json
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

try:
    gi.require_version("GLibUnix", "2.0")
    from gi.repository import GLibUnix  # noqa: E402
    signal_add = GLibUnix.signal_add
except (ImportError, ValueError):
    signal_add = GLib.unix_signal_add

WPAS = "fi.w1.wpa_supplicant1"
WPAS_PATH = "/fi/w1/wpa_supplicant1"
IFACE = WPAS + ".Interface"
P2P = IFACE + ".P2PDevice"
WPS = IFACE + ".WPS"
PEER = WPAS + ".Peer"
BSS = WPAS + ".BSS"
NETWORK = WPAS + ".Network"
PROPS = "org.freedesktop.DBus.Properties"
FW_ZONE = "wfdcast"

DEFAULTS = {
    "iface": "wfd0",           # name of the virtual interface we create
    "name": socket.gethostname(),  # name the TV shows for this computer
    "rtsp_port": 7236,         # port the TV connects to (announced in the WFD IE)
    "throughput": 200,         # Mbit/s announced in the WFD IE
    "find_timeout": 30,        # s to look for the TV
    "search_round": 10,        # s per Wi-Fi Direct search round (restarted until found)
    "answer_timeout": 8,       # s after the join request before pairing starts anyway
    "accept_timeout": 120,     # s to wait for Accept on the TV
    "assoc_timeout": 20,       # s for a join with the saved passphrase
    "dhcp_timeout": 15,        # s for DHCP
}


def emit(event, **data):
    sys.stdout.write(json.dumps({"event": event, **data}) + "\n")
    sys.stdout.flush()


def log(msg):
    emit("log", msg=msg)


def run(cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}: {(r.stderr or r.stdout).strip()}")
    return r.stdout


def mac_bytes(mac):
    return bytes(int(x, 16) for x in mac.split(":"))


def mac_str(raw):
    return ":".join(f"{x:02x}" for x in bytes(raw))


def to_bytes(v):
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    if isinstance(v, str):
        return v.encode()
    return bytes(v)


def wfd_source_ie(rtsp_port, throughput):
    """WFD subelement 0 (device information): source, session available."""
    return bytes([0x00]) + struct.pack(">HHHH", 6, 0x0090, rtsp_port, throughput)


def p2p_assoc_ie(dev_addr, name):
    """Wi-Fi Direct (P2P) IE for an association request: Capability + Device Info.

    With this the TV's wpa_supplicant reports p2p_dev_addr for our regular-client
    association, so the TV treats us as the Wi-Fi Direct device that asked to join.
    """
    name_b = name.encode()[:32]
    capability = bytes([0x02]) + struct.pack("<H", 2) + bytes([0x25, 0x00])
    info_body = (dev_addr + struct.pack(">H", 0x1108)
                 + bytes([0x00, 0x01, 0x00, 0x50, 0xF2, 0x04, 0x00, 0x01])  # computer
                 + b"\x00" + struct.pack(">HH", 0x1011, len(name_b)) + name_b)
    device_info = bytes([0x0D]) + struct.pack("<H", len(info_body)) + info_body
    body = bytes([0x50, 0x6F, 0x9A, 0x09]) + capability + device_info
    return bytes([0xDD, len(body)]) + body


def wfd_device_type(ies):
    """0 source, 1 primary sink, 2 secondary sink, 3 dual role, None unknown."""
    ies = to_bytes(ies)
    i = 0
    while i + 3 <= len(ies):
        sid, length = ies[i], struct.unpack(">H", ies[i + 1:i + 3])[0]
        if sid == 0 and length >= 2:
            return struct.unpack(">H", ies[i + 3:i + 5])[0] & 0x3
        i += 3 + length
    return None


def parse_iw(text):
    """{phy: {"ifaces": [netdev names], "p2p_addr": addr or None}} from `iw dev`."""
    phys, state = {}, {"phy": None, "blk": None}

    def close():
        blk, phy = state["blk"], state["phy"]
        if blk and phy:
            if blk.get("type") == "P2P-device":
                phys[phy]["p2p_addr"] = blk.get("addr")
            elif blk.get("name"):
                phys[phy]["ifaces"].append(blk["name"])

    for line in text.splitlines():
        if line.startswith("phy#"):
            close()
            state["blk"], state["phy"] = None, line.strip()
            phys[state["phy"]] = {"ifaces": [], "p2p_addr": None}
        elif line.startswith("\t") and not line.startswith("\t\t"):
            close()
            s = line.strip()
            state["blk"] = {"name": s.split()[1] if s.startswith("Interface ") else None}
        elif state["blk"] is not None:
            k = line.split()
            if k and k[0] == "addr":
                state["blk"]["addr"] = k[1]
            elif k and k[0] == "type":
                state["blk"]["type"] = " ".join(k[1:])
    close()
    return phys


class Dhcp:
    """Minimal DHCPv4 client on one interface.

    Requests go out over a UDP socket bound to the interface. Replies are read
    at link level (AF_PACKET), like dhclient/udhcpc/networkd do: before the
    interface has an address, the kernel's input path (firewall, reverse-path
    checks) can drop the server's broadcast before a UDP socket ever sees it."""

    def __init__(self, iface, mac, hostname):
        self.iface, self.mac, self.hostname = iface, mac, hostname

    def _socket(self, bind_ip="0.0.0.0"):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, self.iface.encode())
        s.bind((bind_ip, 68))
        s.settimeout(1.0)
        return s

    def _packet(self, kind, xid, requested=None, server=None, ciaddr=None):
        p = struct.pack("!BBBBIHH", 1, 1, 6, 0, xid, 0, 0 if ciaddr else 0x8000)
        p += socket.inet_aton(ciaddr or "0.0.0.0") + bytes(12)
        p += self.mac + bytes(10) + bytes(192)
        o = bytes([0x63, 0x82, 0x53, 0x63, 53, 1, kind, 61, 7, 1]) + self.mac
        host = self.hostname.encode()[:63]
        o += bytes([12, len(host)]) + host + bytes([55, 4, 1, 3, 6, 51])
        if requested:
            o += bytes([50, 4]) + socket.inet_aton(requested)
        if server:
            o += bytes([54, 4]) + socket.inet_aton(server)
        return p + o + b"\xff"

    @staticmethod
    def _parse(data, xid):
        if len(data) < 240 or data[0] != 2 or struct.unpack("!I", data[4:8])[0] != xid:
            return None
        if data[236:240] != b"\x63\x82\x53\x63":
            return None
        opts, i = {}, 240
        while i < len(data) and data[i] != 255:
            if data[i] == 0:
                i += 1
                continue
            opts[data[i]] = data[i + 2:i + 2 + data[i + 1]]
            i += 2 + data[i + 1]
        return socket.inet_ntoa(data[16:20]), opts

    def _link_socket(self):
        s = socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(0x0800))
        s.bind((self.iface, 0x0800))
        s.settimeout(1.0)
        return s

    @staticmethod
    def _udp_payload_to_68(frame):
        """IPv4 packet → UDP payload if it is UDP to port 68, else None."""
        if len(frame) < 28 or frame[0] >> 4 != 4 or frame[9] != 17:
            return None
        ihl = (frame[0] & 0x0F) * 4
        if len(frame) < ihl + 8 or struct.unpack("!H", frame[ihl + 2:ihl + 4])[0] != 68:
            return None
        return frame[ihl + 8:]

    def _exchange(self, sock, packet, dest, want, xid, timeout):
        link = self._link_socket()
        try:
            return self._exchange_on(link, sock, packet, dest, want, xid, timeout)
        finally:
            link.close()

    def _exchange_on(self, link, sock, packet, dest, want, xid, timeout):
        deadline, next_send = time.monotonic() + timeout, 0.0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_send:
                sock.sendto(packet, dest)
                next_send = time.monotonic() + 3
            try:
                frame = link.recv(4096)
            except socket.timeout:
                continue
            data = self._udp_payload_to_68(frame)
            if data is None:
                continue
            r = self._parse(data, xid)
            if not r:
                continue
            kind = r[1].get(53, b"\0")[0]
            if kind == 6:
                raise RuntimeError("the TV refused the DHCP request (NAK)")
            if kind == want:
                return r
        raise RuntimeError("no DHCP answer from the TV")

    @staticmethod
    def _lease(ip, o):
        mask = o.get(1, socket.inet_aton("255.255.255.0"))
        return {
            "ip": ip,
            "prefix": sum(bin(b).count("1") for b in mask),
            "server": socket.inet_ntoa(o[54]) if 54 in o else None,
            "router": socket.inet_ntoa(o[3][:4]) if 3 in o else None,
            "lease": struct.unpack("!I", o[51])[0] if 51 in o else 3600,
        }

    def acquire(self, timeout):
        xid = random.getrandbits(32)
        s = self._socket()
        try:
            ip, o = self._exchange(s, self._packet(1, xid), ("255.255.255.255", 67), 2, xid, timeout)
            server = socket.inet_ntoa(o[54]) if 54 in o else None
            ip, o = self._exchange(s, self._packet(3, xid, requested=ip, server=server),
                                   ("255.255.255.255", 67), 5, xid, timeout)
        finally:
            s.close()
        return self._lease(ip, o)

    def renew(self, lease, timeout):
        xid = random.getrandbits(32)
        s = self._socket(lease["ip"])
        try:
            ip, o = self._exchange(s, self._packet(3, xid, ciaddr=lease["ip"]),
                                   (lease["server"] or "255.255.255.255", 67), 5, xid, timeout)
        finally:
            s.close()
        return self._lease(ip, o)


class Helper:
    def __init__(self):
        self.o = dict(DEFAULTS)
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        self.loop = GLib.MainLoop()
        self.main = self.main_ifname = self.p2p_addr = None
        self.wfd = None
        self.created_netdev = False
        self.old_wfd_ies = None
        self.fw = None
        self.subs = []
        self.phase = "idle"
        self.target = None
        self.peer_path = None
        self.join_scanning = False
        self.join_sent_at = 0.0
        self.timer = None
        self.net = None
        self.new_creds = None
        self.lease = None
        self.renew_timer = None
        self.associated = False
        self.cleaned = False

    # ---- D-Bus plumbing -------------------------------------------------
    def call(self, path, iface, method, sig=None, args=None, timeout=10000):
        params = GLib.Variant(sig, args) if sig else None
        return self.bus.call_sync(WPAS, path, iface, method, params, None,
                                  Gio.DBusCallFlags.NONE, timeout, None)

    def get(self, path, iface, prop):
        r = self.bus.call_sync(WPAS, path, PROPS, "Get", GLib.Variant("(ss)", (iface, prop)),
                               GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, 5000, None)
        return r.unpack()[0]

    def set(self, path, iface, prop, value):
        self.bus.call_sync(WPAS, path, PROPS, "Set", GLib.Variant("(ssv)", (iface, prop, value)),
                           None, Gio.DBusCallFlags.NONE, 5000, None)

    def subscribe(self, iface, member, path, cb):
        sid = self.bus.signal_subscribe(WPAS, iface, member, path, None,
                                        Gio.DBusSignalFlags.NONE,
                                        lambda *a: self.guard(cb, a[5].unpack()))
        self.subs.append(sid)

    def guard(self, fn, args=()):
        try:
            fn(*args)
        except Exception as e:  # report and keep the loop alive
            self.fail(str(e))
        return False

    def set_timer(self, seconds, fn):
        self.clear_timer()
        self.timer = GLib.timeout_add_seconds(seconds, lambda: self.guard(fn))

    def clear_timer(self):
        if self.timer:
            GLib.source_remove(self.timer)
            self.timer = None

    def fail(self, msg):
        self.clear_timer()
        self.phase = "idle"
        emit("error", msg=msg)

    def status(self, msg):
        emit("status", msg=msg)

    # ---- setup / teardown ----------------------------------------------
    def setup(self, opts):
        self.o.update({k: v for k, v in opts.items() if k in DEFAULTS and v not in (None, "")})
        iface = self.o["iface"]
        phys = parse_iw(run(["iw", "dev"]))
        for path in self.get(WPAS_PATH, WPAS, "Interfaces"):
            name = self.get(path, IFACE, "Ifname")
            if name == iface or name.startswith("p2p-"):
                continue
            for info in phys.values():
                if name in info["ifaces"] and info["p2p_addr"]:
                    self.main, self.main_ifname, self.p2p_addr = path, name, info["p2p_addr"]
        if not self.main:
            raise RuntimeError("no Wi-Fi interface with Wi-Fi Direct support is managed by wpa_supplicant")
        log(f"main Wi-Fi {self.main_ifname}, Wi-Fi Direct address {self.p2p_addr}")

        self.remove_iface()  # leftovers from a crash
        run(["iw", "dev", self.main_ifname, "interface", "add", iface, "type", "managed"])
        self.created_netdev = True
        time.sleep(0.2)
        run(["nmcli", "device", "set", iface, "managed", "no"], check=False)
        self.release_from_networkmanager(iface)
        self.wfd = self.create_wpas_iface(iface)
        self.call(self.wfd, IFACE, "VendorElemAdd", "(iay)",
                  (13, p2p_assoc_ie(mac_bytes(self.p2p_addr), self.o["name"])))
        self.old_wfd_ies = bytes(self.get(WPAS_PATH, WPAS, "WFDIEs"))
        self.set(WPAS_PATH, WPAS, "WFDIEs",
                 GLib.Variant("ay", wfd_source_ie(int(self.o["rtsp_port"]), int(self.o["throughput"]))))
        self.firewall_add()

        self.subscribe(P2P, "DeviceFoundProperties", self.main, self.on_device_found)
        self.subscribe(P2P, "FindStopped", self.main, self.on_find_stopped)
        self.subscribe(P2P, "ProvisionDiscoveryPBCResponse", self.main, self.on_pd_response)
        # During a join, wpa_supplicant does not emit ProvisionDiscoveryPBCResponse: on the TV's
        # answer it goes straight to creating its temporary group interface (p2p-<iface>-N).
        self.subscribe(WPAS, "InterfaceAdded", WPAS_PATH, self.on_interface_added)
        self.subscribe(P2P, "GroupFormationFailure", self.main, self.on_group_formation_failure)
        self.subscribe(PROPS, "PropertiesChanged", self.main, self.on_main_props)
        self.subscribe(P2P, "ProvisionDiscoveryFailure", self.main, self.on_pd_failure)
        self.subscribe(WPS, "Credentials", self.wfd, self.on_credentials)
        self.subscribe(WPS, "Event", self.wfd, self.on_wps_event)
        self.subscribe(PROPS, "PropertiesChanged", self.wfd, self.on_props)
        emit("ready", iface=iface, main=self.main_ifname, p2p_addr=self.p2p_addr)

    def firewall_add(self):
        if subprocess.run(["firewall-cmd", "--state"], capture_output=True).returncode != 0:
            log("firewalld is not running: no firewall changes")
            return
        if FW_ZONE not in run(["firewall-cmd", "--get-zones"]).split():
            raise RuntimeError("firewalld zone 'wfdcast' is missing; run install.sh again")
        run(["firewall-cmd", "--zone", FW_ZONE, "--change-interface", self.o["iface"]])
        run(["firewall-cmd", "--zone", FW_ZONE, "--add-port", f"{self.o['rtsp_port']}/tcp"])
        self.fw = FW_ZONE

    def nm_state(self, iface):
        for line in run(["nmcli", "-t", "-f", "DEVICE,STATE", "device"], check=False).splitlines():
            dev, _, state = line.partition(":")
            if dev == iface:
                return state
        return None

    def release_from_networkmanager(self, iface):
        """NetworkManager grabs every new Wi-Fi device (a new device briefly shows
        as "unmanaged" before NM takes it). Always tell NM to leave it alone,
        then require it to stay unmanaged for a moment before going on, or NM
        keeps scanning on it and starves the Wi-Fi Direct join of radio time."""
        deadline = time.monotonic() + 3
        while self.nm_state(iface) is None and time.monotonic() < deadline:
            time.sleep(0.1)
        deadline = time.monotonic() + 8
        stable_since = None
        while time.monotonic() < deadline:
            if self.nm_state(iface) == "unmanaged":
                stable_since = stable_since or time.monotonic()
                if time.monotonic() - stable_since >= 1.0:
                    return
            else:
                stable_since = None
                run(["nmcli", "device", "set", iface, "managed", "no"], check=False)
            time.sleep(0.2)
        raise RuntimeError(f"NetworkManager keeps managing {iface}; cannot use it")

    def wpas_iface_path(self, iface):
        try:
            return self.call(WPAS_PATH, WPAS, "GetInterface", "(s)", (iface,)).unpack()[0]
        except GLib.Error:
            return None

    def create_wpas_iface(self, iface):
        """Register our interface with wpa_supplicant, replacing a registration
        NetworkManager may have left behind."""
        for _ in range(10):
            stale = self.wpas_iface_path(iface)
            if stale:
                try:
                    self.call(WPAS_PATH, WPAS, "RemoveInterface", "(o)", (stale,))
                except GLib.Error:
                    pass
                time.sleep(0.2)
                continue
            try:
                return self.call(WPAS_PATH, WPAS, "CreateInterface", "(a{sv})",
                                 ({"Ifname": GLib.Variant("s", iface),
                                   "Driver": GLib.Variant("s", "nl80211")},)).unpack()[0]
            except GLib.Error as e:
                if "InterfaceExists" not in str(e):
                    raise
                time.sleep(0.2)
        raise RuntimeError(f"could not take {iface} over from NetworkManager/wpa_supplicant")

    def remove_iface(self):
        """Remove our interface from wpa_supplicant and the kernel, by name.
        Only ever touches the interface named in our settings (default wfd0)."""
        iface = self.o["iface"]
        if iface == self.main_ifname:
            return  # never, ever touch the main Wi-Fi
        path = self.wpas_iface_path(iface)
        if path:
            for method in ("Cancel",):
                try:
                    self.call(path, WPS, method)
                except GLib.Error:
                    pass
            try:
                self.call(WPAS_PATH, WPAS, "RemoveInterface", "(o)", (path,))
            except GLib.Error:
                pass
        if os.path.exists(f"/sys/class/net/{iface}"):
            run(["iw", "dev", iface, "del"], check=False)

    def cleanup(self):
        if self.cleaned:
            return
        self.cleaned = True
        self.clear_timer()
        if self.renew_timer:
            GLib.source_remove(self.renew_timer)
        for sid in self.subs:
            self.bus.signal_unsubscribe(sid)
        if self.main:
            try:
                self.call(self.main, P2P, "StopFind")  # only stops discovery
            except GLib.Error:
                pass
        if self.wfd or self.created_netdev:
            self.remove_iface()
        if self.old_wfd_ies is not None:
            try:
                self.set(WPAS_PATH, WPAS, "WFDIEs", GLib.Variant("ay", self.old_wfd_ies))
            except GLib.Error:
                pass
        if self.fw:
            run(["firewall-cmd", "--zone", self.fw, "--remove-port", f"{self.o['rtsp_port']}/tcp"], check=False)
            run(["firewall-cmd", "--zone", self.fw, "--remove-interface", self.o["iface"]], check=False)
        emit("bye")

    # ---- discovery -------------------------------------------------------
    def find(self):
        """One short search round. A TV that hosts its own group sits on its
        operating channel (often 5 GHz) and is only seen by a full scan, which
        wpa_supplicant does once at the start of a search; after that it checks
        only the social channels 1/6/11. So: progressive discovery (adds one
        other channel per cycle) and short rounds, restarted on FindStopped,
        each starting with a full scan."""
        self.call(self.main, P2P, "Find", "(a{sv})",
                  ({"Timeout": GLib.Variant("i", int(self.o["search_round"])),
                    "DiscoveryType": GLib.Variant("s", "progressive")},))

    def scan(self):
        """Look for TVs until one is chosen (Find is restarted when it times out)."""
        self.phase = "scan"
        self.find()
        for path in self.get(self.main, P2P, "Peers"):
            self.report_peer(path)

    def on_find_stopped(self):
        if self.phase in ("scan", "find"):
            self.find()

    def report_peer(self, path):
        try:
            mac = mac_str(self.get(path, PEER, "DeviceAddress"))
            name = self.get(path, PEER, "DeviceName")
            kind = wfd_device_type(self.get(path, PEER, "IEs"))
        except GLib.Error:
            return None
        emit("peer", mac=mac, name=name, sink=kind in (1, 3), wfd=kind is not None)
        return mac

    def on_device_found(self, path, _props):
        mac = self.report_peer(path)
        if self.phase == "find" and mac == self.target:
            self.join_request(path)

    # ---- connecting ------------------------------------------------------
    def connect(self, mac, creds):
        self.target = mac.lower()
        self.new_creds = None
        self.associated = False
        if creds and creds.get("ssid") and creds.get("psk"):
            self.connect_saved(creds)
        else:
            self.connect_wps()

    def connect_saved(self, c):
        self.phase = "saved"
        self.status("Connecting with the saved pairing…")
        psk = (GLib.Variant("ay", bytes.fromhex(c["psk"])) if c.get("psk_hex")
               else GLib.Variant("s", c["psk"]))
        props = {"ssid": GLib.Variant("s", c["ssid"]), "psk": psk,
                 "key_mgmt": GLib.Variant("s", "WPA-PSK"), "proto": GLib.Variant("s", "RSN"),
                 "pairwise": GLib.Variant("s", "CCMP")}
        if c.get("bssid"):
            props["bssid"] = GLib.Variant("s", c["bssid"])
        self.net = self.call(self.wfd, IFACE, "AddNetwork", "(a{sv})", (props,)).unpack()[0]
        self.call(self.wfd, IFACE, "SelectNetwork", "(o)", (self.net,))
        self.set_timer(int(self.o["assoc_timeout"]), self.saved_failed)

    def saved_failed(self):
        log("the saved pairing did not work; pairing again with Accept on the TV")
        emit("forget", mac=self.target)
        self.call(self.wfd, IFACE, "RemoveAllNetworks")
        self.connect_wps()

    def connect_wps(self):
        self.phase = "find"
        self.status("Looking for the TV… Open Screen mirroring on the TV "
                    "(close and reopen it if it was already open).")
        self.call(self.wfd, IFACE, "RemoveAllNetworks")
        self.find()
        self.set_timer(int(self.o["find_timeout"]), lambda: self.fail(
            "TV not found. Is Screen mirroring open on the TV?"))

    def join_request(self, peer_path):
        """Ask the TV to let us join: wpa_supplicant scans for the TV's group,
        sends it a provision discovery request (the TV shows Accept), and on the
        answer tries to create its own temporary group interface. That last step
        usually fails because wlan0 + our interface already use both client
        slots, which is fine: our interface does the real join over WPS. WPS on
        our interface must start only after that scan, or its own scans starve
        the join scan, so it starts on the first sign the request went out."""
        self.phase = "answer"
        self.peer_path = peer_path
        self.join_scanning = False
        self.join_sent_at = time.monotonic()
        self.status("Asking the TV to let this computer join…")
        self.call(self.main, P2P, "Connect", "(a{sv})",
                  ({"peer": GLib.Variant("o", peer_path), "wps_method": GLib.Variant("s", "pbc"),
                    "join": GLib.Variant("b", True)},))
        # If no sign arrives, pair anyway: the TV keeps Accept open for minutes.
        self.set_timer(int(self.o["answer_timeout"]), self.start_wps)

    def on_main_props(self, iface, changed, _invalid):
        if self.phase != "answer" or iface != IFACE or "Scanning" not in changed:
            return
        if changed["Scanning"]:
            self.join_scanning = True
        elif self.join_scanning and time.monotonic() - self.join_sent_at >= 2:
            # (a background scan of the main Wi-Fi ending right away is not the join scan)
            self.join_scanning = False
            self.set_timer(1, self.start_wps)  # the request goes out right after the scan

    def on_group_formation_failure(self, reason):
        if self.phase == "answer":
            log(f"wpa_supplicant's own group setup ended ({reason or 'no reason'}); pairing on our interface")
            self.start_wps()

    def on_pd_response(self, path):
        if self.phase == "answer" and path == self.peer_path:
            self.start_wps()

    def on_interface_added(self, _path, props):
        if self.phase == "answer" and str(props.get("Ifname", "")).startswith("p2p-"):
            log(f"the TV answered the join request ({props.get('Ifname')} created)")
            self.start_wps()

    def on_pd_failure(self, path, code):
        if self.phase == "answer" and path == self.peer_path:
            self.fail(f"The TV rejected the join request (code {code}).")

    def group_bssid(self):
        """The TV's group BSSID: a DIRECT-* network with the TV's address, else the TV's address."""
        for path in self.get(self.main, IFACE, "BSSs"):
            try:
                bssid = mac_str(self.get(path, BSS, "BSSID"))
                ssid = to_bytes(self.get(path, BSS, "SSID"))
            except GLib.Error:
                continue
            if bssid == self.target and ssid.startswith(b"DIRECT-"):
                return mac_bytes(bssid)
        return mac_bytes(self.target)

    def start_wps(self):
        if self.phase != "answer":
            return
        self.phase = "wps"
        emit("accept")
        self.call(self.wfd, WPS, "Start", "(a{sv})",
                  ({"Role": GLib.Variant("s", "enrollee"), "Type": GLib.Variant("s", "pbc"),
                    "Bssid": GLib.Variant("ay", self.group_bssid())},))
        self.set_timer(int(self.o["accept_timeout"]), lambda: self.fail(
            "No Accept on the TV in time."))

    def on_credentials(self, cred):
        ssid = to_bytes(cred.get("SSID", b"")).decode(errors="replace")
        key = to_bytes(cred.get("Key", b"")).decode(errors="replace")
        bssid = mac_str(cred["BSSID"]) if cred.get("BSSID") else None
        if ssid and key:
            self.new_creds = {"ssid": ssid, "psk": key, "bssid": bssid}

    def on_wps_event(self, name, args):
        if self.phase != "wps":
            return
        if name in ("fail", "pbc-overlap"):
            self.fail(f"Pairing failed on the TV side (WPS {name}).")

    def on_props(self, iface, changed, _invalid):
        if iface != IFACE or "State" not in changed:
            return
        state = changed["State"]
        if state == "completed" and not self.associated:
            self.associated = True
            self.on_associated()
        elif state != "completed" and self.associated:
            self.associated = False
            emit("disconnected")

    def read_joined_network(self):
        """SSID/passphrase/BSSID of the network we just joined. wpa_supplicant
        does not emit the WPS Credentials signal here, but the network that
        WPS created carries them."""
        net = self.get(self.wfd, IFACE, "CurrentNetwork")
        props = self.get(net, NETWORK, "Properties")
        unquote = lambda v: v[1:-1] if len(v) >= 2 and v[0] == v[-1] == '"' else v
        ssid, psk = unquote(str(props.get("ssid", ""))), str(props.get("psk", ""))
        psk_hex = not psk.startswith('"')
        bssid = None
        try:
            bssid = mac_str(self.get(self.get(self.wfd, IFACE, "CurrentBSS"), BSS, "BSSID"))
        except GLib.Error:
            pass
        if ssid and psk:
            return {"ssid": ssid, "psk": unquote(psk), "psk_hex": psk_hex, "bssid": bssid}
        return None

    def on_associated(self):
        self.clear_timer()
        if self.phase == "wps":
            try:
                self.new_creds = self.read_joined_network() or self.new_creds
            except GLib.Error as e:
                log(f"could not read the pairing to save it: {e.get_message()}")
        self.phase = "dhcp"
        self.status("Joined the TV's network, getting an address…")
        mac = mac_bytes(open(f"/sys/class/net/{self.o['iface']}/address").read().strip())
        dhcp = Dhcp(self.o["iface"], mac, self.o["name"])

        def work():
            try:
                lease = dhcp.acquire(int(self.o["dhcp_timeout"]))
                GLib.idle_add(lambda: self.guard(self.on_lease, (dhcp, lease)))
            except Exception as e:
                msg = f"Could not get an address from the TV (DHCP): {e}"
                GLib.idle_add(lambda: self.guard(self.fail, (msg,)))

        threading.Thread(target=work, daemon=True).start()

    def on_lease(self, dhcp, lease):
        iface = self.o["iface"]
        run(["ip", "-4", "addr", "flush", "dev", iface], check=False)
        run(["ip", "addr", "add", f"{lease['ip']}/{lease['prefix']}", "dev", iface])
        self.lease = lease
        self.phase = "connected"
        if self.new_creds:
            emit("credentials", mac=self.target, **self.new_creds)
            self.new_creds = None
        emit("connected", ip=lease["ip"], prefix=lease["prefix"],
             tv_ip=lease["server"] or lease["router"])
        if self.renew_timer:
            GLib.source_remove(self.renew_timer)
        self.renew_timer = GLib.timeout_add_seconds(max(60, lease["lease"] // 2),
                                                    lambda: self.renew(dhcp))

    def renew(self, dhcp):
        def work():
            try:
                lease = dhcp.renew(self.lease, int(self.o["dhcp_timeout"]))
                self.lease = lease
                log(f"DHCP renewed for {lease['lease']} s")
            except Exception as e:
                log(f"DHCP renew failed: {e}")  # str() evaluated here, inside the except

        threading.Thread(target=work, daemon=True).start()
        return True

    def reassociate(self):
        if not self.wfd or self.get(self.wfd, IFACE, "Ifname") != self.o["iface"]:
            raise RuntimeError("refusing to reassociate: interface mismatch")
        self.call(self.wfd, IFACE, "Disconnect")
        GLib.timeout_add_seconds(2, lambda: self.guard(lambda: self.call(self.wfd, IFACE, "Reassociate")))

    # ---- command loop ----------------------------------------------------
    def on_stdin(self, channel, condition):
        if condition & (GLib.IO_HUP | GLib.IO_ERR):
            self.quit()
            return False
        line = sys.stdin.readline()
        if not line:
            self.quit()
            return False
        try:
            cmd = json.loads(line)
        except json.JSONDecodeError:
            emit("error", msg=f"bad command: {line.strip()}")
            return True
        name = cmd.get("cmd")
        try:
            if name == "start":
                self.setup(cmd)
            elif name == "scan":
                self.scan()
            elif name == "connect":
                self.connect(cmd["mac"], cmd.get("creds"))
            elif name == "reassociate":
                self.reassociate()
            elif name == "quit":
                self.quit()
                return False
            else:
                emit("error", msg=f"unknown command: {name}")
        except (GLib.Error, RuntimeError, KeyError, OSError) as e:
            self.fail(str(e))
        return True

    def quit(self, *_):
        self.cleanup()
        self.loop.quit()

    def run(self):
        GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()),
                          GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self.on_stdin)
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal_add(GLib.PRIORITY_HIGH, sig, lambda *_: (self.quit(), False)[1])
        try:
            self.loop.run()
        finally:
            self.cleanup()


if __name__ == "__main__":
    if os.geteuid() != 0:
        sys.exit("wfdcast-helper must run as root (the app starts it through pkexec)")
    Helper().run()
