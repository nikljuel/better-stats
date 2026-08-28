import QtQuick
import com.pocketbook.controls
import "."

Item {
    id: tab

    property var ov: ({})
    property var books: []
    property int bookIdx: 0
    property var book: books.length > 0 ? books[bookIdx] : ({})
    // Hardcover sync additions below (ported from a downstream fork - see
    // its own PR description). match is imperative, not a declarative
    // binding on book.bookId, matching how it's kept in sync elsewhere in
    // this file: hardcover.matchedEditionChanged(bookId) fires after a
    // successful link/sync even when book itself hasn't changed at all
    // (a plain property binding wouldn't re-evaluate on a signal, only on
    // an actual property change), so this needs the same explicit
    // re-fetch refresh() already does for everything else.
    property var match: ({})

    // "2026-08-21" -> "21 Aug 2026" (plain, no weekday/ordinal - used for
    // the auto-match popup's publish date, matching the matched-edition
    // widget's own style elsewhere in this file).
    function friendlyDateShort(iso) {
        if (!iso)
            return "";
        var d = new Date(iso + "T00:00:00");
        if (isNaN(d.getTime()))
            return iso;
        var months = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
        return d.getDate() + " " + months[d.getMonth()] + " " + d.getFullYear();
    }

    function languageFlag(code) {
        var m = {
            en: "🇬🇧", de: "🇩🇪", fr: "🇫🇷", es: "🇪🇸", it: "🇮🇹", pt: "🇵🇹",
            nl: "🇳🇱", pl: "🇵🇱", ru: "🇷🇺", ja: "🇯🇵", zh: "🇨🇳", ko: "🇰🇷", sv: "🇸🇪"
        };
        return m[code] || "";
    }

    function refresh() {
        ov = stats.overall();
        books = stats.readingBooks();
        if (bookIdx >= books.length)
            bookIdx = Math.max(0, books.length - 1);
        match = tab.book.ok === true ? hardcover.matchedEdition(tab.book.bookId) : ({});
    }

    // Switching books via the page dots below sets bookIdx directly,
    // rather than going through a named function the way refresh() itself
    // is called - match needs its own hook here too, or the Hardcover
    // section would keep showing the previous book's matched edition
    // after switching.
    onBookIdxChanged: match = tab.book.ok === true ? hardcover.matchedEdition(tab.book.bookId) : ({})

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    // --- Hardcover sync signal handlers, ported from a downstream fork -
    // see its own PR description for the full feature. ---
    Connections {
        target: hardcover

        function onMatchedEditionChanged(bookId) {
            if (bookId === tab.book.bookId)
                tab.refresh();
        }

        function onAutoMatchFound(bookId, edition) {
            autoMatchDialog.bookId = bookId;
            autoMatchDialog.edition = edition;
            autoMatchDialog.visible = true;
        }
        function onAutoMatchNotFound(bookId) {
            bookList.model = [];
            bookList.loading = true;
            bookDialog.visible = true;
            hardcover.searchBook(bookId);
        }
        function onBookSearchReady(bookId, books) {
            bookList.model = books;
            bookList.loading = false;
            if (books.length === 1) {
                bookDialog.visible = false;
                editionList.model = [];
                editionList.loading = true;
                editionDialog.visible = true;
                hardcover.pickBook(bookId, books[0].hcBookId);
            }
        }
        function onEditionsReady(bookId, editions) {
            editionList.model = editions;
            editionList.loading = false;
        }
        function onEditionMatched(bookId, ok, error) {
            if (!ok) {
                infoMessage.message = Tr.t("Fehler: ", "Error: ") + error;
                infoMessage.icon = InfoMessage.ErrorIcon;
                infoMessage.visible = true;
            }
        }
        function onAlreadyReading(bookId, startedAt) {
            infoMessage.message = Tr.t(
                "Buch bereits als Wird gelesen markiert, Startdatum: " + Tr.friendlyDate(startedAt),
                "Book already marked as Currently Reading, with start date: " + Tr.friendlyDate(startedAt));
            infoMessage.icon = InfoMessage.InformationIcon;
            infoMessage.visible = true;
        }
        function onNeedsReadingConfirm(bookId, todayDate) {
            readingConfirmDialog.bookId = bookId;
            readingConfirmDialog.message = Tr.t(
                "Dieses Buch als Wird gelesen markieren, mit Startdatum: " + Tr.friendlyDate(todayDate) + "?",
                "Mark this book as Currently Reading, with start date: " + Tr.friendlyDate(todayDate) + "?");
            readingConfirmDialog.visible = true;
        }
        function onReadingConfirmed(bookId, ok, error) {
            infoMessage.message = ok
                ? Tr.t("Als \u201eWird gelesen\u201c markiert.", "Marked as currently reading.")
                : Tr.t("Fehler: ", "Error: ") + error;
            infoMessage.icon = ok ? InfoMessage.DoneIcon : InfoMessage.ErrorIcon;
            infoMessage.visible = true;
        }
        function onNeedsFinishConfirm(bookId, finishedAtDate) {
            finishConfirmDialog.bookId = bookId;
            finishConfirmDialog.message = Tr.t(
                "Dieses Buch als gelesen markieren, mit Enddatum: " + Tr.friendlyDate(finishedAtDate) + "?",
                "Mark this book as Read, with finish date: " + Tr.friendlyDate(finishedAtDate) + "?");
            finishConfirmDialog.visible = true;
        }
        function onFinishConfirmed(bookId, ok, error) {
            infoMessage.message = ok
                ? Tr.t("Als gelesen markiert.", "Marked as read.")
                : Tr.t("Fehler: ", "Error: ") + error;
            infoMessage.icon = ok ? InfoMessage.DoneIcon : InfoMessage.ErrorIcon;
            infoMessage.visible = true;
        }
        function onProgressPushed(bookId, ok, error) {
            infoMessage.message = ok
                ? Tr.t("Fortschritt synchronisiert.", "Progress synced.")
                : Tr.t("Fehler: ", "Error: ") + error;
            infoMessage.icon = ok ? InfoMessage.DoneIcon : InfoMessage.ErrorIcon;
            infoMessage.visible = true;
        }
    }

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
                        source: tab.book.coverUrl || ""
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
                            value: tab.book.percent || 0
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

            // --- Hardcover sync integration, ported from a downstream
            // fork - see its own PR description for the full feature.
            // Placed directly under the currently-reading section above,
            // matching that fork's own layout. ---

            // --- token entry, only shown until one is set ---
            Column {
                width: parent.width
                spacing: Global.dp(8)
                visible: !hardcover.hasToken

                StyledText {
                    width: parent.width
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultDisabledTextColor
                    wrapMode: Text.Wrap
                    text: Tr.t(
                        "Hardcover-Token eintragen, um den Abgleich zu aktivieren.",
                        "Enter a Hardcover token to enable syncing.")
                }

                Rectangle {
                    width: parent.width
                    height: Global.dp(48)
                    color: "transparent"
                    border.width: GlobalValues.dialogBorderWidth
                    border.color: GlobalValues.defaultBorderColor

                    TextInput {
                        id: tokenField
                        anchors.fill: parent
                        anchors.margins: Global.dp(10)
                        verticalAlignment: TextInput.AlignVCenter
                        font.pixelSize: FontStyles.BodyM.pixelSize
                        color: GlobalValues.defaultTextColor
                        echoMode: focus ? TextInput.Normal : TextInput.Password
                        clip: true
                    }
                }

                RoundedCornerTextButton {
                    width: parent.width
                    height: GlobalValues.defaultTextButtonHeight
                    title: Tr.t("Token speichern", "Save token")
                    radius: GlobalValues.defaultElementBorderRadius
                    border.width: GlobalValues.defaultPressedFrameBorderWidth
                    border.color: GlobalValues.defaultBorderColor
                    onClicked: {
                        hardcover.setToken(tokenField.text);
                        tokenField.text = "";
                    }
                }
            }

            StyledText {
                styledFont: FontStyles.Caption1
                color: GlobalValues.defaultDisabledTextColor
                text: Tr.t("HARDCOVER-INTEGRATION", "HARDCOVER INTEGRATION")
            }

            Row {
                width: parent.width
                spacing: Global.dp(10)
                visible: tab.match.hasMatch === true

                // PreserveAspectCrop (not Fit) so this always fully fills
                // its own fixed box regardless of this edition's own
                // cover aspect ratio, with clip: true since
                // PreserveAspectCrop otherwise renders outside the
                // Image's own bounds.
                Image {
                    source: tab.match.coverUrl || ""
                    visible: (tab.match.coverUrl || "") !== ""
                    width: Global.dp(32)
                    height: Global.dp(48)
                    fillMode: Image.PreserveAspectCrop
                    clip: true
                    asynchronous: false
                }

                StyledText {
                    width: parent.width - Global.dp(32) - Global.dp(20)
                    anchors.verticalCenter: parent.verticalCenter
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultTextColor
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    text: {
                        var parts = [];
                        if (tab.match.formatLabel) parts.push(tab.match.formatLabel);
                        var isbn = tab.match.isbn13 || tab.match.isbn10;
                        if (isbn) parts.push(isbn);
                        if (tab.match.publisher) parts.push(tab.match.publisher);
                        var pubDate = tab.friendlyDateShort(tab.match.releaseDate || "");
                        if (pubDate) parts.push(pubDate);
                        if (tab.match.pages) parts.push(tab.match.pages + Tr.t(" Seiten", " pages"));
                        return parts.join("  ·  ");
                    }
                }
            }

            StyledText {
                visible: tab.match.hasMatch !== true
                width: parent.width
                height: Global.dp(48)
                verticalAlignment: Text.AlignVCenter
                styledFont: FontStyles.BodyM
                color: GlobalValues.defaultDisabledTextColor
                text: Tr.t("Noch keine Edition zugeordnet", "No edition matched yet")
            }

            Row {
                width: parent.width
                spacing: Global.dp(12)
                enabled: tab.book.ok === true && hardcover.hasToken && !hardcover.busy

                RoundedCornerTextButton {
                    width: (parent.width - Global.dp(12)) / 2
                    height: GlobalValues.defaultTextButtonHeight
                    title: Tr.t("Buch verknüpfen", "Link book")
                    radius: GlobalValues.defaultElementBorderRadius
                    border.width: GlobalValues.defaultPressedFrameBorderWidth
                    border.color: GlobalValues.defaultBorderColor
                    enabled: parent.enabled
                    onClicked: hardcover.linkBook(tab.book.bookId)
                }

                RoundedCornerTextButton {
                    width: (parent.width - Global.dp(12)) / 2
                    height: GlobalValues.defaultTextButtonHeight
                    title: hardcover.busy
                           ? Tr.t("Wird abgeglichen…", "Syncing…")
                           : Tr.t("Fortschritt synchronisieren", "Sync progress")
                    radius: GlobalValues.defaultElementBorderRadius
                    border.width: GlobalValues.defaultPressedFrameBorderWidth
                    border.color: GlobalValues.defaultBorderColor
                    enabled: parent.enabled && tab.match.hasMatch === true
                    onClicked: hardcover.syncProgress(tab.book.bookId)
                }
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

    // --- Hardcover sync dialogs, ported from a downstream fork - see its
    // own PR description for the full feature. ---

    // --- auto-match confirmation: needs a cover image, so this is a
    // custom PanelDialog (like the matched-edition widget), not the native
    // ActionConfirmationDialog (text-only, no image support). ---
    PanelDialog {
        id: autoMatchDialog
        title: Tr.t("Automatisch gefunden", "Automatic match found")

        property var bookId: -1
        property var edition: ({})

        Row {
            width: parent.width
            spacing: Global.dp(12)

            Image {
                source: autoMatchDialog.edition.coverUrl || ""
                visible: (autoMatchDialog.edition.coverUrl || "") !== ""
                width: Global.dp(70)
                height: Global.dp(104)
                fillMode: Image.PreserveAspectFit
            }

            Column {
                width: parent.width - Global.dp(82)
                spacing: Global.dp(4)

                StyledText {
                    width: parent.width
                    styledFont: FontStyles.BodyMBold
                    color: GlobalValues.defaultTextColor
                    text: autoMatchDialog.edition.title || ""
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }

                StyledText {
                    width: parent.width
                    styledFont: FontStyles.BodyS
                    color: GlobalValues.defaultDisabledTextColor
                    wrapMode: Text.Wrap
                    text: {
                        var parts = [];
                        var e = autoMatchDialog.edition;
                        var fmt = hardcover.formatLabel(e.readingFormatId || 0);
                        if (fmt) parts.push(fmt);
                        var isbn = e.isbn13 || e.isbn10;
                        if (isbn) parts.push(isbn);
                        if (e.publisher) parts.push(e.publisher);
                        var pubDate = tab.friendlyDateShort(e.releaseDate || "");
                        if (pubDate) parts.push(pubDate);
                        if (e.pages) parts.push(e.pages + Tr.t(" Seiten", " pages"));
                        return parts.join("  ·  ");
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: Global.dp(12)

            RoundedCornerTextButton {
                width: (parent.width - Global.dp(12)) / 2
                height: GlobalValues.defaultTextButtonHeight
                title: Tr.t("Ja", "Yes")
                radius: GlobalValues.defaultElementBorderRadius
                border.width: GlobalValues.defaultPressedFrameBorderWidth
                border.color: GlobalValues.defaultTextColor
                onClicked: {
                    autoMatchDialog.visible = false;
                    hardcover.confirmAutoMatch(autoMatchDialog.bookId, autoMatchDialog.edition.editionId);
                }
            }

            RoundedCornerTextButton {
                width: (parent.width - Global.dp(12)) / 2
                height: GlobalValues.defaultTextButtonHeight
                title: Tr.t("Nein", "No")
                radius: GlobalValues.defaultElementBorderRadius
                border.width: GlobalValues.defaultPressedFrameBorderWidth
                border.color: GlobalValues.defaultBorderColor
                onClicked: {
                    autoMatchDialog.visible = false;
                    bookList.model = [];
                    bookList.loading = true;
                    bookDialog.visible = true;
                    hardcover.searchBook(autoMatchDialog.bookId);
                }
            }
        }
    }

    // --- manual flow step 1: pick the book by title/author ---
    PanelDialog {
        id: bookDialog
        title: Tr.t("Buch wählen", "Choose book")

        StyledText {
            visible: bookList.loading
            width: parent.width
            styledFont: FontStyles.BodyM
            color: GlobalValues.defaultDisabledTextColor
            text: Tr.t("Suche…", "Searching…")
        }

        StyledText {
            visible: !bookList.loading && bookList.model.length === 0
            width: parent.width
            styledFont: FontStyles.BodyM
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: Tr.t("Keine Treffer gefunden.", "No matches found.")
        }

        Column {
            id: bookList
            property bool loading: false
            property var model: []
            width: parent.width
            spacing: Global.dp(4)

            Repeater {
                model: bookList.model

                Rectangle {
                    required property var modelData
                    width: bookList.width
                    height: Math.max(rowContent.height, Global.dp(56)) + Global.dp(16)
                    color: "transparent"

                    Row {
                        id: rowContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Global.dp(8)
                        anchors.rightMargin: Global.dp(8)
                        spacing: Global.dp(12)

                        Image {
                            source: modelData.coverUrl || ""
                            visible: (modelData.coverUrl || "") !== ""
                            width: Global.dp(40)
                            height: Global.dp(56)
                            fillMode: Image.PreserveAspectFit
                        }

                        Column {
                            width: parent.width - Global.dp(52)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Global.dp(2)

                            StyledText {
                                width: parent.width
                                styledFont: FontStyles.BodyS
                                color: GlobalValues.defaultTextColor
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                text: modelData.title || ""
                            }

                            StyledText {
                                width: parent.width
                                styledFont: FontStyles.BodyXS
                                color: GlobalValues.defaultDisabledTextColor
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                text: (modelData.author || "")
                                      + (modelData.releaseYear ? "  ·  " + modelData.releaseYear : "")
                                      + (modelData.pages ? "  ·  " + modelData.pages + Tr.t(" Seiten", " pages") : "")
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            bookDialog.visible = false;
                            editionList.model = [];
                            editionList.loading = true;
                            editionDialog.visible = true;
                            hardcover.pickBook(tab.book.bookId, modelData.hcBookId);
                        }
                    }
                }
            }
        }
    }

    // --- manual flow step 2: pick the edition ---
    PanelDialog {
        id: editionDialog
        title: Tr.t("Edition wählen", "Choose edition")

        StyledText {
            visible: editionList.loading
            width: parent.width
            styledFont: FontStyles.BodyM
            color: GlobalValues.defaultDisabledTextColor
            text: Tr.t("Lade Editionen…", "Loading editions…")
        }

        StyledText {
            visible: !editionList.loading && editionList.model.length === 0
            width: parent.width
            styledFont: FontStyles.BodyM
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: Tr.t("Keine Editionen gefunden.", "No editions found.")
        }

        Column {
            id: editionList
            property bool loading: false
            property var model: []
            width: parent.width
            spacing: Global.dp(4)

            Repeater {
                model: editionList.model

                Rectangle {
                    required property var modelData
                    width: editionList.width
                    height: Math.max(editionRow.height, Global.dp(40)) + Global.dp(16)
                    color: modelData.isCurrent ? GlobalValues.defaultBorderColor : "transparent"

                    Row {
                        id: editionRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Global.dp(8)
                        anchors.rightMargin: Global.dp(8)
                        spacing: Global.dp(10)

                        Image {
                            source: modelData.coverUrl || ""
                            visible: (modelData.coverUrl || "") !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            width: Global.dp(28)
                            height: Global.dp(40)
                            fillMode: Image.PreserveAspectFit
                        }

                        StyledText {
                            width: parent.width - Global.dp(38)
                            anchors.verticalCenter: parent.verticalCenter
                            styledFont: FontStyles.BodyXS
                            color: GlobalValues.defaultDisabledTextColor
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            text: [
                                hardcover.formatLabel(modelData.readingFormatId || 0),
                                modelData.isbn13 || modelData.isbn10 || "",
                                modelData.pages ? modelData.pages + Tr.t(" Seiten", " pages") : "",
                                modelData.releaseDate || "",
                                modelData.languageCode
                                    ? tab.languageFlag(modelData.languageCode) + " "
                                      + modelData.languageCode.toUpperCase()
                                    : "",
                                modelData.isCurrent ? Tr.t("Aktuell", "Current") : ""
                            ].filter(function(s) { return s !== ""; }).join("  ·  ")
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            editionDialog.visible = false;
                            hardcover.confirmEditionPick(tab.book.bookId, modelData.editionId);
                        }
                    }
                }
            }
        }
    }

    // --- native confirmation: mark as currently reading ---
    // Opaque backdrop below: ActionConfirmationDialog only dims what's
    // behind it rather than fully replacing it, which can let this tab's
    // own dense carousel content bleed through on e-ink.
    Rectangle {
        anchors.fill: parent
        visible: readingConfirmDialog.visible
        color: GlobalValues.defaultBackgroundColor
    }

    ActionConfirmationDialog {
        id: readingConfirmDialog
        anchors.fill: parent
        visible: false

        property var bookId: -1

        title: Tr.t("Als \u201eWird gelesen\u201c markieren?", "Mark as currently reading?")
        applyTitle: Tr.t("Ja", "Yes")
        cancelTitle: Tr.t("Nein", "No")

        onApply: {
            hardcover.confirmMarkReading(readingConfirmDialog.bookId);
            readingConfirmDialog.visible = false;
        }
        onCancel: readingConfirmDialog.visible = false
        onClose: readingConfirmDialog.visible = false
    }

    // --- native confirmation: mark as read (finished) ---
    Rectangle {
        anchors.fill: parent
        visible: finishConfirmDialog.visible
        color: GlobalValues.defaultBackgroundColor
    }

    ActionConfirmationDialog {
        id: finishConfirmDialog
        anchors.fill: parent
        visible: false

        property var bookId: -1

        title: Tr.t("Als gelesen markieren?", "Mark as read?")
        applyTitle: Tr.t("Ja", "Yes")
        cancelTitle: Tr.t("Nein", "No")

        onApply: {
            hardcover.confirmFinish(finishConfirmDialog.bookId);
            finishConfirmDialog.visible = false;
        }
        onCancel: {
            hardcover.declineFinish(finishConfirmDialog.bookId);
            finishConfirmDialog.visible = false;
        }
        onClose: finishConfirmDialog.visible = false
    }

    // native firmware toast, used for all status feedback
    InfoMessage {
        id: infoMessage
        anchors.fill: parent
        visible: false
        autohideInterval: 2500
        onClose: infoMessage.visible = false
    }
}
