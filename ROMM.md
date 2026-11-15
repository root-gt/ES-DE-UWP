# ES-DE (EmulationStation Desktop Edition) - RomM integration

Native C++ integration with a [RomM](https://romm.app) server. It is a port of the
[RomM-ES](https://github.com/WirlyWirly/RomM-ES) "plugin" that doesn't need Python, `curl`,
`7z.exe` or custom event scripts. That is required for sandboxed builds such as the
**UWP / Xbox Dev Mode** port, where an application can't start other processes.

The integration does two things:

1. **Library import** (`Main menu > Utilities > RomM > Import games from RomM`). This works like `GameImporter.py`:
   - For every game in the selected RomM platforms, it creates a tiny placeholder file in the
     ROM directory of the matching ES-DE system.
   - It adds a `gamelist.xml` entry with the name, description, release date, rating, genre,
     developer, publisher and players.
   - It downloads the game media into `downloaded_media`: covers, screenshots, marquees,
     3D boxes, back covers, fanart, miximages, physical media, title screens, manuals and
     optionally videos.
2. **Download on launch.** This works like `GameStart.py`. When you launch a placeholder, ES-DE
   downloads the real game file from RomM and writes it straight into the ROM directory, in
   place of the placeholder. A progress dialog with a cancel button is shown during the
   download. The emulator then starts as normal. After that the game is a regular local file.

Placeholders use the same format as RomM-ES (`RomM-ES:<romId>` or
`RomM-ES:<romId>:<archiveExtension>`). Placeholders created by the Python scripts on a PC can
therefore be copied to the Xbox drive and used as they are.

## Settings (`Utilities > RomM`)

| Setting | es_settings.xml key | Description |
|---|---|---|
| Enable RomM integration | `RomMEnabled` | Master switch. Downloads on launch only happen when this is on. |
| Server URL | `RomMServerURL` | For example `http://192.168.1.10:8080`. If you leave out `http://`, it is added. |
| API token | `RomMAPIToken` | RomM client API token (sent as `Authorization: Bearer`). |
| Username / Password | `RomMUsername`, `RomMPassword` | HTTP Basic authentication. Only used when no API token is set. |
| Download games on launch | `RomMDownloadOnLaunch` | Downloads placeholders when they are launched. |
| Import game media | `RomMImportMedia` | Downloads images and manuals during the import. |
| Import videos | `RomMImportVideos` | Also downloads videos. Off by default because videos are large. |
| Delete archives after extraction | `RomMDeleteArchiveAfterExtraction` | Removes the downloaded ZIP file after extraction. |
| Extract archives | `RomMExtractedExtensions` | Comma-separated `system=extension` pairs, for example `ps2=iso,gc=rvz`. For these systems, ZIP files from RomM are extracted, and the placeholder gets the extension of the extracted file. |
| Platform mappings | `RomMPlatformMappings` | Extra or overriding `romm-slug=es-de-system` pairs, for example `neogeoaes=neogeo`. |
| *(automatic)* | `RomMImportPlatforms` | The platforms selected in the import dialog. |

Platform mapping order:
1. The user mappings in `RomMPlatformMappings`, matched on the RomM `fs_slug` or `slug`.
2. The RomM directory name (`fs_slug`), if it is already a valid ES-DE system name.
3. The built-in table of RomM/IGDB slugs (see `sPlatformMap` in `es-app/src/RomM.cpp`).

## Notes

- **Multi-file ROMs** (for example multi-disc games stored as a folder in RomM) are served by
  RomM as one ZIP file. They are imported as `<name>.zip`.
- **Only ZIP files can be extracted** (stored and deflate compression, including ZIP64).
  This needs zlib, see the build notes below. 7z and RAR are not supported.
- **Import safety:**
  - The importer never overwrites a real game file. It only creates placeholders, or updates
    them when the RomM ID has changed.
  - It only adds `gamelist.xml` entries that don't exist yet, so your edits in ES-DE are kept.
  - Before the import starts, pending gamelist changes are written. After the import, the ROM
    directory is rescanned.
- **Stale IDs:** before downloading, ES-DE checks that the RomM ID still points to the same
  file name. If a game was removed and rescanned on the server, you are told to run the
  importer again. No wrong game is downloaded.
- **Partial downloads:** a download is written to `<file>.romm-part` and only moved over the
  placeholder after it completes. A cancelled or failed download leaves the placeholder as
  it was.

## RomM API endpoints used

- `GET /api/heartbeat`: version, used by the connection test.
- `GET /api/platforms`
- `GET /api/roms?platform_ids=<id>&limit=250&offset=<n>&...`: paginated.
- `GET /api/roms/<id>`
- `GET /api/roms/<id>/content/<fs_name>`: the game file. Multi-file ROMs are returned as a ZIP file.
- `GET /assets/romm/resources/...`: media.

## Source files

| File | Purpose |
|---|---|
| `es-app/src/RomM.h/.cpp` | RomM client (libcurl), placeholders, importer, downloader, ZIP extraction |
| `es-app/src/guis/GuiRomM.h/.cpp` | Settings menu, platform selection, import and download flows |
| `es-app/src/guis/GuiRomMProgress.h/.cpp` | Threaded modal progress dialog with cancel support |
| `es-app/src/views/ViewController.cpp` | Launch hook: if the game is a placeholder, it is downloaded first |
| `es-app/src/guis/GuiMenu.cpp` | `Utilities > RomM` menu entry |
| `es-core/src/Settings.cpp` | Default values for the settings |
| `locale/po/pt_BR.po` | Brazilian Portuguese translations |

## Building for UWP / Xbox Dev Mode

The code only uses APIs that the UWP port of ES-DE 3.4.1 already uses:

- **libcurl:** with `USE_BUNDLED_CERTIFICATES` for HTTPS, just like `HttpReq`.
- **File I/O:** `_wfopen`, `std::filesystem::rename`, with `_wremove` + `_wrename` as a fallback.
- **pugixml and rapidjson.**

It doesn't use process creation, `Windows::System::Launcher` or any other WinRT API. The
existing UWP launch path (`retroarch:?cmd=...` protocol activation) runs unchanged once the
download has finished.

To add it to a UWP port tree based on ES-DE 3.4.1:

1. Apply the patch:
   - From git: `git cherry-pick` the RomM commit, or use `git am` with the patch file.
   - It applies cleanly on the `v3.4.1` tag. It was also tested on `master` (4.0 alpha).
2. Add the six new source files to the UWP project (or to `es-app/CMakeLists.txt`, which
   this patch already updates).
3. **ZIP extraction (optional):**
   - Define `ROMM_ZIP_SUPPORT` and link zlib. The UWP package already ships `z.dll`.
   - With CMake on Windows, put `zlib.lib` (and `zlib.h`/`zconf.h` on the include path) in the
     project root, and it is picked up automatically.
   - Without zlib, everything works except the `Extract archives` setting.
4. Capabilities: the existing `internetClient`, `privateNetworkClientServer` and
   `broadFileSystemAccess` capabilities already cover HTTP access to a LAN RomM server and
   writing to the ROM drive.

## Testing

On Linux, this was tested against a mock RomM server with Xvfb:

- **Connection test:** authentication works, and URLs without a scheme or with a trailing
  slash are accepted.
- **Platform mapping:** by `slug`, by `fs_slug`, and unknown platforms are skipped.
- **Import:**
  - Pagination with more than 250 games.
  - Gamelist metadata (including the `&` character and CRLF in descriptions) and media.
  - A second run is idempotent.
- **Download on launch:** the emulator receives the real file.
- **ZIP extraction:** deflate, the result is byte-identical and the archive is deleted.
- **Errors:** a stale or non-existent RomM ID shows an error, and neither the placeholder nor
  the emulator is touched.
- **Progress dialog and cancel:** no partial files are left behind.
