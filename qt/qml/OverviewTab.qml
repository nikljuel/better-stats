import QtQuick
import com.pocketbook.controls
import "."

Item {
    id: tab

    property var ov: ({})
    property var books: []
    property int bookIdx: 0
    property var book: books.length > 0 ? books[bookIdx] : ({})

    function refresh() {
        ov = stats.overall();
        books = stats.readingBooks();
        if (bookIdx >= books.length)
            bookIdx = Math.max(0, books.length - 1);
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    // No scrolling: content fits on one screen (tabs instead of a scroll view)
    Item {
        anchors.fill: parent

        Column {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: GlobalValues.defaultViewSideMargin
            anchors.rightMargin: GlobalValues.defaultViewSideMargin
            spacing: Global.dp(20)

            Item { width: 1; height: Global.dp(12) }

            // Current book with side arrows
            Item {
                id: bookSection
                width: parent.width
                height: bookRow.height
                visible: tab.book.ok === true

                // Left arrow
                StyledText {
                    visible: tab.books.length > 1
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    styledFont: FontStyles.Heading3
                    color: GlobalValues.defaultTextColor
                    opacity: tab.bookIdx > 0 ? 1.0 : 0.3
                    text: "‹"
                }

                MouseArea {
                    visible: tab.books.length > 1
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width / 3
                    enabled: tab.bookIdx > 0
                    onClicked: tab.bookIdx--
                }

                // Right arrow
                StyledText {
                    visible: tab.books.length > 1
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    styledFont: FontStyles.Heading3
                    color: GlobalValues.defaultTextColor
                    opacity: tab.bookIdx < tab.books.length - 1 ? 1.0 : 0.3
                    text: "›"
                }

                MouseArea {
                    visible: tab.books.length > 1
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width / 3
                    enabled: tab.bookIdx < tab.books.length - 1
                    onClicked: tab.bookIdx++
                }

                Row {
                    id: bookRow
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - (tab.books.length > 1 ? Global.dp(60) : 0)
                    spacing: Global.dp(20)

                    Image {
                        id: cover
                        source: stats.inverted
                                ? "image://inverted-cover/" + encodeURIComponent(tab.book.coverUrl || "")
                                : tab.book.coverUrl || ""
                        visible: (tab.book.coverUrl || "") !== ""
                        width: Global.dp(110)
                        height: Global.dp(165)
                        fillMode: Image.PreserveAspectFit
                    }

                    Column {
                        width: parent.width
                               - (cover.visible ? cover.width + Global.dp(20) : 0)
                        spacing: Global.dp(8)

                        StyledText {
                            width: parent.width
                            styledFont: FontStyles.Heading4
                            color: GlobalValues.defaultTextColor
                            text: tab.book.title || ""
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }

                        StyledText {
                            width: parent.width
                            styledFont: FontStyles.BodyS
                            color: GlobalValues.defaultDisabledTextColor
                            text: tab.book.author || ""
                            elide: Text.ElideRight
                        }

                        Item { width: 1; height: Global.dp(6) }

                        StyledText {
                            styledFont: FontStyles.Body
                            color: GlobalValues.defaultTextColor
                            text: Tr.t("Fortschritt: ", "Progress: ") + (tab.book.percent || 0) + " %"
                        }

                        ProgressBar {
                            width: parent.width
                            height: Global.dp(10)
                            minValue: 0
                            maxValue: 100
                            value: tab.book.percent > 0 && width > 0
                                   ? Math.max(tab.book.percent,
                                              maxValue * height / width)
                                   : 0
                        }

                        StyledText {
                            styledFont: FontStyles.BodyS
                            color: GlobalValues.defaultDisabledTextColor
                            text: Tr.t("Gelesen: ", "Read: ") + Tr.fmtHM(tab.book.bookSecs)
                        }

                        StyledText {
                            visible: (tab.book.leftSecs || 0) > 0
                            styledFont: FontStyles.BodyS
                            color: GlobalValues.defaultDisabledTextColor
                            text: Tr.t("Noch ca. ", "About ") + Tr.fmtHM(tab.book.leftSecs)
                        }
                    }
                }
            }

            // Page dots
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: tab.books.length > 1
                spacing: Global.dp(8)
                Repeater {
                    model: tab.books.length
                    Rectangle {
                        required property int index
                        width: Global.dp(8)
                        height: width
                        radius: width / 2
                        color: index === tab.bookIdx
                               ? GlobalValues.defaultTextColor
                               : "transparent"
                        border.width: index === tab.bookIdx ? 0 : Global.dp(1)
                        border.color: GlobalValues.defaultTextColor
                        MouseArea {
                            anchors.fill: parent
                            onClicked: tab.bookIdx = index
                        }
                    }
                }
            }

            StyledText {
                visible: tab.books.length === 0
                styledFont: FontStyles.Body
                color: GlobalValues.defaultDisabledTextColor
                text: Tr.t("Noch kein Buch geöffnet", "No book opened yet")
            }

            Rectangle {
                width: parent.width
                height: GlobalValues.defaultSolidSeparatorThickness
                color: GlobalValues.defaultBorderColor
            }

            // Kennzahlen-Kacheln
            Row {
                width: parent.width

                Repeater {
                    model: [
                        { v: Tr.fmtHM(tab.ov.todaySecs), l: Tr.t("Gelesen heute", "Read today") },
                        { v: Math.round(tab.ov.avgSessionMin || 0) + "", l: Tr.t("Ø Min/Session", "Avg min/session") },
                        { v: ((tab.ov.pagesPerMin || 0) * 60).toFixed(0), l: Tr.t("Seiten pro Stunde", "Pages per hour") }
                    ]

                    Column {
                        required property var modelData
                        width: parent.width / 3
                        spacing: Global.dp(4)

                        StyledText {
                            styledFont: FontStyles.Heading2
                            color: GlobalValues.defaultTextColor
                            text: modelData.v
                        }

                        StyledText {
                            width: parent.width - Global.dp(12)
                            styledFont: FontStyles.BodyS
                            color: GlobalValues.defaultDisabledTextColor
                            text: modelData.l
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: GlobalValues.defaultSolidSeparatorThickness
                color: GlobalValues.defaultBorderColor
            }

            StyledText {
                styledFont: FontStyles.Caption1
                color: GlobalValues.defaultDisabledTextColor
                text: Tr.t("ALLE BÜCHER", "ALL BOOKS")
            }

            // Donut + figures in one row, caption below each
            Row {
                width: parent.width
                spacing: Global.dp(16)

                // Column 1: donut with caption below
                Column {
                    width: (parent.width - 2 * Global.dp(16)) / 3
                    spacing: Global.dp(8)

                    Item {
                        id: donut
                        width: Global.dp(110)
                        height: width

                        property real frac: tab.ov.finishedFrac || 0

                        onFracChanged: donutCanvas.requestPaint()

                        Canvas {
                            id: donutCanvas
                            anchors.fill: parent
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.reset();
                                var cx = width / 2, cy = height / 2;
                                var r = width / 2 - Global.dp(7);
                                ctx.lineWidth = Global.dp(13);
                                /* light ring: defaultBorderColor is nearly
                                 * black on the grayscale panel */
                                ctx.strokeStyle = "#d8d8d8";
                                ctx.beginPath();
                                ctx.arc(cx, cy, r, 0, 2 * Math.PI);
                                ctx.stroke();
                                if (donut.frac > 0) {
                                    ctx.strokeStyle = GlobalValues.defaultTextColor;
                                    ctx.beginPath();
                                    ctx.arc(cx, cy, r, -Math.PI / 2,
                                            -Math.PI / 2 + donut.frac * 2 * Math.PI);
                                    ctx.stroke();
                                }
                            }
                        }

                        StyledText {
                            anchors.centerIn: parent
                            styledFont: FontStyles.BodyLBold
                            color: GlobalValues.defaultTextColor
                            text: Math.round(donut.frac * 100) + " %"
                        }
                    }

                    StyledText {
                        width: parent.width - Global.dp(8)
                        styledFont: FontStyles.BodyS
                        color: GlobalValues.defaultDisabledTextColor
                        text: Tr.t("deiner Bücher beendet", "of your books finished")
                        wrapMode: Text.Wrap
                    }
                }

                // Columns 2+3: number on top (at donut height), caption below
                Repeater {
                    model: [
                        { v: (tab.ov.booksFinished || 0) + "", l: Tr.t("Bücher beendet", "Books finished") },
                        { v: (tab.ov.totalHours || 0).toFixed(1), l: Tr.t("Lesezeit gesamt", "Total reading time") }
                    ]

                    Column {
                        required property var modelData
                        width: (parent.width - 2 * Global.dp(16)) / 3
                        spacing: Global.dp(8)

                        Item {
                            width: parent.width
                            height: donut.height

                            StyledText {
                                anchors.verticalCenter: parent.verticalCenter
                                styledFont: FontStyles.Heading2
                                color: GlobalValues.defaultTextColor
                                text: modelData.v
                            }
                        }

                        StyledText {
                            width: parent.width - Global.dp(8)
                            styledFont: FontStyles.BodyS
                            color: GlobalValues.defaultDisabledTextColor
                            text: modelData.l
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Item { width: 1; height: Global.dp(24) }
        }
    }
}
