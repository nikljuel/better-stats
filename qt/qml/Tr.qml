pragma Singleton
import QtQuick

/* Bilingual: German by default, English when the device language != de.
 * t(de, en) picks inline; month/weekday names as arrays. */
QtObject {
    readonly property bool de: (typeof deviceLang === "undefined")
                               || deviceLang === ""
                               || deviceLang.substring(0, 2) === "de"

    function t(deStr, enStr) {
        return de ? deStr : enStr;
    }

    readonly property var monthsShort: de
        ? ["Jan", "Feb", "Mär", "Apr", "Mai", "Jun",
           "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"]
        : ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

    readonly property var monthsFull: de
        ? ["Januar", "Februar", "März", "April", "Mai", "Juni", "Juli",
           "August", "September", "Oktober", "November", "Dezember"]
        : ["January", "February", "March", "April", "May", "June", "July",
           "August", "September", "October", "November", "December"]

    readonly property var weekdaysShort: de
        ? ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]
        : ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]

    // Reading time "1h 05m" / "12 min"
    function fmtHM(secs) {
        var s = secs || 0;
        var h = Math.floor(s / 3600);
        var m = Math.floor((s % 3600) / 60);
        if (h > 0)
            return h + "h " + (m < 10 ? "0" : "") + m + "m";
        return de ? (m + " Min") : (m + " min");
    }
}
