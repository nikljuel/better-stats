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
    }
}
