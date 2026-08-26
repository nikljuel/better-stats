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
- **Real covers** — extracted straight from your EPUBs (the firmware's cover
  cache is sometimes wrong for sideloaded/Calibre books).
- **Bilingual** — German by default, English automatically when the device
  language isn't German.
- **Automatic tracking** — starts before the stock reader opens a book,
  including when the firmware restores the last book after a reboot.

## How it works

A small background daemon polls the firmware's
library database (`explorer-3.db`) **read-only** every 30 seconds and derives
reading sessions from the book's open time and last position update. Idle gaps
(standby, long pauses) are capped so they don't count as reading time. If the
daemon wasn't running, Better Stats reconstructs a best-effort estimate of the
last session per book on the next launch. The firmware does not retain enough
timestamps for an exact reconstruction, so short sessions or pauses can still
be missed.

Autostart installs a small EPUB file handler: a shell script that backgrounds the daemon and then `exec`s
the stock reader, so it *becomes* the reader in the same process and task slot
the firmware already created. If the daemon fails to start for any reason the
`exec` still runs, so the tracking hook can never keep a book from opening. PDF
and other file types are unchanged. KOReader associations are detected but
deliberately left untouched for now.

Note that the `eink-reader_with_<engine>.app` names in `extensions.cfg` are
virtual — only `/ebrmain/bin/eink-reader.app` exists on disk — so the handler
resolves the name to that binary and passes no engine flag.

"Finished" books come from the firmware's own *mark as read* flag, so they match
what you see in the Library.

## Privacy

Everything stays on the device. No network access, no account, no telemetry.
Better Stats only **reads** the firmware database. It writes its own stats
database and cover cache under `system/pbreadstats/`; if Autostart is enabled,
it also creates a marked EPUB handler under `system/bin/` and updates the user
`extensions.cfg` (with a backup created before the first change).

## Install

1. Download the `.zip` from the [latest release](../../releases/latest).
2. Connect your reader via USB and extract the ZIP into the reader's top-level
   directory. It installs `applications/BetterStats.app` plus the versioned
   binaries below `applications/betterstats/`.
3. Eject the reader and open **BetterStats** once.

The dispatcher detects soft-float versus hard-float from the firmware loader.
On soft-float devices it starts Qt first and falls back to InkView only if Qt
cannot create its root view. Startup decisions and errors are written to
`system/pbreadstats/app.log` (rotated at 256 KiB).

On first launch Better Stats also sets up EPUB autostart, because the daemon is
otherwise only running while the app itself is open. It writes a marked handler
to `system/bin/` and puts it ahead of the stock reader in the user
`extensions.cfg`, backing that file up first
(`extensions.cfg.betterstats-backup`). PDF and other file types are untouched,
and any reader that already holds the first slot for EPUBs -- KOReader or
anything else -- is left alone, so it keeps opening your books. Reboot
the reader once for the change to take effect.

When the Qt UI starts, it installs its own launcher icon by adding one entry to
`system/config/desktop/view.json` and saving a backup next to it
(`view.json.betterstats-backup`). The custom icon appears after the reader
rescans its apps (reboot if needed). InkView-only devices keep the default
user-app icon.

To uninstall, delete `applications/BetterStats.app` and
`applications/betterstats/`. Books keep opening either way — the handler skips a
missing app and execs the stock reader regardless — but
to remove every trace, also delete `system/bin/betterstats-handler.app`, restore
`system/config/extensions.cfg` from `extensions.cfg.betterstats-backup`, and
restore `view.json` from its backup (or drop the `U_betterstats` entry). Your
statistics live in `system/pbreadstats/` and are only removed if you delete that
folder.

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
