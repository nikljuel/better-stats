import QtQuick
import com.pocketbook.controls

/* Modal panel in the firmware look: title row with X, content as children.
 * Closes via X or a tap on the dimmed background. */
Item {
    id: dlg

    anchors.fill: parent
    visible: false
    z: 10

    property string title: ""
    default property alias content: contentSlot.data

    Rectangle {
        anchors.fill: parent
        color: GlobalValues.defaultTextColor
        opacity: 0.35

        MouseArea {
            anchors.fill: parent
            onClicked: dlg.visible = false
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(GlobalValues.defaultDialogWidth,
                        dlg.width - Global.dp(40))
        height: Math.min(
            Global.dp(88) + contentSlot.implicitHeight,
            dlg.height - Global.dp(80))
        color: GlobalValues.defaultBackgroundColor
        border.width: GlobalValues.dialogBorderWidth
        border.color: GlobalValues.defaultTextColor

        MouseArea { anchors.fill: parent } // swallows taps inside the panel

        Item {
            id: titleBar

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: Global.dp(16)
            anchors.leftMargin: Global.dp(20)
            anchors.rightMargin: Global.dp(20)
            height: Global.dp(40)

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.right: closeBox.left
                styledFont: FontStyles.Heading4
                color: GlobalValues.defaultTextColor
                text: dlg.title
                elide: Text.ElideRight
            }

            Item {
                id: closeBox
                anchors.right: parent.right
                anchors.top: parent.top
                width: Global.dp(48)
                height: Global.dp(48)

                StyledText {
                    anchors.centerIn: parent
                    styledFont: FontStyles.Heading4
                    color: GlobalValues.defaultTextColor
                    text: "X"
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: dlg.visible = false
                }
            }
        }

        Flickable {
            anchors.top: titleBar.bottom
            anchors.topMargin: Global.dp(16)
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: Global.dp(20)
            anchors.rightMargin: Global.dp(20)
            anchors.bottomMargin: Global.dp(16)
            contentHeight: contentSlot.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: contentSlot
                width: parent.width
                spacing: Global.dp(12)
            }
        }
    }
}
