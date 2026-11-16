#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 stignarnia
# SPDX-License-Identifier: MIT
#
# Installs WFD Cast: a source copy for rebuilds, the rebuild script and a
# pacman hook (rebuild after Qt, KDE Frameworks or GStreamer updates), then
# builds and installs the app and its privileged helper under /usr/local, the
# polkit action that lets the app start the helper, and the firewalld zone
# "wfdcast" used for the link to the TV. Run with sudo from the repository folder.

set -euo pipefail

readonly SRC_DIR=/usr/local/src/wfdcast
readonly LIB_DIR=/usr/local/lib/wfdcast
readonly HOOK=/etc/pacman.d/hooks/90-wfdcast.hook
readonly ZONE_FILE=/etc/firewalld/zones/wfdcast.xml

cd "$(dirname -- "$(readlink -f -- "$0")")"

die() {
    echo "install.sh: $*" >&2
    exit 1
}

[[ $EUID -eq 0 ]] || die "run it with sudo: sudo ./install.sh"

# Requirements
missing=()
for cmd in cmake c++ pkg-config iw nmcli pkexec pactl python3 gst-inspect-1.0; do
    command -v "$cmd" >/dev/null || missing+=("$cmd")
done
python3 -c 'import gi; gi.require_version("Gio", "2.0")' 2>/dev/null || missing+=("python-gobject")
for el in pipewiresrc mpegtsmux rtpmp2tpay h264parse aacparse udpsink pulsesrc; do
    gst-inspect-1.0 "$el" >/dev/null 2>&1 || missing+=("GStreamer element $el")
done
gst-inspect-1.0 x264enc >/dev/null 2>&1 || gst-inspect-1.0 vah264enc >/dev/null 2>&1 \
    || gst-inspect-1.0 nvh264enc >/dev/null 2>&1 || missing+=("an H.264 encoder (x264enc, vah264enc or nvh264enc)")
gst-inspect-1.0 fdkaacenc >/dev/null 2>&1 || gst-inspect-1.0 avenc_aac >/dev/null 2>&1 \
    || missing+=("an AAC encoder (fdkaacenc or avenc_aac)")
if ((${#missing[@]})); then
    printf 'install.sh: missing: %s\n' "${missing[@]}" >&2
    echo "On Arch: sudo pacman -S --needed cmake base-devel extra-cmake-modules kirigami ki18n kconfig kcoreaddons" \
         "qt6-declarative gstreamer gst-plugins-good gst-plugins-bad gst-plugins-ugly gst-libav gst-plugin-pipewire" \
         "python-gobject iw qtkeychain-qt6" >&2
    exit 1
fi

# Source copy, rebuild script, pacman hook
rm -rf -- "$SRC_DIR"
install -d -m 0755 "$SRC_DIR"
cp -r CMakeLists.txt src helper data "$SRC_DIR/"
install -D -m 0755 packaging/rebuild "$LIB_DIR/rebuild"
if command -v pacman >/dev/null; then
    install -D -m 0644 packaging/90-wfdcast.hook "$HOOK"
else
    echo "no pacman found: run 'sudo $LIB_DIR/rebuild' after Qt, KDE Frameworks or GStreamer updates"
fi

# Build and install app, helper, desktop file, polkit action
"$LIB_DIR/rebuild" || die "the build failed, see /var/cache/wfdcast/build.log"

# firewalld zone (only if firewalld is in use)
if systemctl is-active --quiet firewalld; then
    install -m 0644 data/wfdcast.xml "$ZONE_FILE"
    firewall-cmd --reload >/dev/null
    echo "firewalld zone 'wfdcast' installed (it allows only DHCP replies; the helper opens the RTSP port while casting)."
else
    echo "firewalld is not running: no zone installed."
fi

echo
echo "WFD Cast is installed. Start it from the app menu (WFD Cast) or run: wfdcast"
