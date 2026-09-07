import QtQuick
import com.pocketbook.controls
import "."

Item {
    id: tab

    property var yr: ({ heat: [], startWeekday: 0, ndays: 365, daysRead: 0,
                        currentStreak: 0, bestStreak: 0, bestStreakStart: "" })
    readonly property int yearNum: new Date().getFullYear()
    readonly property real sideMargin: GlobalValues.defaultViewSideMargin

    function refresh() {
        yr = stats.year(yearNum);
    }

    function insightText() {
        if (!(yr.bestStreak > 0) || yr.bestStreakStart === "")
            return Tr.t("Noch keine Lese-Serie — heute ist ein guter Tag, um eine zu starten.",
                        "No reading streak yet — today is a good day to start one.");
        var d = new Date(yr.bestStreakStart);
        var when = Tr.de
            ? (d.getDate() + ". " + Tr.monthsFull[d.getMonth()])
            : (Tr.monthsFull[d.getMonth()] + " " + d.getDate());
        return Tr.de
            ? ("Deine längste Serie begann am " + when + " und hielt "
               + yr.bestStreak + (yr.bestStreak === 1 ? " Tag." : " Tage."))
            : ("Your longest streak began on " + when + " and lasted "
               + yr.bestStreak + (yr.bestStreak === 1 ? " day." : " days."));
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    // Header block
    Column {
        id: headBlock

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: tab.sideMargin
        anchors.rightMargin: tab.sideMargin
        spacing: Global.dp(10)

        Item { width: 1; height: Global.dp(10) }

        Row {
            width: parent.width

            Repeater {
                model: [
                    { v: (tab.yr.currentStreak || 0), l: Tr.t("Tage aktuelle Serie", "Days current streak") },
                    { v: (tab.yr.bestStreak || 0), l: Tr.t("Tage beste Serie ", "Days best streak ") + tab.yearNum }
                ]

                Column {
                    required property var modelData
                    width: parent.width / 2
                    spacing: Global.dp(2)

                    StyledText {
                        styledFont: FontStyles.Heading1
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

        StyledText {
            width: parent.width
            styledFont: FontStyles.Body
            color: GlobalValues.defaultTextColor
            text: tab.insightText()
            wrapMode: Text.Wrap
        }

        StyledText {
            styledFont: FontStyles.Caption1
            color: GlobalValues.defaultDisabledTextColor
            text: (tab.yr.daysRead || 0) + Tr.t(" LESETAGE IN ", " READING DAYS IN ") + tab.yearNum
        }
    }

    // Legend pinned to the bottom edge — the heatmap between adapts.
    Row {
        id: legend

        anchors.bottom: parent.bottom
        anchors.bottomMargin: Global.dp(14)
        anchors.left: parent.left
        anchors.leftMargin: tab.sideMargin
        spacing: Global.dp(20)

        Repeater {
            model: [
                { c: "#d8d8d8", dot: false, l: Tr.t("nicht gelesen", "not read") },
                { c: GlobalValues.defaultTextColor, dot: false, l: Tr.t("gelesen", "read") },
                { c: GlobalValues.defaultTextColor, dot: true, l: Tr.t("Buch beendet", "book finished") }
            ]

            Row {
                required property var modelData
                spacing: Global.dp(6)

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Global.dp(16)
                    height: Global.dp(16)
                    radius: Global.dp(3)
                    color: modelData.c

                    Rectangle {
                        visible: modelData.dot
                        anchors.centerIn: parent
                        width: Global.dp(6)
                        height: width
                        radius: width / 2
                        color: GlobalValues.defaultBackgroundColor
                    }
                }

                StyledText {
                    anchors.verticalCenter: parent.verticalCenter
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultDisabledTextColor
                    text: modelData.l
                }
            }
        }
    }

    // Year heatmap: two half-year blocks; cell size adapts to the available
    // height AND width — nothing spills off the screen.
    Item {
        id: heatmap

        anchors.top: headBlock.bottom
        anchors.topMargin: Global.dp(14)
        anchors.bottom: legend.top
        anchors.bottomMargin: Global.dp(14)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: tab.sideMargin
        anchors.rightMargin: tab.sideMargin

        readonly property int weeksPerBlock: 27
        readonly property real labelH: Global.dp(26)
        readonly property real blockGap: Global.dp(10)

        readonly property real cellFromW:
            width / (weeksPerBlock + (weeksPerBlock - 1) * 0.18)
        readonly property real cellFromH:
            ((height - blockGap) / 2 - labelH) / (7 + 6 * 0.18)
        readonly property real cell: Math.max(4, Math.min(cellFromW, cellFromH))
        readonly property real gap: cell * 0.18
        readonly property real blockH: 7 * cell + 6 * gap + labelH

        function cellX(week) {
            return (week % weeksPerBlock) * (cell + gap);
        }

        function blockY(week) {
            return week >= weeksPerBlock ? blockH + blockGap : 0;
        }

        Repeater {
            model: tab.yr.ndays || 0

            Rectangle {
                required property int index

                readonly property int slot: index + (tab.yr.startWeekday || 0)
                readonly property int week: Math.floor(slot / 7)
                readonly property int state_: (tab.yr.heat || [])[index] || 0

                x: heatmap.cellX(week)
                y: heatmap.blockY(week) + (slot % 7) * (heatmap.cell + heatmap.gap)
                width: heatmap.cell
                height: heatmap.cell
                radius: Math.max(1, heatmap.cell * 0.15)
                color: state_ > 0 ? GlobalValues.defaultTextColor : "#d8d8d8"

                Rectangle {
                    visible: parent.state_ === 2
                    anchors.centerIn: parent
                    width: Math.max(4, heatmap.cell * 0.3)
                    height: width
                    radius: width / 2
                    color: GlobalValues.defaultBackgroundColor
                }
            }
        }

        // Month labels beneath each block
        Repeater {
            model: Tr.monthsShort

            StyledText {
                required property string modelData
                required property int index

                readonly property int firstDoy: {
                    var doy = 0;
                    var dim = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
                    if ((tab.yr.ndays || 365) === 366)
                        dim[1] = 29;
                    for (var i = 0; i < index; i++)
                        doy += dim[i];
                    return doy;
                }
                readonly property int week:
                    Math.floor((firstDoy + (tab.yr.startWeekday || 0)) / 7)

                x: heatmap.cellX(week)
                y: heatmap.blockY(week) + 7 * heatmap.cell + 6 * heatmap.gap
                   + Global.dp(4)
                styledFont: FontStyles.BodyXS
                color: GlobalValues.defaultDisabledTextColor
                text: modelData
            }
        }
    }
}
