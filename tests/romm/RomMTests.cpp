//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  RomMTests.cpp
//
//  Automated tests for the RomM integration (es-app/src/RomM.cpp). The tests use the real
//  RomM::Client against tests/romm/mock_romm.py, which implements the RomM API endpoints.
//
//  Usage: es-de-tests <server URL> <empty temporary directory>
//

#include "RomM.h"

#include "Log.h"
#include "Settings.h"
#include "utils/FileSystemUtil.h"

#include <curl/curl.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace
{
    int sFailures {0};
    int sChecks {0};

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        ++sChecks;                                                                         \
        if (!(condition)) {                                                                \
            ++sFailures;                                                                   \
            std::cerr << "FAILED: " << #condition << " (" << __FILE__ << ":" << __LINE__ \
                      << ")" << std::endl;                                                 \
        }                                                                                  \
    } while (false)

    std::string readFile(const std::string& path)
    {
        std::ifstream file {path, std::ios::binary};
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::string repeat(const std::string& string, int count)
    {
        std::string result;
        for (int i {0}; i < count; ++i)
            result.append(string);
        return result;
    }

    const RomM::Platform* findPlatform(const std::vector<RomM::Platform>& platforms,
                                       const std::string& slug)
    {
        for (auto& platform : platforms) {
            if (platform.slug == slug)
                return &platform;
        }
        return nullptr;
    }

    void testHelpers()
    {
        std::cout << "== Helpers" << std::endl;
        CHECK(RomM::urlEncode("Kirby & The Amazing Mirror (USA).gba") ==
              "Kirby%20%26%20The%20Amazing%20Mirror%20%28USA%29.gba");
        CHECK(RomM::mapPlatform("gba", "gba") == "gba");
        CHECK(RomM::mapPlatform("snes", "SNES-roms") == "snes");
        CHECK(RomM::mapPlatform("genesis-slash-megadrive", "genesis") == "genesis");
        CHECK(RomM::mapPlatform("psx", "playstation") == "psx");
        CHECK(RomM::mapPlatform("unknown-thing", "unknown-thing").empty());

        Settings::getInstance()->setString("RomMPlatformMappings", "unknown-thing=amiga");
        CHECK(RomM::mapPlatform("unknown-thing", "unknown-thing") == "amiga");
        Settings::getInstance()->setString("RomMPlatformMappings", "");

        const std::string path {Utils::FileSystem::getHomePath() + "/placeholder.sfc"};
        RomM::Placeholder placeholder;
        placeholder.romId = 1234;
        placeholder.archiveExtension = "zip";
        CHECK(RomM::writePlaceholder(path, placeholder));
        CHECK(readFile(path).find("RomM-ES:1234:zip") == 0);
        const auto readBack = RomM::readPlaceholder(path);
        CHECK(readBack && readBack->romId == 1234 && readBack->archiveExtension == "zip");

        std::ofstream {path, std::ios::binary} << "Not a placeholder";
        CHECK(!RomM::readPlaceholder(path));
        Utils::FileSystem::removeFile(path);
    }

    void testConnection(const std::string& serverURL)
    {
        std::cout << "== Connection" << std::endl;
        std::string message;

        Settings::getInstance()->setString("RomMAPIToken", "wrongtoken");
        CHECK(!RomM::Client().testConnection(message));
        std::cout << "   wrong token: " << message << std::endl;

        Settings::getInstance()->setString("RomMServerURL", "http://127.0.0.1:1");
        CHECK(!RomM::Client().testConnection(message));
        std::cout << "   unreachable server: " << message << std::endl;

        Settings::getInstance()->setString("RomMServerURL", serverURL + "/");
        Settings::getInstance()->setString("RomMAPIToken", "testtoken");
        CHECK(RomM::isConfigured());
        CHECK(RomM::Client().testConnection(message));
        std::cout << "   valid token: " << message << std::endl;
    }

    void testImportAndDownload()
    {
        std::cout << "== Platforms" << std::endl;
        RomM::Client client;
        std::vector<RomM::Platform> platforms;
        std::string error;
        CHECK(client.getPlatforms(platforms, error));
        CHECK(platforms.size() == 4);
        for (auto& platform : platforms) {
            std::cout << "   " << platform.slug << " (" << platform.romCount << " games) -> \""
                      << platform.esdeSystem << "\"" << std::endl;
        }

        const RomM::Platform* gba {findPlatform(platforms, "gba")};
        const RomM::Platform* snes {findPlatform(platforms, "snes")};
        const RomM::Platform* genesis {findPlatform(platforms, "genesis-slash-megadrive")};
        const RomM::Platform* unknown {findPlatform(platforms, "unknown-thing")};
        CHECK(gba && gba->esdeSystem == "gba" && gba->romCount == 262);
        CHECK(snes && snes->esdeSystem == "snes");
        CHECK(genesis && genesis->esdeSystem == "genesis");
        CHECK(unknown && unknown->esdeSystem.empty());
        if (sFailures != 0)
            return;

        std::cout << "== Import" << std::endl;
        RomM::Progress progress;
        RomM::ImportStats stats;
        CHECK(client.importPlatforms(platforms, progress, stats, error));
        std::cout << "   imported " << stats.imported << ", updated " << stats.updated
                  << ", skipped " << stats.skipped << ", failed " << stats.failed << ", media "
                  << stats.mediaFiles << std::endl;
        // 262 GBA games (more than one page of 250), 2 SNES games and 1 Genesis game.
        CHECK(stats.imported == 265);
        CHECK(stats.failed == 0);
        CHECK(stats.mediaFiles > 0);

        const std::string romDir {Utils::FileSystem::getHomePath() + "/ROMs"};
        const std::string kirby {romDir + "/gba/Kirby & The Amazing Mirror (USA).gba"};
        const std::string metroid {romDir + "/snes/Super Metroid (USA).sfc"};
        const std::string plain {romDir + "/snes/Plain Game (USA).sfc"};
        const std::string multi {romDir + "/genesis/Multi Disc Game.zip"};
        CHECK(Utils::FileSystem::exists(romDir + "/gba/Homebrew Game 261 (World).gba"));
        CHECK(!Utils::FileSystem::exists(romDir + "/unknown-thing"));

        auto placeholder = RomM::readPlaceholder(kirby);
        CHECK(placeholder && placeholder->romId == 1 && placeholder->archiveExtension.empty());
        placeholder = RomM::readPlaceholder(metroid);
        CHECK(placeholder && placeholder->romId == 500 && placeholder->archiveExtension == "zip");
        placeholder = RomM::readPlaceholder(plain);
        CHECK(placeholder && placeholder->romId == 501 && placeholder->archiveExtension.empty());
        CHECK(RomM::readPlaceholder(multi).has_value());

        const std::string gamelist {
            readFile(Utils::FileSystem::getAppDataDirectory() + "/gamelists/gba/gamelist.xml")};
        CHECK(gamelist.find("<path>./Kirby &amp; The Amazing Mirror (USA).gba</path>") !=
              std::string::npos);
        CHECK(gamelist.find("<name>Kirby &amp; The Amazing Mirror</name>") !=
              std::string::npos);
        CHECK(gamelist.find("<publisher>Nintendo</publisher>") != std::string::npos);
        CHECK(gamelist.find("<developer>Nintendo EAD</developer>") != std::string::npos);
        CHECK(gamelist.find("<players>1-2</players>") != std::string::npos);

        const std::string mediaDir {Utils::FileSystem::getAppDataDirectory() +
                                    "/downloaded_media/gba"};
        CHECK(Utils::FileSystem::exists(mediaDir + "/covers/Kirby & The Amazing Mirror (USA).png"));
        CHECK(Utils::FileSystem::exists(mediaDir +
                                        "/screenshots/Kirby & The Amazing Mirror (USA).png"));

        std::cout << "== Re-import (must not duplicate)" << std::endl;
        RomM::ImportStats stats2;
        CHECK(client.importPlatforms(platforms, progress, stats2, error));
        CHECK(stats2.imported == 0 && stats2.skipped == 265);

        std::cout << "== Download" << std::endl;
        RomM::Progress downloadProgress;
        CHECK(client.downloadGame(kirby, "gba", downloadProgress, error));
        CHECK(readFile(kirby) == repeat("GBAROM", 200000));
        CHECK(!RomM::readPlaceholder(kirby));
        CHECK(!Utils::FileSystem::exists(kirby + ".romm"));
        CHECK(!Utils::FileSystem::exists(kirby + ".romm-part"));

        std::cout << "== Download and extract ZIP" << std::endl;
#if defined(ROMM_ZIP_SUPPORT)
        CHECK(client.downloadGame(metroid, "snes", downloadProgress, error));
        std::string sfc;
        for (int i {0}; i < 4096; ++i) {
            for (int c {0}; c < 256; ++c)
                sfc.push_back(static_cast<char>(c));
        }
        CHECK(readFile(metroid) == sfc);
        CHECK(!Utils::FileSystem::exists(romDir + "/snes/Super Metroid (USA).zip"));
#else
        std::cout << "   skipped, built without ROMM_ZIP_SUPPORT" << std::endl;
#endif

        CHECK(client.downloadGame(plain, "snes", downloadProgress, error));
        CHECK(readFile(plain) == repeat("PLAIN", 1000));
        CHECK(client.downloadGame(multi, "genesis", downloadProgress, error));
        CHECK(readFile(multi).compare(0, 2, "PK") == 0);

        std::cout << "== Stale and missing IDs" << std::endl;
        const std::string stale {romDir + "/gba/Stale Game.gba"};
        RomM::Placeholder stalePlaceholder;
        stalePlaceholder.romId = 2;
        CHECK(RomM::writePlaceholder(stale, stalePlaceholder));
        CHECK(!client.downloadGame(stale, "gba", downloadProgress, error));
        std::cout << "   " << error << std::endl;
        CHECK(error.find("now refers to") != std::string::npos);
        CHECK(RomM::readPlaceholder(stale).has_value());

        stalePlaceholder.romId = 99999;
        CHECK(RomM::writePlaceholder(stale, stalePlaceholder));
        CHECK(!client.downloadGame(stale, "gba", downloadProgress, error));
        std::cout << "   " << error << std::endl;
        CHECK(error.find("no longer exists") != std::string::npos);

        std::cout << "== Cancel" << std::endl;
        const std::string big {romDir + "/gba/Zzz Big Game (USA).gba"};
        RomM::Progress cancelProgress;
        std::thread canceller {[&cancelProgress] {
            while (cancelProgress.bytesNow < 1024 * 1024)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            cancelProgress.cancel = true;
        }};
        const bool cancelled {!client.downloadGame(big, "gba", cancelProgress, error)};
        canceller.join();
        CHECK(cancelled);
        std::cout << "   " << error << std::endl;
        CHECK(RomM::readPlaceholder(big).has_value());
        CHECK(!Utils::FileSystem::exists(big + ".romm-part"));
    }
} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage: es-de-tests <RomM server URL> <empty temporary directory>"
                  << std::endl;
        return 2;
    }

    const std::string serverURL {argv[1]};
    const std::string home {argv[2]};
    Utils::FileSystem::setHomePath(home);
    Utils::FileSystem::createDirectory(Utils::FileSystem::getAppDataDirectory() + "/logs");
    Log::init();
    Log::open();
    Log::setReportingLevel(LogInfo);
    curl_global_init(CURL_GLOBAL_ALL);

    Settings::getInstance()->setString("ROMDirectory", home + "/ROMs");
    Settings::getInstance()->setBool("RomMEnabled", true);
    Settings::getInstance()->setString("RomMServerURL", serverURL);
    Settings::getInstance()->setString("RomMAPIToken", "testtoken");
    Settings::getInstance()->setBool("RomMImportMedia", true);
    Settings::getInstance()->setString("RomMExtractedExtensions", "snes=sfc");

    testHelpers();
    testConnection(serverURL);
    testImportAndDownload();

    Log::close();
    curl_global_cleanup();

    std::cout << std::endl
              << (sChecks - sFailures) << "/" << sChecks << " checks passed" << std::endl;
    return sFailures == 0 ? 0 : 1;
}
