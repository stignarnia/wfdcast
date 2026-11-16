#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 stignarnia
# SPDX-License-Identifier: MIT
#
# Removes everything install.sh added. Settings in ~/.config/wfdcastrc are kept
# unless --purge is given (they include the saved TV pairing).

set -euo pipefail

readonly PREFIX=/usr/local
readonly ZONE_FILE=/etc/firewalld/zones/wfdcast.xml

die() {
    echo "uninstall.sh: $*" >&2
    exit 1
}

[[ $EUID -eq 0 ]] || die "run it with sudo: sudo ./uninstall.sh [--purge]"

rm -f /etc/pacman.d/hooks/90-wfdcast.hook
rm -rf /usr/local/src/wfdcast /var/lib/wfdcast /var/cache/wfdcast
rm -f "$PREFIX/bin/wfdcast"
rm -rf "$PREFIX/lib/wfdcast"
rm -f "$PREFIX/share/applications/io.github.stignarnia.wfdcast.desktop"
rm -f /usr/share/polkit-1/actions/io.github.stignarnia.wfdcast.policy

if [[ -f $ZONE_FILE ]]; then
    rm -f "$ZONE_FILE" "$ZONE_FILE.old"
    if systemctl is-active --quiet firewalld; then
        firewall-cmd --reload >/dev/null
    fi
fi

if [[ ${1:-} == --purge && -n ${SUDO_USER:-} ]]; then
    home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    rm -f "$home/.config/wfdcastrc"
    echo "Settings and saved TV pairing removed."
fi

echo "WFD Cast is uninstalled."
