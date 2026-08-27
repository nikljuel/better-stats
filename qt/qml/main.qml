import QtQuick
import QtQuick.Window
import com.pocketbook.controls
import "."

Window {
    id: root

    visible: true
    width: screenW
    height: screenH - panelH
    color: GlobalValues.defaultBackgroundColor
    title: "Better Stats"

    AppHeader {
        id: appHeader

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        title: root.title
        onClose: Qt.quit()

        Rectangle {
            width: GlobalValues.defaultListItemHeight
            height: GlobalValues.defaultListItemHeight
            radius: width / 2
            color: settingsTap.pressed
                   ? GlobalValues.defaultTextColor : "transparent"

            Canvas {
                anchors.centerIn: parent
                width: parent.width * 0.45
                height: parent.height * 0.45
                property bool inv: settingsTap.pressed
                onInvChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.clearRect(0, 0, width, height);
                    ctx.fillStyle = inv
                        ? GlobalValues.defaultBackgroundColor
                        : GlobalValues.defaultTextColor;
                    var barH = Math.max(Math.round(height / 10), 1);
                    var gap = (height - 3 * barH) / 2;
                    for (var i = 0; i < 3; i++)
                        ctx.fillRect(0, i * (barH + gap), width, barH);
                }
            }

            TapHandler {
                id: settingsTap
                onTapped: {
                    settingsDialog.refresh()
                    settingsDialog.visible = true
                }
            }
        }
    }

    // Firmware-style tab bar: four large zones, active tab underlined.
    Item {
        id: tabBar

        anchors.top: appHeader.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: GlobalValues.defaultListItemHeight

        property int current: 0

        Row {
            anchors.fill: parent

            Repeater {
                model: [Tr.t("Übersicht", "Overview"), Tr.t("Serie", "Streak"),
                        Tr.t("Kalender", "Calendar"), Tr.t("Jahr", "Year")]

                Item {
                    required property string modelData
                    required property int index

                    width: tabBar.width / 4
                    height: tabBar.height

                    StyledText {
                        anchors.centerIn: parent
                        styledFont: index === tabBar.current
                                    ? FontStyles.BodyLBold : FontStyles.BodyL
                        color: index === tabBar.current
                               ? GlobalValues.defaultTextColor
                               : GlobalValues.defaultDisabledTextColor
                        text: modelData
                    }

                    Rectangle {
                        visible: index === tabBar.current
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width * 0.55
                        height: Global.dp(4)
                        color: GlobalValues.defaultTextColor
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: tabBar.current = index
                    }
                }
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: GlobalValues.defaultSolidSeparatorThickness
            color: GlobalValues.defaultBorderColor
        }
    }

    FocusScope {
        anchors.top: tabBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        focus: true

        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape
                    || event.key === Qt.Key_Home) {
                event.accepted = true;
                Qt.quit();
            }
        }

        OverviewTab {
            anchors.fill: parent
            visible: tabBar.current === 0
        }

        StreakTab {
            anchors.fill: parent
            visible: tabBar.current === 1
        }

        CalendarTab {
            anchors.fill: parent
            visible: tabBar.current === 2
        }

        YearTab {
            anchors.fill: parent
            visible: tabBar.current === 3
        }
    }

    Connections {
        target: stats

        function onUpdateChanged() {
            if (stats.updateState === "available" && !settingsDialog.visible)
                updateDialog.visible = true
        }
    }

    PanelDialog {
        id: updateDialog

        title: Tr.t("Update verfügbar", "Update available")

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyL
            color: GlobalValues.defaultTextColor
            wrapMode: Text.Wrap
            text: Tr.t("Version %1 ist verfügbar.",
                       "Version %1 is available.").arg(stats.latestVersion)
        }

        Row {
            width: parent.width
            spacing: Global.dp(12)

            Rectangle {
                width: (parent.width - parent.spacing) / 2
                height: Global.dp(48)
                color: GlobalValues.defaultTextColor

                StyledText {
                    anchors.centerIn: parent
                    styledFont: FontStyles.BodyLBold
                    color: GlobalValues.defaultBackgroundColor
                    text: Tr.t("Jetzt installieren", "Install now")
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        updateDialog.visible = false
                        settingsDialog.refresh()
                        settingsDialog.visible = true
                        stats.installUpdate()
                    }
                }
            }

            Rectangle {
                width: (parent.width - parent.spacing) / 2
                height: Global.dp(48)
                color: GlobalValues.defaultBackgroundColor
                border.width: GlobalValues.dialogBorderWidth
                border.color: GlobalValues.defaultTextColor

                StyledText {
                    anchors.centerIn: parent
                    styledFont: FontStyles.BodyLBold
                    color: GlobalValues.defaultTextColor
                    text: Tr.t("Später", "Later")
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: updateDialog.visible = false
                }
            }
        }
    }

    PanelDialog {
        id: settingsDialog

        title: Tr.t("Einstellungen", "Settings")
        property var status: ({ enabled: false, available: false, message: "" })

        function refresh() {
            status = stats.autostartStatus()
        }

        function updateStatusText() {
            if (stats.updateState === "checking")
                return Tr.t("Suche nach Updates …", "Checking for updates …")
            if (stats.updateState === "downloading")
                return Tr.t("Update wird geladen und geprüft …",
                            "Downloading and verifying update …")
            if (stats.updateState === "restarting")
                return Tr.t("Update installiert. Neustart …",
                            "Update installed. Restarting …")
            if (stats.updateState === "available")
                return Tr.t("Update verfügbar: ", "Update available: ")
                       + stats.latestVersion
            if (stats.updateState === "current")
                return Tr.t("Better Stats ist aktuell.", "Better Stats is up to date.")
            if (stats.updateState !== "error")
                return Tr.t("Noch nicht geprüft.", "Not checked yet.")
            if (stats.updateError === -1)
                return Tr.t("Keine WLAN-Verbindung.", "No Wi-Fi connection.")
            if (stats.updateError === -2)
                return Tr.t("Download fehlgeschlagen.", "Download failed.")
            if (stats.updateError === -3)
                return Tr.t("Release-Antwort ungültig.", "Invalid release response.")
            if (stats.updateError === -4)
                return Tr.t("Kein passendes Update-Paket gefunden.",
                            "No matching update package found.")
            if (stats.updateError === -5)
                return Tr.t("Update-Paket ist beschädigt.", "Update package is damaged.")
            if (stats.updateError === -7)
                return Tr.t("Diese Firmware unterstützt WLAN-Updates nicht.",
                            "This firmware does not support Wi-Fi updates.")
            return Tr.t("Update konnte nicht installiert werden.",
                        "The update could not be installed.")
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyLBold
            color: GlobalValues.defaultTextColor
            text: Tr.t("Tracking", "Tracking")
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: settingsDialog.status.enabled === true
                ? Tr.t("Tracking läuft automatisch, sobald ein EPUB geöffnet wird – auch beim letzten Buch nach einem Neustart.",
                       "Tracking runs automatically when an EPUB opens, including the last book after a restart.")
                : Tr.t("Tracking läuft nur, solange Better Stats geöffnet ist.",
                       "Tracking only runs while Better Stats is open.")
        }

        StyledText {
            visible: settingsDialog.status.enabled !== true
                     && settingsDialog.status.message !== ""
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: settingsDialog.status.message === "KOReader association detected"
                ? Tr.t("Grund: KOReader ist als EPUB-Reader eingetragen. Die Unterstützung folgt später.",
                       "Reason: KOReader is registered as the EPUB reader. Support will follow later.")
                : settingsDialog.status.message === "Another EPUB reader is registered"
                ? Tr.t("Grund: Ein anderer Reader ist für EPUBs eingetragen. Better Stats lässt ihn unangetastet.",
                       "Reason: another reader is registered for EPUBs. Better Stats leaves it alone.")
                : Tr.t("Grund: ", "Reason: ") + settingsDialog.status.message
        }

        Rectangle {
            width: parent.width
            height: GlobalValues.defaultSolidSeparatorThickness
            color: GlobalValues.defaultBorderColor
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyLBold
            color: GlobalValues.defaultTextColor
            text: Tr.t("Updates", "Updates")
        }

        Item {
            width: parent.width
            height: Global.dp(64)

            Column {
                anchors.left: parent.left
                anchors.right: updateSwitch.left
                anchors.rightMargin: Global.dp(12)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Global.dp(4)

                StyledText {
                    width: parent.width
                    styledFont: FontStyles.BodyL
                    color: GlobalValues.defaultTextColor
                    text: Tr.t("Automatisch nach Updates suchen",
                               "Check for updates automatically")
                }

                StyledText {
                    width: parent.width
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultDisabledTextColor
                    wrapMode: Text.Wrap
                    text: Tr.t(
                        "Prüft beim Start über bekanntes WLAN; Installation nach Bestätigung.",
                        "Checks on launch over known Wi-Fi; installs after confirmation.")
                }
            }

            Rectangle {
                id: updateSwitch
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: Global.dp(52)
                height: Global.dp(30)
                radius: height / 2
                color: stats.automaticUpdates
                       ? GlobalValues.defaultTextColor
                       : GlobalValues.defaultBackgroundColor
                border.width: GlobalValues.dialogBorderWidth
                border.color: GlobalValues.defaultTextColor

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    x: stats.automaticUpdates
                       ? parent.width - width - Global.dp(4) : Global.dp(4)
                    width: Global.dp(20)
                    height: width
                    radius: width / 2
                    color: stats.automaticUpdates
                           ? GlobalValues.defaultBackgroundColor
                           : GlobalValues.defaultTextColor
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: stats.setAutomaticUpdates(!stats.automaticUpdates)
            }
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            text: Tr.t("Installiert: ", "Installed: ")
                  + (stats.currentVersion === "" ? "–" : stats.currentVersion)
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyS
            color: stats.updateState === "error"
                   ? GlobalValues.defaultTextColor
                   : GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: settingsDialog.updateStatusText()
        }

        Rectangle {
            property bool busy: stats.updateState === "checking"
                                || stats.updateState === "downloading"
                                || stats.updateState === "restarting"
            width: parent.width
            height: Global.dp(48)
            color: GlobalValues.defaultBackgroundColor
            border.width: GlobalValues.dialogBorderWidth
            border.color: busy ? GlobalValues.defaultDisabledTextColor
                               : GlobalValues.defaultTextColor

            StyledText {
                anchors.centerIn: parent
                styledFont: FontStyles.BodyLBold
                color: parent.busy ? GlobalValues.defaultDisabledTextColor
                                   : GlobalValues.defaultTextColor
                text: Tr.t("Jetzt prüfen", "Check now")
            }

            MouseArea {
                anchors.fill: parent
                enabled: !parent.busy
                onClicked: stats.checkForUpdates()
            }
        }

        Rectangle {
            visible: stats.updateState === "available"
            width: parent.width
            height: visible ? Global.dp(48) : 0
            color: GlobalValues.defaultTextColor

            StyledText {
                anchors.centerIn: parent
                styledFont: FontStyles.BodyLBold
                color: GlobalValues.defaultBackgroundColor
                text: Tr.t("Update installieren", "Install update")
            }

            MouseArea {
                anchors.fill: parent
                onClicked: stats.installUpdate()
            }
        }
    }
}
