//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  RomM.h
//
//  Integration with a RomM server (https://romm.app).
//  Native C++ port of the RomM-ES "plugin" (https://github.com/WirlyWirly/RomM-ES) that
//  does not depend on Python, curl.exe or 7z.exe, so it also works in sandboxed builds
//  such as the UWP/Xbox Dev Mode port where spawning external processes is not possible.
//
//  - The importer creates tiny placeholder files ("RomM-ES:<romId>[:<archiveExt>]") in the
//    ROM directory together with gamelist.xml entries and media files.
//  - When a placeholder is launched, the actual game file is downloaded from RomM and
//    written in place of the placeholder before the emulator is started.
//
//  The placeholder format is identical to the one used by RomM-ES, so placeholders
//  created by the Python scripts on a PC can be used as-is.
//

#ifndef ES_APP_ROMM_H
#define ES_APP_ROMM_H

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace RomM
{
    // Maximum size of a placeholder file in bytes.
    constexpr long PLACEHOLDER_MAX_SIZE {64};

    struct Placeholder {
        int romId {0};
        // If set, the RomM file is an archive with this extension (e.g. "zip") that needs
        // to be extracted, and the placeholder has the extension of the extracted file.
        std::string archiveExtension;
    };

    struct Platform {
        int id {0};
        std::string slug;
        std::string fsSlug;
        std::string name;
        int romCount {0};
        // Target ES-DE system, empty if the platform could not be mapped.
        std::string esdeSystem;
    };

    struct ImportStats {
        int imported {0};
        int updated {0};
        int skipped {0};
        int failed {0};
        int mediaFiles {0};
    };

    // Shared state between a worker thread and the GUI. All strings are protected by mMutex.
    class Progress
    {
    public:
        void setStatus(const std::string& status)
        {
            std::unique_lock<std::mutex> lock {mMutex};
            mStatus = status;
        }
        void setDetail(const std::string& detail)
        {
            std::unique_lock<std::mutex> lock {mMutex};
            mDetail = detail;
        }
        std::string getStatus()
        {
            std::unique_lock<std::mutex> lock {mMutex};
            return mStatus;
        }
        std::string getDetail()
        {
            std::unique_lock<std::mutex> lock {mMutex};
            return mDetail;
        }

        std::atomic<long long> bytesNow {0};
        std::atomic<long long> bytesTotal {0};
        std::atomic<int> itemsNow {0};
        std::atomic<int> itemsTotal {0};
        std::atomic<bool> cancel {false};

    private:
        std::mutex mMutex;
        std::string mStatus;
        std::string mDetail;
    };

    // Settings helpers.
    bool isConfigured();
    bool downloadOnLaunchEnabled();

    // Returns the placeholder content if the file is a RomM placeholder.
    std::optional<Placeholder> readPlaceholder(const std::string& path);
    bool writePlaceholder(const std::string& path, const Placeholder& placeholder);
    std::string placeholderText(const Placeholder& placeholder);

    // Map a RomM platform to an ES-DE system name. Returns an empty string if not possible.
    std::string mapPlatform(const std::string& slug, const std::string& fsSlug);

    // Parses "system=ext,system=ext" from the RomMExtractedExtensions setting.
    std::map<std::string, std::string> getExtractedExtensions();

    // Parses the comma-separated RomMImportPlatforms setting.
    std::vector<std::string> getSelectedPlatforms();

    class Client
    {
    public:
        // Reads the server URL and credentials from the settings.
        Client();

        const std::string& getServerURL() const { return mServerURL; }

        // GET request, the response body is returned in response.
        bool get(const std::string& pathOrURL,
                 std::string& response,
                 std::string& error,
                 long* httpStatus = nullptr);

        // Streams a file to disk. Data is first written to a temporary file which is
        // renamed to destinationPath on success.
        bool download(const std::string& pathOrURL,
                      const std::string& destinationPath,
                      Progress* progress,
                      std::string& error);

        // Verifies the server URL and credentials.
        bool testConnection(std::string& message);

        bool getPlatforms(std::vector<Platform>& platforms, std::string& error);

        // Imports all games for the supplied platforms. Placeholders, gamelist.xml entries
        // and media files are created. ES-DE must rescan the ROM directory afterwards.
        bool importPlatforms(const std::vector<Platform>& platforms,
                             Progress& progress,
                             ImportStats& stats,
                             std::string& error);

        // Downloads the game for a placeholder file and replaces the placeholder with it.
        bool downloadGame(const std::string& placeholderPath,
                          const std::string& esdeSystem,
                          Progress& progress,
                          std::string& error);

        std::string buildURL(const std::string& pathOrURL) const;

    private:
        void* createHandle(const std::string& url, std::string& error);

        std::string mServerURL;
        std::string mAPIToken;
        std::string mUsername;
        std::string mPassword;
    };

    // Extracts an entry from a ZIP archive. If entryName is not found (case-insensitive),
    // the first entry with the extension wantedExtension is used instead.
    bool extractFromZip(const std::string& archivePath,
                        const std::string& entryName,
                        const std::string& wantedExtension,
                        const std::string& destinationPath,
                        Progress* progress,
                        std::string& error);

    // URL encoding of a path segment (RFC 3986 unreserved characters are kept).
    std::string urlEncode(const std::string& string);

    // Formats a byte count for display, e.g. "1.4 GiB".
    std::string formatBytes(long long bytes);

} // namespace RomM

#endif // ES_APP_ROMM_H
