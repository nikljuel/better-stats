import QtQuick
import com.pocketbook.controls
import "."

Item {
    id: tab

    property int year: new Date().getFullYear()
    property int month: new Date().getMonth() + 1
    property var m: ({ ndays: 30, firstWeekday: 0, days: [], books: [] })

    readonly property var monthNames: Tr.monthsFull

    function refresh() {
        m = stats.month(year, month);
    }

    function shiftMonth(delta) {
        var mo = month + delta;
        var y = year;
        if (mo < 1) { mo = 12; y--; }
        if (mo > 12) { mo = 1; y++; }
        month = mo;
        year = y;
        refresh();
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    readonly property int rows: Math.ceil((m.firstWeekday + m.ndays) / 7)
    readonly property real sideMargin: GlobalValues.defaultViewSideMargin

    // Month row with large paging zones
    Item {
        id: monthRow

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: GlobalValues.defaultListItemHeight

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: tab.sideMargin
            styledFont: FontStyles.Heading3
            color: GlobalValues.defaultTextColor
            text: "‹"
        }

        StyledText {
            anchors.centerIn: parent
            styledFont: FontStyles.Heading4
            color: GlobalValues.defaultTextColor
            text: tab.monthNames[tab.month - 1] + " " + tab.year
        }

        StyledText {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: tab.sideMargin
            styledFont: FontStyles.Heading3
            color: GlobalValues.defaultTextColor
            text: "›"
        }

        MouseArea {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: parent.width / 3
            onClicked: tab.shiftMonth(-1)
        }

        MouseArea {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: parent.width / 3
            onClicked: tab.shiftMonth(1)
        }
    }

    // Weekday headers
    Row {
        id: weekdayRow

        anchors.top: monthRow.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: tab.sideMargin
        anchors.rightMargin: tab.sideMargin
        height: Global.dp(30)

        Repeater {
            model: Tr.weekdaysShort

            Item {
                required property string modelData
                width: parent.width / 7
                height: parent.height

                StyledText {
                    anchors.centerIn: parent
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultDisabledTextColor
                    text: modelData
                }
            }
        }
    }

    function fmtHM(secs) { return Tr.fmtHM(secs); }

    function openDay(day, entry) {
        dayDialog.title = day + ". " + monthNames[month - 1]
                          + "  ·  " + fmtHM(entry.secs);
        dayDialog.entry = entry;
        dayDialog.visible = true;
    }

    // Grid: the cover of the most-read book per reading day
    Item {
        id: grid

        anchors.top: weekdayRow.bottom
        anchors.topMargin: Global.dp(6)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Global.dp(16)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: tab.sideMargin
        anchors.rightMargin: tab.sideMargin

        readonly property real cw: width / 7
        readonly property real ch: height / tab.rows

        Repeater {
            model: tab.m.ndays

            Rectangle {
                id: cell

                required property int index

                readonly property int day: index + 1
                readonly property int cellIdx: tab.m.firstWeekday + index
                readonly property var entry: (tab.m.days || [])[index]
                                             || ({ secs: 0, books: [] })
                readonly property var topBook: (entry.books || [])[0] || null

                x: (cellIdx % 7) * grid.cw
                y: Math.floor(cellIdx / 7) * grid.ch
                width: grid.cw
                height: grid.ch
                color: "transparent"
                border.width: 1
                border.color: GlobalValues.defaultBorderColor

                StyledText {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: Global.dp(5)
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultTextColor
                    text: cell.day
                }

                // Cover: full cell height, out to the right edge; the left
                // keeps room for the day number.
                Item {
                    id: coverBox

                    visible: cell.topBook !== null
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                    anchors.margins: Global.dp(3)
                    anchors.leftMargin: 0
                    width: grid.cw - Global.dp(30)

                    Image {
                        id: coverImg
                        anchors.fill: parent
                        source: cell.topBook ? (cell.topBook.coverUrl || "") : ""
                        visible: source != "" && status === Image.Ready
                        fillMode: Image.PreserveAspectFit
                        horizontalAlignment: Image.AlignRight
                    }

                    Rectangle {
                        visible: !coverImg.visible
                        anchors.fill: parent
                        color: "transparent"
                        border.width: 1
                        border.color: GlobalValues.defaultBorderColor

                        StyledText {
                            anchors.centerIn: parent
                            styledFont: FontStyles.BodyLBold
                            color: GlobalValues.defaultDisabledTextColor
                            text: cell.topBook
                                  ? (cell.topBook.title || "?").charAt(0) : ""
                        }
                    }

                    // "+n" when several books on that day
                    Rectangle {
                        visible: (cell.entry.books || []).length > 1
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: -Global.dp(4)
                        width: Global.dp(26)
                        height: Global.dp(26)
                        radius: width / 2
                        color: GlobalValues.defaultTextColor

                        StyledText {
                            anchors.centerIn: parent
                            styledFont: FontStyles.BodyXS
                            color: GlobalValues.defaultBackgroundColor
                            text: "+" + ((cell.entry.books || []).length - 1)
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: (cell.entry.secs || 0) > 0
                    onClicked: tab.openDay(cell.day, cell.entry)
                }
            }
        }
    }

    // Day dialog: covers side by side, only the reading time per book below;
    // total time is in the title row.
    PanelDialog {
        id: dayDialog

        property var entry: ({ secs: 0, books: [] })

        Flow {
            width: parent.width
            spacing: Global.dp(16)

            Repeater {
                model: dayDialog.entry.books || []

                Column {
                    required property var modelData
                    spacing: Global.dp(6)

                    Item {
                        width: Global.dp(100)
                        height: Global.dp(150)

                        Image {
                            id: dlgCover
                            anchors.fill: parent
                            source: modelData.coverUrl || ""
                            visible: source != "" && status === Image.Ready
                            fillMode: Image.PreserveAspectFit
                        }

                        Rectangle {
                            visible: !dlgCover.visible
                            anchors.fill: parent
                            color: "transparent"
                            border.width: 1
                            border.color: GlobalValues.defaultBorderColor

                            StyledText {
                                anchors.centerIn: parent
                                styledFont: FontStyles.Heading3
                                color: GlobalValues.defaultDisabledTextColor
                                text: (modelData.title || "?").charAt(0)
                            }
                        }
                    }

                    StyledText {
                        anchors.horizontalCenter: parent.horizontalCenter
                        styledFont: FontStyles.BodyS
                        color: GlobalValues.defaultTextColor
                        text: tab.fmtHM(modelData.secs)
                    }
                }
            }
        }
    }
}
