# Experimental KOReader support

This work lives only on the `koreader` branch and is not ready for a release.

## Implemented

The current change only covers coexistence with KOReader's PocketBook file
associations:

- A user `system/config/extensions.cfg` is treated as a partial override. For
  EPUB, FB2 and CBZ, Better Stats uses the user entry when present and otherwise
  inherits the firmware entry from `/ebrmain/config/extensions.cfg`.
- Existing comments, unrelated entries and reader order are preserved. The
  original user configuration is backed up before it is changed.
- `betterstats-handler.app` is placed immediately before the first stock
  `eink-reader*.app`. A preceding KOReader or other third-party reader therefore
  remains the default.
- When selected through "Open with", the handler only searches entries after
  itself before falling back to the stock reader. This avoids opening KOReader
  again through the Better Stats entry.
- Autostart status and both UIs describe this mixed-reader setup instead of
  disabling the setting whenever KOReader is present.
- Malformed user entries fail visibly instead of silently falling back to the
  firmware configuration.

Automated coverage includes KOReader marker-only files (including the reported
`#koreader\n#betterstats` file without a trailing newline), partial overrides,
real KOReader associations, third-party readers, repeated preparation,
disabling, and malformed entries. `make test`, `make qt`, and `make hardfp`
pass on the development machine.

## Not implemented

- No read-only import from KOReader's `statistics.sqlite3`.
- No import consent, settings switch, combined statistics, or removal of an
  imported Better Stats copy.
- No daemon pause/resume based on `/tmp/koreader.pid`.
- No live KOReader progress, close, or completion bridge/plugin.
- No writes to KOReader data or PocketBook's `explorer-3.db` are planned.

## Risks and required device testing

This has not yet been validated on a PocketBook Era Lite. Do not advertise or
release it until these checks pass:

1. Install KOReader before and after Better Stats.
2. Change KOReader file associations after both installation orders.
3. Confirm that a normal tap opens the configured default reader.
4. Confirm that "Open with -> Better Stats" opens the stock reader and tracks
   it without looping back to KOReader.
5. Check restart caching, display names, and G-sensor rotation.

KOReader may rewrite the user `extensions.cfg` when its associations change and
remove the Better Stats hook. The intended recovery is that the next Better
Stats launch repairs only its own entry; this still needs the device test.
Also, when KOReader precedes the handler, a normal tap starts KOReader directly,
so the Better Stats handler and daemon do not run for that opening.

## Later phases

Only after the coexistence behavior is proven on-device should historical
statistics be imported read-only from
`/mnt/ext1/applications/koreader/settings/statistics.sqlite3`. Existing signals
such as `/tmp/.current` should be reused where sufficient. An optional KOReader
plugin is only justified for live progress, close, and completion events that
cannot be obtained safely otherwise; it must never modify `explorer-3.db`.
