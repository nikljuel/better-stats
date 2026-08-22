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

        // AppHeader 2.0 places child controls in its right-hand action row.
        Item {
            width: GlobalValues.defaultListItemHeight
            height: GlobalValues.defaultListItemHeight

            StyledText {
                anchors.centerIn: parent
                styledFont: FontStyles.Heading3
                color: GlobalValues.defaultTextColor
                text: "⚙"
            }

            MouseArea {
                anchors.fill: parent
                onClicked: {
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

    PanelDialog {
        id: settingsDialog

        title: Tr.t("Einstellungen", "Settings")
        property var status: ({ enabled: false, available: false, message: "" })

        function refresh() {
            status = stats.autostartStatus()
        }

        SettingsBitmapTextSwitcher {
            width: parent.width
            height: GlobalValues.defaultListItemHeight
            title: Tr.t("Autostart", "Autostart")
            switch_value: settingsDialog.status.enabled === true
            enabled: settingsDialog.status.available === true
                     || settingsDialog.status.enabled === true
            opacity: enabled ? 1 : 0.45

            onAction: {
                var wanted = !settingsDialog.status.enabled
                var result = stats.setAutostartEnabled(wanted)
                settingsDialog.status = result
                if (result.enabled === wanted) {
                    setupMessage.message = wanted
                        ? Tr.t("Autostart aktiviert", "Autostart enabled")
                        : Tr.t("Autostart deaktiviert", "Autostart disabled")
                } else if (result.message === "KOReader association detected") {
                    setupMessage.message = Tr.t(
                        "KOReader erkannt. Die Unterstützung folgt in einem späteren Schritt.",
                        "KOReader detected. Support will follow in a later step.")
                } else {
                    setupMessage.message = Tr.t(
                        "Autostart konnte nicht geändert werden: ",
                        "Could not change autostart: ") + (result.message || "?")
                }
                setupMessage.visible = true
            }
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: Tr.t(
                "Startet das Tracking automatisch beim Öffnen eines EPUBs – auch beim letzten Buch nach einem Neustart.",
                "Starts tracking automatically when an EPUB opens, including the last book after a restart.")
        }

        StyledText {
            visible: settingsDialog.status.message !== ""
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: settingsDialog.status.message === "KOReader association detected"
                ? Tr.t("KOReader-Zuordnung erkannt; Autostart ist vorerst nicht verfügbar.",
                       "KOReader association detected; autostart is not available yet.")
                : settingsDialog.status.message
        }
    }

    InfoMessage {
        id: setupMessage

        anchors.fill: parent
        visible: false
        onClose: setupMessage.visible = false
    }
}
