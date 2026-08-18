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
}
