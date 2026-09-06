import QtQuick
import com.pocketbook.controls
import "."

Item {
    id: tab

    property var yb: ({ months: [], total: 0 })
    property int yearNum: new Date().getFullYear()
    readonly property int thisYear: new Date().getFullYear()
    readonly property int monthCount: yearNum === thisYear
                                      ? new Date().getMonth() + 1 : 12
    readonly property var monthNames: Tr.monthsShort
    readonly property var monthNamesFull: Tr.monthsFull

    function openMonth(idx) {
        var books = ((yb.months || [])[idx]) || [];
        if (books.length === 0)
            return;
        monthDialog.title = monthNamesFull[idx] + " " + yearNum + "  ·  "
                            + books.length + (books.length === 1
                                ? Tr.t(" Buch", " book", " livre", " libro")
                                : Tr.t(" Bücher", " books", " livres", " libros"));
        monthDialog.books = books;
        monthDialog.visible = true;
    }
    readonly property real sideMargin: GlobalValues.defaultViewSideMargin

    readonly property int maxCount: {
        var mx = 1;
        var ms = yb.months || [];
        for (var i = 0; i < ms.length; i++)
            if ((ms[i] || []).length > mx)
                mx = ms[i].length;
        return mx;
    }

    function shiftYear(delta) {
        yearNum += delta;
        refresh();
    }

    function refresh() {
        yb = stats.yearBooks(yearNum);
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    Column {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: tab.sideMargin
        anchors.rightMargin: tab.sideMargin
        spacing: Global.dp(10)

        Item {
            width: parent.width
            height: GlobalValues.defaultListItemHeight

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                styledFont: FontStyles.Heading3
                color: GlobalValues.defaultTextColor
                opacity: tab.yearNum > 2015 ? 1.0 : 0.3
                text: "‹"
            }

            Row {
                anchors.centerIn: parent
                spacing: Global.dp(12)

                StyledText {
                    styledFont: FontStyles.Heading1
                    color: GlobalValues.defaultTextColor
                    text: tab.yb.total || 0
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    styledFont: FontStyles.Body
                    color: GlobalValues.defaultDisabledTextColor
                    text: Tr.t("Bücher beendet in ", "Books finished in ",
                              "Livres terminés en ", "Libros terminados en ") + tab.yearNum
                }
            }

            StyledText {
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                styledFont: FontStyles.Heading3
                color: GlobalValues.defaultTextColor
                opacity: tab.yearNum < tab.thisYear ? 1.0 : 0.3
                text: "›"
            }

            MouseArea {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width / 3
                enabled: tab.yearNum > 2015
                onClicked: tab.shiftYear(-1)
            }

            MouseArea {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width / 3
                enabled: tab.yearNum < tab.thisYear
                onClicked: tab.shiftYear(1)
            }
        }

        Rectangle {
            width: parent.width
            height: GlobalValues.defaultSolidSeparatorThickness
            color: GlobalValues.defaultBorderColor
        }
    }

    // Month rows: height adapts so everything is visible without scrolling
    Item {
        id: rowsArea

        anchors.top: header.bottom
        anchors.topMargin: Global.dp(10)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Global.dp(14)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: tab.sideMargin
        anchors.rightMargin: tab.sideMargin

        readonly property real rowH: height / tab.monthCount
        readonly property real barH: Math.min(Global.dp(44), rowH - Global.dp(8))

        Repeater {
            model: tab.monthCount

            Item {
                required property int index

                readonly property var books:
                    ((tab.yb.months || [])[index]) || []

                x: 0
                y: index * rowsArea.rowH
                width: rowsArea.width
                height: rowsArea.rowH

                StyledText {
                    id: monthLabel
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    width: Global.dp(52)
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultDisabledTextColor
                    text: tab.monthNames[index]
                }

                Rectangle {
                    anchors.left: monthLabel.right
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: rowsArea.barH
                    radius: Global.dp(6)
                    color: "#eeeeee"
                }

                Rectangle {
                    visible: books.length > 0
                    anchors.left: monthLabel.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: (parent.width - monthLabel.width)
                           * books.length / tab.maxCount
                    height: rowsArea.barH
                    radius: Global.dp(6)
                    color: "#c4c4c4"
                }

                Row {
                    anchors.left: monthLabel.right
                    anchors.leftMargin: Global.dp(6)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Global.dp(4)

                    Repeater {
                        model: books

                        Item {
                            required property var modelData

                            width: height * 0.7
                            height: rowsArea.barH - Global.dp(6)

                            Image {
                                id: mini
                                anchors.fill: parent
                                source: stats.inverted
                                        ? "image://inverted-cover/" + encodeURIComponent(modelData.coverUrl || "")
                                        : modelData.coverUrl || ""
                                visible: source != "" && status === Image.Ready
                                fillMode: Image.PreserveAspectCrop
                            }

                            Rectangle {
                                visible: !mini.visible
                                anchors.fill: parent
                                color: GlobalValues.defaultBackgroundColor
                                border.width: 1
                                border.color: GlobalValues.defaultTextColor

                                StyledText {
                                    anchors.centerIn: parent
                                    styledFont: FontStyles.BodyXS
                                    color: GlobalValues.defaultTextColor
                                    text: (modelData.title || "?").charAt(0)
                                }
                            }
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: books.length > 0
                    onClicked: tab.openMonth(index)
                }
            }
        }
    }

    // Month dialog: covers side by side, finish date below
    PanelDialog {
        id: monthDialog

        property var books: []

        Flow {
            width: parent.width
            spacing: Global.dp(16)

            Repeater {
                model: monthDialog.books

                Column {
                    required property var modelData
                    spacing: Global.dp(6)

                    Item {
                        width: Global.dp(100)
                        height: Global.dp(150)

                        Image {
                            id: dlgCover
                            anchors.fill: parent
                            source: stats.inverted
                                    ? "image://inverted-cover/" + encodeURIComponent(modelData.coverUrl || "")
                                    : modelData.coverUrl || ""
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
                        text: modelData.dateStr || ""
                    }
                }
            }
        }
    }
}
