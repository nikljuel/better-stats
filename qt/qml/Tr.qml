pragma Singleton
import QtQuick

QtObject {
    // 0=de, 1=en, 2=fr, 3=es
    readonly property int lang: {
        if (typeof deviceLang === "undefined" || deviceLang === "") return 0;
        var p = deviceLang.substring(0, 2);
        if (p === "de") return 0;
        if (p === "fr") return 2;
        if (p === "es") return 3;
        return 1;
    }

    function t(de, en, fr, es) { return [de, en, fr, es][lang]; }

    readonly property var monthsShort: [
        ["Jan", "Feb", "Mär", "Apr", "Mai", "Jun",
         "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"],
        ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"],
        ["janv.", "fév.", "mars", "avr.", "mai", "juin",
         "juil.", "août", "sept.", "oct.", "nov.", "déc."],
        ["ene.", "feb.", "mar.", "abr.", "may.", "jun.",
         "jul.", "ago.", "sept.", "oct.", "nov.", "dic."],
    ][lang]

    readonly property var monthsFull: [
        ["Januar", "Februar", "März", "April", "Mai", "Juni", "Juli",
         "August", "September", "Oktober", "November", "Dezember"],
        ["January", "February", "March", "April", "May", "June", "July",
         "August", "September", "October", "November", "December"],
        ["janvier", "février", "mars", "avril", "mai", "juin", "juillet",
         "août", "septembre", "octobre", "novembre", "décembre"],
        ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
         "agosto", "septiembre", "octubre", "noviembre", "diciembre"],
    ][lang]

    readonly property var weekdaysShort: [
        ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"],
        ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"],
        ["lun.", "mar.", "mer.", "jeu.", "ven.", "sam.", "dim."],
        ["lun.", "mar.", "mié.", "jue.", "vie.", "sáb.", "dom."],
    ][lang]

    function fmtHM(secs) {
        var s = secs || 0;
        var h = Math.floor(s / 3600);
        var m = Math.floor((s % 3600) / 60);
        if (h > 0)
            return h + "h " + (m < 10 ? "0" : "") + m + "m";
        return lang === 0 ? (m + " Min") : (m + " min");
    }
}
