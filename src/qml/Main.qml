// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: i18n("WFD Cast")
    width: Kirigami.Units.gridUnit * 30
    height: Kirigami.Units.gridUnit * 42
    minimumWidth: Kirigami.Units.gridUnit * 22

    readonly property bool busy: cast.state !== "idle"
    readonly property bool sessionLocked: busy  // options that only apply on the next connection

    // Combo helper: model of {text, value}; keeps the setting in sync both ways.
    component SettingCombo: Controls.ComboBox {
        property string key
        property var options: []
        model: options
        textRole: "text"
        valueRole: "value"
        currentIndex: Math.max(0, options.findIndex(o => o.value === settings[key]))
        onActivated: settings[key] = currentValue
    }
    component SettingSpin: Controls.SpinBox {
        property string key
        property string unit: ""
        value: settings[key]
        editable: true
        onValueModified: settings[key] = value
        textFromValue: (v, locale) => unit ? i18nc("value with unit", "%1 %2", v, unit) : String(v)
        valueFromText: (text, locale) => parseInt(text)
    }

    pageStack.initialPage: Kirigami.ScrollablePage {
        title: i18n("WFD Cast")
        actions: [
            Kirigami.Action {
                text: i18n("Defaults")
                icon.name: "edit-reset"
                enabled: !root.busy
                onTriggered: settings.resetToDefaults()
            }
        ]

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: cast.status !== ""
                text: cast.status
                type: cast.error ? Kirigami.MessageType.Error
                    : cast.state === "streaming" ? Kirigami.MessageType.Positive
                    : cast.state === "accept" ? Kirigami.MessageType.Warning
                    : Kirigami.MessageType.Information
            }

            Controls.Button {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                Layout.preferredHeight: Kirigami.Units.gridUnit * 2.5
                highlighted: !root.busy
                icon.name: root.busy ? "media-playback-stop" : "video-display"
                text: root.busy ? i18n("Stop casting") : i18n("Cast to TV")
                enabled: cast.state !== "stopping"
                onClicked: root.busy ? cast.stop() : cast.start()
            }

            // TV list while choosing
            Kirigami.Heading {
                level: 4
                visible: cast.state === "choosing"
                text: cast.peers.length ? i18n("Choose your TV") : i18n("Searching for TVs…")
            }
            Repeater {
                model: cast.state === "choosing" ? cast.peers : []
                delegate: Controls.ItemDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    icon.name: "video-television"
                    text: modelData.name || modelData.mac
                    onClicked: cast.choosePeer(modelData.mac, modelData.name)
                }
            }

            Kirigami.FormLayout {
                Layout.fillWidth: true

                // ---- TV
                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Kirigami.FormData.label: i18n("TV")
                }
                RowLayout {
                    Kirigami.FormData.label: i18n("Remembered TV:")
                    Controls.Label {
                        text: settings.tvMac ? (settings.tvName || settings.tvMac)
                                             + (settings.tvSsid ? i18n(" (paired, passphrase in KWallet)") : "")
                                             : i18n("none, you choose it on first cast")
                    }
                    Controls.Button {
                        visible: settings.tvMac !== ""
                        enabled: !root.busy
                        text: i18n("Forget")
                        icon.name: "edit-delete"
                        onClicked: cast.forgetTv()
                    }
                }

                // ---- Picture
                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Kirigami.FormData.label: i18n("Picture")
                }
                SettingCombo {
                    Kirigami.FormData.label: i18n("Frame rate:")
                    enabled: !root.sessionLocked
                    key: "fps"
                    options: [{text: i18n("30 fps"), value: 30}, {text: i18n("60 fps"), value: 60}]
                }
                RowLayout {
                    Kirigami.FormData.label: i18n("Video bitrate:")
                    Controls.Slider {
                        id: bitrateSlider
                        Layout.fillWidth: true
                        from: 1000; to: 40000; stepSize: 500
                        value: settings.videoBitrate
                        onMoved: settings.videoBitrate = value
                    }
                    Controls.Label {
                        text: i18n("%1 Mbit/s", (settings.videoBitrate / 1000).toLocaleString(Qt.locale(), "f", 1))
                    }
                }
                SettingCombo {
                    Kirigami.FormData.label: i18n("Encoder:")
                    enabled: !root.sessionLocked
                    key: "encoder"
                    options: {
                        const all = [{text: i18n("Automatic (GPU KDE draws the screen on)"), value: "auto"},
                                     {text: i18n("AMD / Intel GPU (VA-API)"), value: "vaapi"},
                                     {text: i18n("NVIDIA GPU (NVENC)"), value: "nvenc"},
                                     {text: i18n("Software (x264)"), value: "x264"}]
                        return all.filter(o => o.value === "auto" || cast.availableEncoders.indexOf(o.value) >= 0)
                    }
                }
                RowLayout {
                    Kirigami.FormData.label: i18n("Speed ↔ quality:")
                    enabled: !root.sessionLocked
                    Controls.Label { text: i18n("faster") }
                    Controls.Slider {
                        Layout.fillWidth: true
                        from: 1; to: 7; stepSize: 1; snapMode: Controls.Slider.SnapAlways
                        value: settings.encoderQuality
                        onMoved: settings.encoderQuality = value
                    }
                    Controls.Label { text: i18n("better") }
                }
                SettingSpin {
                    Kirigami.FormData.label: i18n("Keyframe every:")
                    enabled: !root.sessionLocked
                    key: "keyframeSeconds"; unit: i18n("s"); from: 1; to: 10
                }

                // ---- Sound
                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Kirigami.FormData.label: i18n("Sound")
                }
                SettingCombo {
                    Kirigami.FormData.label: i18n("Sound plays on:")
                    enabled: !root.sessionLocked
                    key: "audioMode"
                    options: [{text: i18n("TV only"), value: "tv"},
                              {text: i18n("Laptop and TV"), value: "mirror"},
                              {text: i18n("No sound"), value: "none"}]
                }
                SettingSpin {
                    Kirigami.FormData.label: i18n("Audio bitrate:")
                    enabled: !root.sessionLocked && settings.audioMode !== "none"
                    key: "audioBitrate"; unit: i18n("kbit/s"); from: 64; to: 320; stepSize: 32
                }

                // ---- Picker
                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Kirigami.FormData.label: i18n("What KDE offers to share")
                }
                Controls.CheckBox {
                    Kirigami.FormData.label: i18n("Offer:")
                    enabled: !root.sessionLocked
                    text: i18n("Screens")
                    checked: settings.pickScreens
                    onToggled: settings.pickScreens = checked
                }
                Controls.CheckBox {
                    enabled: !root.sessionLocked
                    text: i18n("Windows")
                    checked: settings.pickWindows
                    onToggled: settings.pickWindows = checked
                }
                Controls.CheckBox {
                    enabled: !root.sessionLocked
                    text: i18n("New virtual screen")
                    checked: settings.pickVirtual
                    onToggled: settings.pickVirtual = checked
                }
                SettingCombo {
                    Kirigami.FormData.label: i18n("Mouse pointer:")
                    enabled: !root.sessionLocked
                    key: "cursorMode"
                    options: [{text: i18n("Shown"), value: "embedded"}, {text: i18n("Hidden"), value: "hidden"}]
                }

                // ---- Advanced
                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Kirigami.FormData.label: i18n("Connection (advanced)")
                }
                Controls.Switch {
                    id: showAdvanced
                    Kirigami.FormData.label: i18n("Show:")
                    text: i18n("Connection settings")
                }
                Controls.TextField {
                    visible: showAdvanced.checked
                    enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Name shown on the TV:")
                    text: settings.deviceName
                    onEditingFinished: settings.deviceName = text
                }
                Controls.TextField {
                    visible: showAdvanced.checked
                    enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Wi-Fi Direct interface:")
                    text: settings.iface
                    onEditingFinished: settings.iface = text
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("RTSP port:"); key: "rtspPort"; from: 1024; to: 65535
                    textFromValue: (v) => String(v)
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Local RTP port:"); key: "rtpPort"; from: 1024; to: 65535
                    textFromValue: (v) => String(v)
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Announced throughput:"); key: "throughput"; unit: i18n("Mbit/s"); from: 1; to: 1000
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("TV search timeout:"); key: "findTimeout"; unit: i18n("s"); from: 5; to: 300
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Search round:"); key: "searchRound"; unit: i18n("s"); from: 3; to: 60
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Pairing starts after:"); key: "answerTimeout"; unit: i18n("s"); from: 2; to: 60
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Accept timeout:"); key: "acceptTimeout"; unit: i18n("s"); from: 10; to: 600
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Saved-pairing timeout:"); key: "assocTimeout"; unit: i18n("s"); from: 5; to: 120
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("DHCP timeout:"); key: "dhcpTimeout"; unit: i18n("s"); from: 3; to: 120
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Wait for TV stream:"); key: "tvWaitSeconds"; unit: i18n("s"); from: 3; to: 120
                }
                SettingSpin {
                    visible: showAdvanced.checked; enabled: !root.sessionLocked
                    Kirigami.FormData.label: i18n("Keep-alive every:"); key: "keepaliveSeconds"; unit: i18n("s"); from: 5; to: 55
                }

                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                    opacity: 0.8
                    text: root.busy ? i18n("The bitrate changes live. Other settings apply from the next cast.")
                                    : i18n("The bitrate can also be changed while casting.")
                }
            }

            // ---- Log
            RowLayout {
                Layout.fillWidth: true
                Controls.Switch {
                    id: showLog
                    text: i18n("Show log")
                }
                Item { Layout.fillWidth: true }
                Controls.Button {
                    visible: showLog.checked
                    text: i18n("Copy")
                    icon.name: "edit-copy"
                    onClicked: { logArea.selectAll(); logArea.copy(); logArea.deselect() }
                }
                Controls.Button {
                    visible: showLog.checked
                    text: i18n("Clear")
                    icon.name: "edit-clear-history"
                    onClicked: cast.clearLog()
                }
            }
            Controls.ScrollView {
                id: logScroll
                visible: showLog.checked
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 14
                // follow new lines only while the view is already at the bottom
                property bool atBottom: true
                readonly property Flickable flick: contentItem
                function scrollToEnd() {
                    flick.contentY = Math.max(0, flick.contentHeight - flick.height)
                }
                // only a user scroll changes atBottom; following new text keeps it true
                Connections {
                    target: logScroll.flick
                    function onMovementEnded() {
                        logScroll.atBottom = logScroll.flick.contentY >= logScroll.flick.contentHeight - logScroll.flick.height - 4
                    }
                }
                Controls.ScrollBar.vertical.onPressedChanged: if (!Controls.ScrollBar.vertical.pressed)
                    atBottom = Controls.ScrollBar.vertical.position + Controls.ScrollBar.vertical.size >= 0.995
                onVisibleChanged: if (visible) Qt.callLater(scrollToEnd)

                Controls.TextArea {
                    id: logArea
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    font.family: "monospace"
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    text: cast.log
                    onTextChanged: if (logScroll.atBottom) Qt.callLater(logScroll.scrollToEnd)
                }
            }

            Controls.Label {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Kirigami.Units.largeSpacing
                text: i18n("WFD Cast %1", appVersion)
                font: Kirigami.Theme.smallFont
                opacity: 0.6
            }
        }
    }
}
