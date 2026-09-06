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

    Component.onCompleted: {
        releaseNotesDialog.message = stats.releaseNotes(deviceLang)
        releaseNotesDialog.visible = releaseNotesDialog.message !== ""
    }

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

    MouseArea {
        anchors.top: appHeader.top
        anchors.bottom: appHeader.bottom
        anchors.horizontalCenter: appHeader.horizontalCenter
        width: appHeader.width * 0.5
        z: appHeader.z + 1
        pressAndHoldInterval: 2000
        onPressAndHold: {
            sessionDialog.sessions = stats.todaySessions()
            sessionDialog.visible = true
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
                model: [Tr.t("Übersicht", "Overview", "Aperçu", "Resumen"),
                        Tr.t("Serie", "Streak", "Série", "Racha"),
                        Tr.t("Kalender", "Calendar", "Calendrier", "Calendario"),
                        Tr.t("Jahr", "Year", "Année", "Año")]

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
                if (sessionDialog.visible)
                    sessionDialog.dismiss()
                else if (releaseNotesDialog.visible)
                    releaseNotesDialog.dismiss()
                else
                    Qt.quit();
            }
        }

        OverviewTab {
            id: overviewTab
            anchors.fill: parent
            visible: tabBar.current === 0
        }

        StreakTab {
            id: streakTab
            anchors.fill: parent
            visible: tabBar.current === 1
        }

        CalendarTab {
            id: calendarTab
            anchors.fill: parent
            visible: tabBar.current === 2
        }

        YearTab {
            id: yearTab
            anchors.fill: parent
            visible: tabBar.current === 3
        }
    }

    Connections {
        target: stats

        function onActivated() {
            var tabs = [overviewTab, streakTab, calendarTab, yearTab]
            tabs[tabBar.current].refresh()
        }

        function onUpdateChanged() {
            if (stats.updateState === "available" && !settingsDialog.visible
                    && !releaseNotesDialog.visible)
                updateDialog.visible = true
        }
    }

    PanelDialog {
        id: sessionDialog

        property var sessions: []
        readonly property var columns: [
            { text: Tr.t("Start", "Start", "Début", "Inicio"), width: 0.13 },
            { text: Tr.t("Ende", "End", "Fin", "Fin"), width: 0.13 },
            { text: Tr.t("Dauer", "Duration", "Durée", "Duración"), width: 0.17 },
            { text: Tr.t("Seiten", "Pages", "Pages", "Páginas"), width: 0.20 },
            { text: Tr.t("Buch", "Book", "Livre", "Libro"), width: 0.37 }
        ]
        title: Tr.t("Sessions heute", "Today's sessions",
                    "Sessions aujourd'hui", "Sesiones de hoy")

        function fmtDuration(value) {
            var secs = Math.max(0, Number(value) || 0)
            var hours = Math.floor(secs / 3600)
            var minutes = Math.floor((secs % 3600) / 60)
            var seconds = Math.floor(secs % 60)
            if (hours > 0)
                return hours + "h " + (minutes < 10 ? "0" : "") + minutes
                       + "m " + (seconds < 10 ? "0" : "") + seconds + "s"
            if (minutes > 0)
                return minutes + "m " + (seconds < 10 ? "0" : "")
                       + seconds + "s"
            return seconds + "s"
        }

        Row {
            width: parent.width
            height: Global.dp(34)

            Repeater {
                model: sessionDialog.columns

                StyledText {
                    required property var modelData
                    width: parent.width * modelData.width
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    styledFont: FontStyles.BodySBold
                    color: GlobalValues.defaultTextColor
                    elide: Text.ElideRight
                    text: modelData.text
                }
            }
        }

        Rectangle {
            width: parent.width
            height: GlobalValues.defaultSolidSeparatorThickness
            color: GlobalValues.defaultBorderColor
        }

        StyledText {
            visible: sessionDialog.sessions.length === 0
            width: parent.width
            styledFont: FontStyles.Body
            color: GlobalValues.defaultDisabledTextColor
            text: Tr.t("Heute wurden keine Sessions gespeichert.",
                       "No sessions were stored today.",
                       "Aucune session enregistrée aujourd'hui.",
                       "No se guardaron sesiones hoy.")
        }

        Repeater {
            model: sessionDialog.sessions

            Item {
                id: sessionRow

                required property var modelData
                property var values: [
                    modelData.start,
                    modelData.end,
                    sessionDialog.fmtDuration(modelData.activeSecs),
                    modelData.pages,
                    modelData.title
                ]
                width: parent.width
                height: Global.dp(40)

                Row {
                    anchors.fill: parent

                    Repeater {
                        model: sessionDialog.columns

                        StyledText {
                            required property var modelData
                            required property int index
                            width: parent.width * modelData.width
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            styledFont: FontStyles.BodyXS
                            color: GlobalValues.defaultTextColor
                            elide: Text.ElideRight
                            text: sessionRow.values[index]
                        }
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: GlobalValues.defaultSolidSeparatorThickness
                    color: GlobalValues.defaultBorderColor
                }
            }
        }
    }

    PanelDialog {
        id: releaseNotesDialog

        property string message: ""
        title: Tr.t("Neu in Better Stats %1", "What's new in Better Stats %1",
                    "Nouveautés Better Stats %1", "Novedades Better Stats %1")
                 .arg(stats.currentVersion)

        onDismissed: {
            stats.dismissReleaseNotes()
            if (stats.updateState === "available" && !settingsDialog.visible)
                updateDialog.visible = true
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.Body
            color: GlobalValues.defaultTextColor
            wrapMode: Text.Wrap
            text: releaseNotesDialog.message
        }

        footer: Rectangle {
            width: parent.width
            height: Global.dp(48)
            color: GlobalValues.defaultTextColor

            StyledText {
                anchors.centerIn: parent
                styledFont: FontStyles.BodyLBold
                color: GlobalValues.defaultBackgroundColor
                text: Tr.t("Verstanden", "Got it", "Compris", "Entendido")
            }

            MouseArea {
                anchors.fill: parent
                onClicked: releaseNotesDialog.dismiss()
            }
        }
    }

    PanelDialog {
        id: updateDialog

        title: Tr.t("Update verfügbar", "Update available",
                    "Mise à jour disponible", "Actualización disponible")

        StyledText {
            width: parent.width
            styledFont: FontStyles.Body
            color: GlobalValues.defaultTextColor
            wrapMode: Text.Wrap
            text: Tr.t("Version %1 ist verfügbar.",
                       "Version %1 is available.",
                       "La version %1 est disponible.",
                       "La versión %1 está disponible.").arg(stats.latestVersion)
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
                    text: Tr.t("Jetzt installieren", "Install now",
                              "Installer maintenant", "Instalar ahora")
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
                    text: Tr.t("Später", "Later", "Plus tard", "Más tarde")
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: updateDialog.visible = false
                }
            }
        }
    }

    PanelDialog {
        id: restartDialog
        z: 11

        title: Tr.t("Neustart erforderlich", "Restart required",
                    "Redémarrage nécessaire", "Reinicio necesario")

        Row {
            width: parent.width
            spacing: Global.dp(16)

            Rectangle {
                width: Global.dp(44)
                height: width
                radius: width / 2
                color: "transparent"
                border.width: GlobalValues.dialogBorderWidth
                border.color: GlobalValues.defaultTextColor

                StyledText {
                    anchors.centerIn: parent
                    styledFont: FontStyles.Heading3
                    color: GlobalValues.defaultTextColor
                    text: "?"
                }
            }

            StyledText {
                width: parent.width - Global.dp(60)
                styledFont: FontStyles.Body
                color: GlobalValues.defaultTextColor
                wrapMode: Text.Wrap
                text: Tr.t(
                    "Autostart ist ausgeschaltet. Starte den Reader neu, damit Bücher direkt im Stock-Reader geöffnet werden und der G-Sensor wieder funktioniert.",
                    "Autostart is off. Restart the reader so books open directly in the stock reader and G-sensor rotation works again.",
                    "Le démarrage auto est désactivé. Redémarre le lecteur pour que les livres s'ouvrent dans le lecteur d'origine et que le capteur G fonctionne à nouveau.",
                    "El inicio automático está desactivado. Reinicia el lector para que los libros se abran en el lector de serie y el sensor G vuelva a funcionar.")
            }
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
                    text: Tr.t("Jetzt neu starten", "Restart now",
                              "Redémarrer", "Reiniciar")
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: stats.rebootDevice()
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
                    text: Tr.t("Später", "Later", "Plus tard", "Más tarde")
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: restartDialog.dismiss()
                }
            }
        }
    }

    PanelDialog {
        id: settingsDialog

        title: Tr.t("Einstellungen", "Settings", "Paramètres", "Ajustes")
        property var status: ({ enabled: false, available: false, message: "" })

        function refresh() {
            status = stats.autostartStatus()
        }

        function updateStatusText() {
            if (stats.updateState === "checking")
                return Tr.t("Suche nach Updates …", "Checking for updates …",
                            "Recherche de mises à jour …", "Buscando actualizaciones …")
            if (stats.updateState === "downloading")
                return Tr.t("Update wird geladen und geprüft …",
                            "Downloading and verifying update …",
                            "Téléchargement et vérification …",
                            "Descargando y verificando …")
            if (stats.updateState === "restarting")
                return Tr.t("Update installiert. Neustart …",
                            "Update installed. Restarting …",
                            "Mise à jour installée. Redémarrage …",
                            "Actualización instalada. Reiniciando …")
            if (stats.updateState === "available")
                return Tr.t("Update verfügbar: ", "Update available: ",
                            "Mise à jour disponible : ", "Actualización disponible: ")
                       + stats.latestVersion
            if (stats.updateState === "current")
                return Tr.t("Better Stats ist aktuell.", "Better Stats is up to date.",
                            "Better Stats est à jour.", "Better Stats está actualizado.")
            if (stats.updateState !== "error")
                return Tr.t("Noch nicht geprüft.", "Not checked yet.",
                            "Pas encore vérifié.", "No comprobado aún.")
            if (stats.updateError === -1)
                return Tr.t("Keine WLAN-Verbindung.", "No Wi-Fi connection.",
                            "Pas de connexion Wi-Fi.", "Sin conexión Wi-Fi.")
            if (stats.updateError === -2)
                return Tr.t("Download fehlgeschlagen.", "Download failed.",
                            "Échec du téléchargement.", "Descarga fallida.")
            if (stats.updateError === -3)
                return Tr.t("Release-Antwort ungültig.", "Invalid release response.",
                            "Réponse de version invalide.", "Respuesta de versión no válida.")
            if (stats.updateError === -4)
                return Tr.t("Kein passendes Update-Paket gefunden.",
                            "No matching update package found.",
                            "Aucun paquet de mise à jour correspondant trouvé.",
                            "No se encontró un paquete de actualización compatible.")
            if (stats.updateError === -5)
                return Tr.t("Update-Paket ist beschädigt.", "Update package is damaged.",
                            "Le paquet de mise à jour est endommagé.",
                            "El paquete de actualización está dañado.")
            if (stats.updateError === -7)
                return Tr.t("Diese Firmware unterstützt WLAN-Updates nicht.",
                            "This firmware does not support Wi-Fi updates.",
                            "Ce firmware ne prend pas en charge les mises à jour Wi-Fi.",
                            "Este firmware no admite actualizaciones por Wi-Fi.")
            return Tr.t("Update konnte nicht installiert werden.",
                        "The update could not be installed.",
                        "La mise à jour n'a pas pu être installée.",
                        "La actualización no se pudo instalar.")
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyLBold
            color: GlobalValues.defaultTextColor
            text: Tr.t("Tracking", "Tracking", "Suivi", "Seguimiento")
        }

        SettingsBitmapTextSwitcher {
            width: parent.width
            height: GlobalValues.defaultListItemHeight
            title: Tr.t("Autostart", "Autostart", "Démarrage auto", "Inicio automático")
            switch_value: settingsDialog.status.enabled === true
            enabled: settingsDialog.status.available === true
                     || settingsDialog.status.enabled === true
            opacity: enabled ? 1 : 0.45

            onAction: {
                var wanted = !settingsDialog.status.enabled
                settingsDialog.status = stats.setAutostartEnabled(wanted)
                if (settingsDialog.status.enabled === wanted && !wanted) {
                    restartDialog.visible = true
                }
            }
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: settingsDialog.status.message === "KOReader association detected"
                  || settingsDialog.status.message === "Another reader is registered"
                ? settingsDialog.status.enabled === true
                  ? Tr.t("Der andere Reader bleibt Standard. Der Better-Stats-Handler ist zusätzlich vor dem Stock-Reader installiert.",
                         "The other reader remains the default. The Better Stats handler is also installed before the stock reader.",
                         "L'autre lecteur reste par défaut. Le handler Better Stats est installé en plus avant le lecteur d'origine.",
                         "El otro lector sigue siendo el predeterminado. El handler de Better Stats está instalado antes del lector de serie.")
                  : Tr.t("Der andere Reader bleibt Standard. Beim Aktivieren bleibt seine Zuordnung unverändert.",
                         "The other reader remains the default. Enabling leaves its association unchanged.",
                         "L'autre lecteur reste par défaut. L'activation ne modifie pas son association.",
                         "El otro lector sigue siendo el predeterminado. Activar no cambia su asociación.")
                : settingsDialog.status.enabled === true
                  ? Tr.t("Tracking startet automatisch bei EPUB, FB2 und CBZ. Auf manchen Geräten kann dadurch die G-Sensor-Drehung im Reader ausfallen.",
                         "Tracking starts automatically for EPUB, FB2, and CBZ. On some devices this can disable G-sensor rotation in the reader.",
                         "Le suivi démarre automatiquement pour les EPUB, FB2 et CBZ. Sur certains appareils, la rotation du capteur G peut être désactivée.",
                         "El seguimiento se inicia automáticamente para EPUB, FB2 y CBZ. En algunos dispositivos esto puede desactivar la rotación del sensor G.")
                  : Tr.t("Öffne Better Stats einmal nach jedem Neustart, um Tracking zu starten. Bücher öffnen direkt im Stock-Reader und die G-Sensor-Drehung bleibt verfügbar.",
                         "Open Better Stats once after each restart to start tracking. Books open directly in the stock reader and G-sensor rotation remains available.",
                         "Ouvre Better Stats une fois après chaque redémarrage pour lancer le suivi. Les livres s'ouvrent dans le lecteur d'origine et le capteur G reste disponible.",
                         "Abre Better Stats una vez tras cada reinicio para iniciar el seguimiento. Los libros se abren en el lector de serie y el sensor G sigue disponible.")
        }

        StyledText {
            visible: settingsDialog.status.enabled !== true
                     && settingsDialog.status.message !== ""
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: settingsDialog.status.message === "KOReader association detected"
                ? Tr.t("KOReader ist eingetragen. Beim Aktivieren bleibt KOReader Standard.",
                       "KOReader is registered. Enabling keeps it as the default.",
                       "KOReader est enregistré. L'activation le garde par défaut.",
                       "KOReader está registrado. Activar lo mantiene como predeterminado.")
                : settingsDialog.status.message === "Another reader is registered"
                ? Tr.t("Ein anderer Reader ist eingetragen. Beim Aktivieren bleibt er Standard.",
                       "Another reader is registered. Enabling keeps it as the default.",
                       "Un autre lecteur est enregistré. L'activation le garde par défaut.",
                       "Otro lector está registrado. Activar lo mantiene como predeterminado.")
                : Tr.t("Grund: ", "Reason: ", "Raison : ", "Razón: ")
                  + settingsDialog.status.message
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
            text: Tr.t("Updates", "Updates", "Mises à jour", "Actualizaciones")
        }

        SettingsBitmapTextSwitcher {
            width: parent.width
            height: GlobalValues.defaultListItemHeight
            title: Tr.t("Automatisch nach Updates suchen",
                        "Check for updates automatically",
                        "Vérifier les mises à jour automatiquement",
                        "Buscar actualizaciones automáticamente")
            switch_value: stats.automaticUpdates
            onAction: stats.setAutomaticUpdates(!stats.automaticUpdates)
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            wrapMode: Text.Wrap
            text: Tr.t(
                "Prüft beim Start über bekanntes WLAN; Installation nach Bestätigung.",
                "Checks on launch over known Wi-Fi; installs after confirmation.",
                "Vérifie au lancement via le Wi-Fi connu ; installe après confirmation.",
                "Comprueba al iniciar por Wi-Fi conocida; instala tras confirmación.")
        }

        StyledText {
            width: parent.width
            styledFont: FontStyles.BodyS
            color: GlobalValues.defaultDisabledTextColor
            text: Tr.t("Installiert: ", "Installed: ", "Installé : ", "Instalado: ")
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
                text: Tr.t("Jetzt prüfen", "Check now",
                          "Vérifier", "Comprobar")
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
                text: Tr.t("Update installieren", "Install update",
                          "Installer la mise à jour", "Instalar actualización")
            }

            MouseArea {
                anchors.fill: parent
                onClicked: stats.installUpdate()
            }
        }
    }
}
