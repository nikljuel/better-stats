# Better Stats

Reading statistics for stock PocketBook e-readers — no KOReader, no account, no
cloud. Better Stats reads what the firmware already records and turns it into the
kind of stats screen you'd expect from Kobo or Fable.

> Built and tested on a **PocketBook Era Lite (PB710), firmware 6.x**. The bundle
> also includes a Qt-free InkView UI for soft-float PocketBooks and a separate
> hard-float build for the PB1030/RK3566 family. Device testing beyond PB710 is
> still welcome.

| Overview | Streak | Calendar | Year |
|:---:|:---:|:---:|:---:|
| ![Overview](docs/overview.png) | ![Streak](docs/streak.png) | ![Calendar](docs/calendar.png) | ![Year](docs/year.png) |

## Features

- **Four tabs, no scrolling** (e-ink scrolling is fiddly): Overview · Streak ·
  Calendar · Year.
- **Overview** — current book with cover, progress and estimated time left;
  tiles for time read today, avg minutes per session and pages per minute; a
  "books finished" donut.
- **Streak** — current and best streak, a GitHub-style year heatmap (read days
  and finished-book markers), plus a longest-streak insight line.
- **Calendar** — a month grid showing the cover of the most-read book per day;
  tap a day for a breakdown of books and time.
- **Year** — books finished per month with mini covers; tap a month for the
  finish dates.
- **Automatic compatibility fallback** — the launcher tries the richer Qt UI on
  compatible soft-float firmware and switches to a Qt-free InkView UI if Qt does
  not reach its ready state. Hard-float devices go directly to InkView.
- **Native look** — Qt uses the firmware's `com.pocketbook.controls`; the fallback
  draws with InkView fonts, dialogs and screen updates.
- **Real covers** — extracted straight from EPUB and FB2 files, or from the
  first image in a CBZ (the firmware's cover cache is sometimes wrong for
  sideloaded/Calibre books).
- **Bilingual** — German by default, English automatically when the device
  language isn't German.
- **Automatic tracking** — starts before the stock reader opens a book,
  including when the firmware restores the last book after a reboot; it can be
  switched off on devices where the handler interferes with G-sensor rotation.
- **Updates over Wi-Fi** — installs stable GitHub releases without another USB
  copy; automatic updates are enabled by default and can be switched off.

## How it works

A small background daemon watches the firmware's
library database (`explorer-3.db`) **read-only** via inotify (falling back to a
30-second poll where inotify is unavailable) and derives reading sessions from
the book's open time and last position update. Idle gaps (standby, long pauses)
are capped so they don't count as reading time. If the daemon wasn't running,
Better Stats reconstructs a best-effort estimate of the last session per book on
the next launch. The firmware does not retain enough timestamps for an exact
reconstruction, so short sessions or pauses can still be missed.

Autostart installs a small EPUB/FB2/CBZ file handler: a shell script that
backgrounds the daemon and then `exec`s the stock reader, so it *becomes* the
reader in the same process and task slot the firmware already created. If the
daemon fails to start for any reason the `exec` still runs, so the tracking hook
can never keep a book from opening. PDF and other file types are unchanged.
Existing reader order stays intact. Better Stats is added directly before the
stock reader, so a preceding KOReader association remains the default. A
partial user `extensions.cfg` inherits missing EPUB/FB2/CBZ entries from the
firmware file; comments and unrelated associations are preserved.
On some PocketBook firmware the handler prevents G-sensor rotation inside the
reader. Turning Autostart off in Settings removes the handler; open Better Stats
once after each reboot to start tracking manually while keeping rotation. The
app offers to restart the reader after turning Autostart off because the
firmware caches file-handler assignments. Turning it on needs no immediate
restart: the open app already started the daemon, and the handler takes over
after the next normal reboot.

Note that the `eink-reader_with_<engine>.app` names in `extensions.cfg` are
virtual — only `/ebrmain/bin/eink-reader.app` exists on disk — so the handler
resolves the name to that binary and passes no engine flag.

"Finished" books come from the firmware's own *mark as read* flag, so they match
what you see in the Library.

## Privacy

Reading data stays on the device. There is no account and no telemetry. Network
access is only used to check and download releases from this GitHub repository:
automatically at app launch when enabled by silently connecting to a configured
Wi-Fi network, or when **Check now** is selected in Settings. Better Stats only
**reads** the firmware database. It writes its own stats database and cover cache under
`system/pbreadstats/`. While Autostart is enabled, it also creates a marked book
handler under `system/bin/` and updates the user `extensions.cfg` (with a backup
before the first change).

## Install

1. Download the `.zip` from the [latest release](../../releases/latest).
2. Connect your reader via USB and extract the ZIP into the reader's top-level
   directory. It installs `applications/BetterStats.app` plus the versioned
   binaries below `applications/betterstats/`.
3. Eject the reader and open **BetterStats** once.

This USB install is only required once. Later stable releases can be installed
from **Settings → Check now**. Automatic update checks are on by default; at app
launch they silently connect to a configured Wi-Fi network and ask before
installing a new release. Choosing **Later** stores nothing, so the next launch
checks again for the newest release. Confirmed updates use the GitHub SHA-256
digest when available and always validate the bundled checksums before atomic
activation and restarting Better Stats. PocketBook
controls when Wi-Fi disconnects again. Switching automatic checks off does not
disable the manual Wi-Fi update button; a manual check can show the connection
dialog when needed.

The dispatcher detects soft-float versus hard-float from the firmware loader.
On soft-float devices it starts Qt first and falls back to InkView only if Qt
cannot create its root view. Startup decisions and errors are written to
`system/pbreadstats/app.log` (rotated at 256 KiB).

On first launch Better Stats also sets up EPUB/FB2/CBZ autostart, because the
daemon otherwise does not start automatically after a reboot. It writes a
marked handler to `system/bin/` and puts it ahead of the stock reader in the user
`extensions.cfg`, backing that file up first
(`extensions.cfg.betterstats-backup`). PDF and other file types are untouched.
If KOReader is already associated with one of these formats, it remains the
default and Better Stats is inserted directly before the stock reader. Autostart
is enabled by default and can be disabled persistently in Settings. No immediate
reboot is needed after installation or after enabling it: opening Better Stats
has already started the daemon, and the handler is ready for the next normal
reboot. Turning Autostart off shows a restart prompt so the firmware drops its
cached handler.

When the Qt UI starts, it registers a custom launcher icon by adding one entry
to `system/config/desktop/view.json` and saving a backup next to it
(`view.json.betterstats-backup`). The icon is refreshed automatically on each
launch when a new version ships an updated image. It appears after the reader
rescans its apps (reboot if needed). InkView-only devices keep the default
user-app icon.

For a clean uninstall, first turn Autostart off in Settings and choose
**Restart now**. Then delete `applications/BetterStats.app` and
`applications/betterstats/`. The toggle removes Better Stats from
`system/config/extensions.cfg` without overwriting other reader associations and
deletes `system/bin/betterstats-handler.app`. If the Qt UI registered its custom
icon, remove the `U_betterstats` entry from `system/config/desktop/view.json` and
the two `applications/icons/betterstats*.bmp` files. The two
`*.betterstats-backup` files can then be deleted; restore one only if you are sure
the corresponding firmware configuration has not gained unrelated changes
since installation. Reading statistics and settings remain in
`system/pbreadstats/` until that folder is deleted.

## Building

See [BUILDING.md](BUILDING.md). In short: `make sdk` once, then `make package`
builds Qt/InkView soft-float, InkView hard-float and the installable ZIP.
`make test` runs the host-side logic and launcher tests.

## Credits

- Cross-compilation and the on-device Qt/InkView bridge build on
  [fstanis/pocketbook-sdk-qt6](https://github.com/fstanis/pocketbook-sdk-qt6).
- Bundles [SQLite](https://www.sqlite.org/) (public domain) and
  [miniz](https://github.com/richgel999/miniz) (MIT) as source.

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

Not affiliated with PocketBook. Use at your own risk. It only reads the firmware
database. Its optional configuration edits are backed up, but you are
installing third-party software on your device.
