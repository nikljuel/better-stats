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

    // --- Hardcover sync integration, ported from a downstream fork - see
    // its own PR description for the full feature. ---

    // Checked once per app launch, not on a timer - a QTimer inside this
    // GUI process almost certainly doesn't fire while backgrounded, which
    // is most of the time this app would be "open." daemon.c (a separate,
    // continuously-running process) does the actual completion detection
    // in the background; this just checks what it found.
    Component.onCompleted: hardcover.checkPendingFinishConfirm()

    Connections {
        target: hardcover

        function onAutoSyncNeedsFinishConfirm(bookId, finishedAtDate, title) {
            autoSyncFinishDialog.bookId = bookId;
            autoSyncFinishDialog.title = Tr.t("Automatisch erkannt", "Automatically detected");
            autoSyncFinishDialog.message = Tr.t(
                "\u201e" + title + "\u201c als gelesen markieren, mit Enddatum: "
                    + Tr.friendlyDate(finishedAtDate) + "?",
                "Mark \u201c" + title + "\u201d as Read, with finish date: "
                    + Tr.friendlyDate(finishedAtDate) + "?");
            autoSyncFinishDialog.visible = true;
        }

        // Hardcover already has this book marked Read, with the same date
        // we were about to ask about - purely informational, so both
        // buttons on this dialog do the same thing (acknowledge and clear
        // the flag), rather than introducing a single-button dialog
        // pattern just for this one case.
        function onAutoSyncAlreadyFinished(bookId, finishedAtDate, title) {
            alreadyFinishedDialog.bookId = bookId;
            alreadyFinishedDialog.finishedAt = finishedAtDate;
            // InfoMessage has no separate title property - just message,
            // so "Already in sync" folds into the message text itself.
            alreadyFinishedDialog.message = Tr.t(
                "Bereits synchronisiert: \u201e" + title + "\u201c wurde bei Hardcover bereits als gelesen markiert, mit Enddatum: "
                    + Tr.friendlyDate(finishedAtDate) + ".",
                "Already in sync: \u201c" + title + "\u201d was already marked as finished on Hardcover, with date: "
                    + Tr.friendlyDate(finishedAtDate) + ".");
            alreadyFinishedDialog.visible = true;
        }

        // Hardcover has this book marked Read, but with a different finish
        // date than what we have locally (e.g. edited by hand on the
        // Hardcover site itself) - offers to update Hardcover's own date
        // to match ours. Reuses confirmFinish/declineFinish underneath,
        // same as the normal finish-confirm flow: the actual API call
        // (push status=Read with our finishedAt) is identical either way,
        // only the prompt differs.
        function onAutoSyncNeedsDateUpdate(bookId, existingDate, correctDate, title) {
            dateUpdateDialog.bookId = bookId;
            // The existing, unchanged Hardcover date - stored for the "No"
            // path, which needs to record what's actually still true on
            // Hardcover (nothing pushed), not the value we were proposing.
            dateUpdateDialog.existingDate = existingDate;
            dateUpdateDialog.title = Tr.t("Enddatum abweichend", "Finish date differs");
            dateUpdateDialog.message = Tr.t(
                "\u201e" + title + "\u201c ist bei Hardcover als gelesen markiert, mit Enddatum "
                    + Tr.friendlyDate(existingDate) + ". Auf "
                    + Tr.friendlyDate(correctDate) + " aktualisieren?",
                "\u201c" + title + "\u201d is marked as finished on Hardcover, with date "
                    + Tr.friendlyDate(existingDate) + ". Update it to "
                    + Tr.friendlyDate(correctDate) + "?");
            dateUpdateDialog.visible = true;
        }
    }

    // App-root version of the same finish-confirmation OverviewTab.qml
    // shows for a manual "Sync progress" press - this one can appear
    // regardless of which tab is currently open, since background sync
    // isn't tied to viewing any particular book.
    //
    // z: 1000 is required: the tab bar and tab content below are both
    // declared later in this file, and QML's default "later siblings
    // paint on top" rule means they would otherwise paint OVER this
    // dialog no matter how opaque it claims to be, regardless of
    // anchors.fill: parent. Confirmed as a real, on-device bug in the
    // fork this was ported from - without an explicit z here, the
    // dialog's own border never actually covers the full window, with
    // whichever tab is open still visible around and through it.
    ActionConfirmationDialog {
        id: autoSyncFinishDialog
        z: 1000
        anchors.fill: parent
        visible: false

        property var bookId: -1

        applyTitle: Tr.t("Ja", "Yes")
        cancelTitle: Tr.t("Nein", "No")

        onApply: {
            hardcover.confirmFinish(autoSyncFinishDialog.bookId);
            autoSyncFinishDialog.visible = false;
        }
        onCancel: {
            hardcover.declineFinish(autoSyncFinishDialog.bookId);
            autoSyncFinishDialog.visible = false;
        }
        onClose: autoSyncFinishDialog.visible = false
    }

    // Purely informational - acknowledges and clears the flag once shown,
    // nothing to actually decide, so this is InfoMessage (dismisses itself
    // after autohideInterval, or immediately on a tap anywhere else), not
    // ActionConfirmationDialog (which always renders two buttons). Same z
    // reasoning as autoSyncFinishDialog above.
    InfoMessage {
        id: alreadyFinishedDialog
        z: 1000
        anchors.fill: parent
        visible: false
        autohideInterval: 3000
        icon: InfoMessage.InformationIcon

        property var bookId: -1
        property string finishedAt: ""

        onClose: {
            hardcover.acknowledgeAlreadyFinished(alreadyFinishedDialog.bookId,
                                                  alreadyFinishedDialog.finishedAt);
            alreadyFinishedDialog.visible = false;
        }
    }

    // Reuses confirmFinish/declineFinish - see onAutoSyncNeedsDateUpdate's
    // own comment above for why. Same z reasoning as autoSyncFinishDialog
    // above.
    ActionConfirmationDialog {
        id: dateUpdateDialog
        z: 1000
        anchors.fill: parent
        visible: false

        property var bookId: -1
        property string existingDate: ""

        applyTitle: Tr.t("Ja", "Yes")
        cancelTitle: Tr.t("Nein", "No")

        onApply: {
            // Not confirmFinish() - that would redundantly re-run the
            // whole "check Hardcover's status first" logic, which already
            // ran once to get us this exact prompt. confirmDateUpdate()
            // just pushes directly, since the answer is already known.
            hardcover.confirmDateUpdate(dateUpdateDialog.bookId);
            dateUpdateDialog.visible = false;
        }
        onCancel: {
            // Not declineFinish() - that pushes status=Reading, which
            // would incorrectly revert a book Hardcover already has
            // correctly marked Read, just because the date update itself
            // was declined. Only the flag needs clearing; Hardcover's own,
            // already-correct Read status shouldn't be touched at all -
            // recorded as existingDate (what's actually still there), not
            // the date we'd proposed and the user just declined.
            hardcover.acknowledgeAlreadyFinished(dateUpdateDialog.bookId,
                                                  dateUpdateDialog.existingDate);
            dateUpdateDialog.visible = false;
        }
        onClose: dateUpdateDialog.visible = false
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
