//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  RomM.cpp
//
//  Integration with a RomM server (https://romm.app).
//  See RomM.h for details.
//

#include "RomM.h"

#include "ApplicationVersion.h"
#include "FileData.h"
#include "Log.h"
#include "Settings.h"
#include "SystemData.h"
#include "resources/ResourceManager.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

#include <curl/curl.h>
#include <pugixml.hpp>

#if defined(ROMM_ZIP_SUPPORT)
#include <zlib.h>
#endif

#include <algorithm>
#include <cstdio>
#include <memory>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace
{
    // RomM platform slug (IGDB style) -> ES-DE system name.
    // The RomM fs_slug (directory name on the server) is tried first and used directly if it's
    // already a valid ES-DE system name, so this table only needs to cover the cases where the
    // names differ. Users can add or override mappings in the RomMPlatformMappings setting.
    const std::map<std::string, std::string> sPlatformMap {
        // clang-format off
        {"3do", "3do"},
        {"3ds", "n3ds"},
        {"new-nintendo-3ds", "n3ds"},
        {"acpc", "amstradcpc"},
        {"amstrad-cpc", "amstradcpc"},
        {"amiga", "amiga"},
        {"amiga-cd32", "amigacd32"},
        {"apple-ii", "apple2"},
        {"appleii", "apple2"},
        {"apple-iigs", "apple2gs"},
        {"arcade", "arcade"},
        {"atari2600", "atari2600"},
        {"atari5200", "atari5200"},
        {"atari7800", "atari7800"},
        {"atari8bit", "atari800"},
        {"atari-st", "atarist"},
        {"jaguar", "atarijaguar"},
        {"atari-jaguar-cd", "atarijaguarcd"},
        {"lynx", "atarilynx"},
        {"c64", "c64"},
        {"vic-20", "vic20"},
        {"c-plus-4", "plus4"},
        {"colecovision", "colecovision"},
        {"cps1", "cps1"},
        {"cps2", "cps2"},
        {"cps3", "cps3"},
        {"dc", "dreamcast"},
        {"dos", "dos"},
        {"fds", "fds"},
        {"famicom", "famicom"},
        {"fm-towns", "fmtowns"},
        {"g-and-w", "gameandwatch"},
        {"game-and-watch", "gameandwatch"},
        {"gamegear", "gamegear"},
        {"gb", "gb"},
        {"gba", "gba"},
        {"gbc", "gbc"},
        {"genesis-slash-megadrive", "megadrive"},
        {"genesis", "genesis"},
        {"megadrive", "megadrive"},
        {"intellivision", "intellivision"},
        {"mame", "mame"},
        {"msx", "msx"},
        {"msx2", "msx2"},
        {"n64", "n64"},
        {"64dd", "n64dd"},
        {"nds", "nds"},
        {"neogeoaes", "neogeo"},
        {"neogeomvs", "neogeo"},
        {"neo-geo", "neogeo"},
        {"neo-geo-cd", "neogeocd"},
        {"neo-geo-pocket", "ngp"},
        {"neo-geo-pocket-color", "ngpc"},
        {"nes", "nes"},
        {"ngc", "gc"},
        {"gamecube", "gc"},
        {"odyssey-2", "odyssey2"},
        {"odyssey-2-slash-videopac-g7000", "odyssey2"},
        {"pc-8800-series", "pc88"},
        {"pc-9800-series", "pc98"},
        {"pc-fx", "pcfx"},
        {"philips-cd-i", "cdimono1"},
        {"pico-8", "pico8"},
        {"pokemon-mini", "pokemini"},
        {"ps2", "ps2"},
        {"ps3", "ps3"},
        {"ps4", "ps4"},
        {"psp", "psp"},
        {"psvita", "psvita"},
        {"psx", "psx"},
        {"saturn", "saturn"},
        {"scummvm", "scummvm"},
        {"segacd", "segacd"},
        {"sega-cd", "segacd"},
        {"sega32", "sega32x"},
        {"sega-32x", "sega32x"},
        {"sg1000", "sg-1000"},
        {"sms", "mastersystem"},
        {"snes", "snes"},
        {"sfam", "sfc"},
        {"supergrafx", "supergrafx"},
        {"switch", "switch"},
        {"turbografx16--1", "pcengine"},
        {"turbografx-16-slash-pc-engine", "pcengine"},
        {"tg16", "tg16"},
        {"turbografx-16-slash-pc-engine-cd", "pcenginecd"},
        {"turbografx-cd", "tg-cd"},
        {"vectrex", "vectrex"},
        {"virtualboy", "virtualboy"},
        {"wii", "wii"},
        {"wiiu", "wiiu"},
        {"win", "windows"},
        {"wonderswan", "wonderswan"},
        {"wonderswan-color", "wonderswancolor"},
        {"x68000", "x68000"},
        {"xbox", "xbox"},
        {"xbox360", "xbox360"},
        {"xboxone", "xboxone"},
        {"zxs", "zxspectrum"},
        {"zx-spectrum", "zxspectrum"},
        {"zx81", "zx81"},
        // clang-format on
    };

    // Media types: ES-DE media directory -> RomM JSON location.
    struct MediaSource {
        std::string esdeDirectory;
        std::string field; // Field in the ROM object, or in ss_metadata if ssMetadata is true.
        bool ssMetadata;
        bool isVideo;
    };

    const std::vector<MediaSource> sMediaSources {
        // clang-format off
        {"covers",        "path_cover_large",   false, false},
        {"screenshots",   "merged_screenshots", false, false},
        {"manuals",       "path_manual",        false, false},
        {"3dboxes",       "box3d_path",         true,  false},
        {"backcovers",    "box2d_back_path",    true,  false},
        {"fanart",        "fanart_path",        true,  false},
        {"marquees",      "logo_path",          true,  false},
        {"miximages",     "miximage_path",      true,  false},
        {"physicalmedia", "physical_path",      true,  false},
        {"titlescreens",  "title_screen_path",  true,  false},
        {"videos",        "path_video",         false, true},
        // clang-format on
    };

    std::string jsonString(const rapidjson::Value& object, const char* member)
    {
        if (!object.IsObject())
            return "";
        auto it = object.FindMember(member);
        if (it == object.MemberEnd() || !it->value.IsString())
            return "";
        return it->value.GetString();
    }

    long long jsonInt(const rapidjson::Value& object, const char* member, long long def = 0)
    {
        if (!object.IsObject())
            return def;
        auto it = object.FindMember(member);
        if (it == object.MemberEnd() || !it->value.IsNumber())
            return def;
        return it->value.IsInt64() ? it->value.GetInt64() :
                                     static_cast<long long>(it->value.GetDouble());
    }

    bool jsonBool(const rapidjson::Value& object, const char* member)
    {
        if (!object.IsObject())
            return false;
        auto it = object.FindMember(member);
        return it != object.MemberEnd() && it->value.IsBool() && it->value.GetBool();
    }

    const rapidjson::Value* jsonMember(const rapidjson::Value& object, const char* member)
    {
        if (!object.IsObject())
            return nullptr;
        auto it = object.FindMember(member);
        if (it == object.MemberEnd() || it->value.IsNull())
            return nullptr;
        return &it->value;
    }

    // Opens a file for binary writing, using wide paths on Windows.
    std::FILE* openFile(const std::string& path, const char* mode)
    {
#if defined(_WIN64)
        std::wstring wideMode {Utils::String::stringToWideString(mode)};
        return _wfopen(Utils::String::stringToWideString(path).c_str(), wideMode.c_str());
#else
        return std::fopen(path.c_str(), mode);
#endif
    }

    // Moves a file and replaces any existing destination file. Returns true on success.
    // Utils::FileSystem::renameFile() is not used as it can't replace existing files on
    // Windows (_wrename fails if the destination exists) while std::filesystem::rename()
    // replaces the destination atomically on all platforms.
    bool moveFile(const std::string& source, const std::string& destination, bool overwrite)
    {
        if (!overwrite && Utils::FileSystem::exists(destination))
            return false;
        std::error_code errorCode;
#if defined(_WIN64)
        const std::wstring wideSource {Utils::String::stringToWideString(source)};
        const std::wstring wideDestination {Utils::String::stringToWideString(destination)};
        std::filesystem::rename(wideSource, wideDestination, errorCode);
        if (errorCode) {
            // Fallback for sandboxed environments (UWP) where the file system rename call may
            // not be permitted outside the application directories. _wrename() can't replace
            // an existing file so the destination is removed first.
            if (_wremove(wideDestination.c_str()) == 0 || !Utils::FileSystem::exists(destination)) {
                if (_wrename(wideSource.c_str(), wideDestination.c_str()) == 0)
                    errorCode.clear();
            }
        }
#else
        std::filesystem::rename(source, destination, errorCode);
#endif
        if (errorCode) {
            LOG(LogError) << "RomM: Couldn't move \"" << source << "\" to \"" << destination
                          << "\": " << errorCode.message();
            return false;
        }
        return true;
    }

    std::string readSmallFile(const std::string& path, size_t maxSize)
    {
        std::FILE* file {openFile(path, "rb")};
        if (file == nullptr)
            return "";
        std::string content(maxSize, '\0');
        const size_t read {std::fread(&content[0], 1, maxSize, file)};
        std::fclose(file);
        content.resize(read);
        return content;
    }

    std::string trimSlashes(std::string url)
    {
        url = Utils::String::trim(url);
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        return url;
    }

    // The ROM directory for an ES-DE system. If the system is loaded then its configured
    // path is used, otherwise the default <ROM directory>/<system> path.
    std::string getSystemROMPath(const std::string& esdeSystem)
    {
        for (auto system : SystemData::sSystemVector) {
            if (!system->isCollection() && system->getName() == esdeSystem &&
                system->getRootFolder() != nullptr)
                return system->getRootFolder()->getPath();
        }
        std::string romDir {FileData::getROMDirectory()};
        if (!romDir.empty() && romDir.back() != '/')
            romDir.push_back('/');
        return romDir + esdeSystem;
    }

    std::string getSystemGamelistPath(const std::string& esdeSystem)
    {
        for (auto system : SystemData::sSystemVector) {
            if (!system->isCollection() && system->getName() == esdeSystem)
                return system->getGamelistPath(true);
        }
        const std::string dir {Utils::FileSystem::getAppDataDirectory() + "/gamelists/" +
                               esdeSystem};
        Utils::FileSystem::createDirectory(dir);
        return dir + "/gamelist.xml";
    }

    // Characters not allowed in file names on Windows/Xbox.
    std::string sanitizeFileName(const std::string& name)
    {
        std::string result;
        for (char c : name) {
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
                c == '|' || c == '?' || c == '*' || (static_cast<unsigned char>(c) < 32))
                result.push_back('_');
            else
                result.push_back(c);
        }
        return result;
    }

    struct CurlBuffer {
        std::string* string {nullptr};
        std::FILE* file {nullptr};
        CURL* handle {nullptr};
        long httpStatus {0};
        std::string errorBody;
        bool writeError {false};
        RomM::Progress* progress {nullptr};
    };

    size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        CurlBuffer* buffer {static_cast<CurlBuffer*>(userdata)};
        const size_t length {size * nmemb};

        if (buffer->httpStatus == 0)
            curl_easy_getinfo(buffer->handle, CURLINFO_RESPONSE_CODE, &buffer->httpStatus);

        if (buffer->httpStatus >= 400) {
            // Keep the beginning of the error body for diagnostics, don't write it to disk.
            if (buffer->errorBody.size() < 2048)
                buffer->errorBody.append(ptr, std::min<size_t>(length, 2048));
            return length;
        }

        if (buffer->file != nullptr) {
            if (std::fwrite(ptr, 1, length, buffer->file) != length) {
                buffer->writeError = true;
                return 0;
            }
        }
        else if (buffer->string != nullptr) {
            buffer->string->append(ptr, length);
        }
        return length;
    }

    int curlProgress(void* clientp,
                     curl_off_t dltotal,
                     curl_off_t dlnow,
                     curl_off_t /*ultotal*/,
                     curl_off_t /*ulnow*/)
    {
        CurlBuffer* buffer {static_cast<CurlBuffer*>(clientp)};
        if (buffer->progress != nullptr) {
            if (dltotal > 0)
                buffer->progress->bytesTotal = static_cast<long long>(dltotal);
            buffer->progress->bytesNow = static_cast<long long>(dlnow);
            if (buffer->progress->cancel)
                return 1; // Abort the transfer.
        }
        return 0;
    }

    std::string httpErrorMessage(long status, const std::string& body)
    {
        std::string detail;
        rapidjson::Document doc;
        doc.Parse(body.c_str());
        if (!doc.HasParseError() && doc.IsObject())
            detail = jsonString(doc, "detail");
        if (status == 401 || status == 403)
            return "HTTP " + std::to_string(status) + ": authentication failed, check the RomM " +
                   "API token or username/password" + (detail.empty() ? "" : " (" + detail + ")");
        return "HTTP " + std::to_string(status) + (detail.empty() ? "" : ": " + detail);
    }

    void setChild(pugi::xml_node game, const char* name, const std::string& value)
    {
        if (value.empty())
            return;
        pugi::xml_node node {game.child(name)};
        if (!node)
            node = game.append_child(name);
        node.text().set(value.c_str());
    }

} // namespace

namespace RomM
{
    bool isConfigured()
    {
        return Settings::getInstance()->getBool("RomMEnabled") &&
               !trimSlashes(Settings::getInstance()->getString("RomMServerURL")).empty();
    }

    bool downloadOnLaunchEnabled()
    {
        return isConfigured() && Settings::getInstance()->getBool("RomMDownloadOnLaunch");
    }

    std::optional<Placeholder> readPlaceholder(const std::string& path)
    {
        if (!Utils::FileSystem::isRegularFile(path))
            return std::nullopt;
        const long size {Utils::FileSystem::getFileSize(path)};
        if (size <= 0 || size > PLACEHOLDER_MAX_SIZE)
            return std::nullopt;

        std::string content {readSmallFile(path, PLACEHOLDER_MAX_SIZE)};
        content = Utils::String::trim(content);

        const std::string prefix {"RomM-ES:"};
        if (content.compare(0, prefix.size(), prefix) != 0)
            return std::nullopt;

        content = content.substr(prefix.size());
        const size_t separator {content.find(':')};
        const std::string idString {content.substr(0, separator)};
        if (idString.empty() || idString.size() > 10 ||
            idString.find_first_not_of("0123456789") != std::string::npos)
            return std::nullopt;

        Placeholder placeholder;
        placeholder.romId = std::stoi(idString);
        if (separator != std::string::npos) {
            placeholder.archiveExtension = content.substr(separator + 1);
            for (char c : placeholder.archiveExtension) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                    return std::nullopt;
            }
        }
        if (placeholder.romId <= 0)
            return std::nullopt;
        return placeholder;
    }

    std::string placeholderText(const Placeholder& placeholder)
    {
        std::string text {"RomM-ES:" + std::to_string(placeholder.romId)};
        if (!placeholder.archiveExtension.empty())
            text.append(":").append(placeholder.archiveExtension);
        return text;
    }

    bool writePlaceholder(const std::string& path, const Placeholder& placeholder)
    {
        Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(path));
        std::FILE* file {openFile(path, "wb")};
        if (file == nullptr)
            return false;
        const std::string text {placeholderText(placeholder)};
        const bool success {std::fwrite(text.data(), 1, text.size(), file) == text.size()};
        std::fclose(file);
        return success;
    }

    std::map<std::string, std::string> parseKeyValueSetting(const std::string& setting)
    {
        std::map<std::string, std::string> result;
        for (auto& entry : Utils::String::delimitedStringToVector(setting, ",")) {
            const size_t equals {entry.find('=')};
            if (equals == std::string::npos)
                continue;
            std::string key {Utils::String::trim(entry.substr(0, equals))};
            std::string value {Utils::String::trim(entry.substr(equals + 1))};
            if (!value.empty() && value.front() == '.')
                value = value.substr(1);
            if (!key.empty() && !value.empty())
                result[Utils::String::toLower(key)] = value;
        }
        return result;
    }

    std::map<std::string, std::string> getExtractedExtensions()
    {
        return parseKeyValueSetting(Settings::getInstance()->getString("RomMExtractedExtensions"));
    }

    std::vector<std::string> getSelectedPlatforms()
    {
        std::vector<std::string> result;
        for (auto& entry : Utils::String::delimitedStringToVector(
                 Settings::getInstance()->getString("RomMImportPlatforms"), ",")) {
            entry = Utils::String::trim(entry);
            if (!entry.empty())
                result.emplace_back(entry);
        }
        return result;
    }

    std::string mapPlatform(const std::string& slug, const std::string& fsSlug)
    {
        // User-defined mappings take precedence, keyed on either the slug or the fs_slug.
        const auto custom {
            parseKeyValueSetting(Settings::getInstance()->getString("RomMPlatformMappings"))};
        auto it = custom.find(Utils::String::toLower(fsSlug));
        if (it != custom.end())
            return it->second;
        it = custom.find(Utils::String::toLower(slug));
        if (it != custom.end())
            return it->second;

        // If the directory name on the RomM server already is an ES-DE system name, use it.
        std::set<std::string> esdeSystems;
        for (auto& entry : sPlatformMap)
            esdeSystems.insert(entry.second);
        for (auto system : SystemData::sSystemVector) {
            if (!system->isCollection())
                esdeSystems.insert(system->getName());
        }
        const std::string fsLower {Utils::String::toLower(fsSlug)};
        if (esdeSystems.find(fsLower) != esdeSystems.end())
            return fsLower;

        auto mapped = sPlatformMap.find(Utils::String::toLower(slug));
        if (mapped != sPlatformMap.end())
            return mapped->second;
        mapped = sPlatformMap.find(fsLower);
        if (mapped != sPlatformMap.end())
            return mapped->second;

        return "";
    }

    std::string urlEncode(const std::string& string)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex << std::uppercase;
        for (unsigned char c : string) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                escaped << c;
            else
                escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
        return escaped.str();
    }

    std::string formatBytes(long long bytes)
    {
        const char* units[] {"B", "KiB", "MiB", "GiB", "TiB"};
        double value {static_cast<double>(bytes)};
        int unit {0};
        while (value >= 1024.0 && unit < 4) {
            value /= 1024.0;
            ++unit;
        }
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " "
               << units[unit];
        return stream.str();
    }

    // -------------------------------------------------------------------------------------
    // Client.
    // -------------------------------------------------------------------------------------

    Client::Client()
        : mServerURL {trimSlashes(Settings::getInstance()->getString("RomMServerURL"))}
        , mAPIToken {Utils::String::trim(Settings::getInstance()->getString("RomMAPIToken"))}
        , mUsername {Settings::getInstance()->getString("RomMUsername")}
        , mPassword {Settings::getInstance()->getString("RomMPassword")}
    {
        if (!mServerURL.empty() && mServerURL.find("://") == std::string::npos)
            mServerURL = "http://" + mServerURL;
    }

    std::string Client::buildURL(const std::string& pathOrURL) const
    {
        if (pathOrURL.compare(0, 7, "http://") == 0 || pathOrURL.compare(0, 8, "https://") == 0)
            return pathOrURL;
        if (!pathOrURL.empty() && pathOrURL.front() == '/')
            return mServerURL + pathOrURL;
        return mServerURL + "/" + pathOrURL;
    }

    void* Client::createHandle(const std::string& url, std::string& error)
    {
        CURL* handle {curl_easy_init()};
        if (handle == nullptr) {
            error = "Couldn't initialize curl";
            return nullptr;
        }

#if defined(USE_BUNDLED_CERTIFICATES)
        curl_easy_setopt(handle, CURLOPT_CAINFO,
                         ResourceManager::getInstance()
                             .getResourcePath(":/certificates/curl-ca-bundle.crt")
                             .c_str());
#endif
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        const std::string userAgent {std::string {"ES-DE Frontend/"} + PROGRAM_VERSION_STRING +
                                     " (RomM)"};
        curl_easy_setopt(handle, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
        // Abort if the transfer speed is below 1 byte/s for 60 seconds.
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");

        if (mAPIToken.empty() && !mUsername.empty()) {
            curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(handle, CURLOPT_USERNAME, mUsername.c_str());
            curl_easy_setopt(handle, CURLOPT_PASSWORD, mPassword.c_str());
        }
        return handle;
    }

    bool Client::get(const std::string& pathOrURL,
                     std::string& response,
                     std::string& error,
                     long* httpStatus)
    {
        response.clear();
        if (mServerURL.empty()) {
            error = "No RomM server URL configured";
            return false;
        }

        const std::string url {buildURL(pathOrURL)};
        CURL* handle {static_cast<CURL*>(createHandle(url, error))};
        if (handle == nullptr)
            return false;

        struct curl_slist* headers {nullptr};
        headers = curl_slist_append(headers, "Accept: application/json");
        if (!mAPIToken.empty()) {
            const std::string auth {"Authorization: Bearer " + mAPIToken};
            headers = curl_slist_append(headers, auth.c_str());
        }
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

        CurlBuffer buffer;
        buffer.string = &response;
        buffer.handle = handle;
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curlWrite);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);

        const CURLcode result {curl_easy_perform(handle)};
        long status {0};
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);

        if (httpStatus != nullptr)
            *httpStatus = status;

        if (result != CURLE_OK) {
            error = curl_easy_strerror(result);
            LOG(LogWarning) << "RomM: Request to \"" << url << "\" failed: " << error;
            return false;
        }
        if (status >= 400) {
            error = httpErrorMessage(status, buffer.errorBody);
            LOG(LogWarning) << "RomM: Request to \"" << url << "\" failed: " << error;
            return false;
        }
        return true;
    }

    bool Client::download(const std::string& pathOrURL,
                          const std::string& destinationPath,
                          Progress* progress,
                          std::string& error)
    {
        const std::string url {buildURL(pathOrURL)};
        const std::string tempPath {destinationPath + ".romm-part"};

        Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(destinationPath));
        std::FILE* file {openFile(tempPath, "wb")};
        if (file == nullptr) {
            error = "Couldn't open \"" + tempPath + "\" for writing";
            return false;
        }

        CURL* handle {static_cast<CURL*>(createHandle(url, error))};
        if (handle == nullptr) {
            std::fclose(file);
            Utils::FileSystem::removeFile(tempPath);
            return false;
        }

        struct curl_slist* headers {nullptr};
        if (!mAPIToken.empty()) {
            const std::string auth {"Authorization: Bearer " + mAPIToken};
            headers = curl_slist_append(headers, auth.c_str());
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        }
        // Content is binary, don't ask for compression.
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, nullptr);

        CurlBuffer buffer;
        buffer.file = file;
        buffer.handle = handle;
        buffer.progress = progress;
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curlWrite);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, curlProgress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &buffer);

        if (progress != nullptr) {
            progress->bytesNow = 0;
            progress->bytesTotal = 0;
        }

        LOG(LogDebug) << "RomM: Downloading \"" << url << "\" to \"" << destinationPath << "\"";

        const CURLcode result {curl_easy_perform(handle)};
        long status {0};
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        const bool closeError {std::fclose(file) != 0};

        bool success {false};
        if (progress != nullptr && progress->cancel)
            error = "Download cancelled";
        else if (buffer.writeError || closeError)
            error = "Couldn't write to \"" + destinationPath + "\", disk full?";
        else if (result != CURLE_OK)
            error = curl_easy_strerror(result);
        else if (status >= 400)
            error = httpErrorMessage(status, buffer.errorBody);
        else
            success = true;

        if (!success) {
            LOG(LogWarning) << "RomM: Download of \"" << url << "\" failed: " << error;
            Utils::FileSystem::removeFile(tempPath);
            return false;
        }

        if (!moveFile(tempPath, destinationPath, true)) {
            error = "Couldn't rename \"" + tempPath + "\" to \"" + destinationPath + "\"";
            Utils::FileSystem::removeFile(tempPath);
            return false;
        }
        return true;
    }

    bool Client::testConnection(std::string& message)
    {
        std::string response;
        std::string error;

        if (!get("/api/heartbeat", response, error)) {
            message = error;
            return false;
        }

        std::string version;
        rapidjson::Document doc;
        doc.Parse(response.c_str());
        if (!doc.HasParseError() && doc.IsObject()) {
            if (const rapidjson::Value* system {jsonMember(doc, "SYSTEM")})
                version = jsonString(*system, "VERSION");
        }

        // The heartbeat endpoint doesn't require authentication, so verify the credentials.
        std::vector<Platform> platforms;
        if (!getPlatforms(platforms, error)) {
            message = error;
            return false;
        }

        int mapped {0};
        for (auto& platform : platforms) {
            if (!platform.esdeSystem.empty())
                ++mapped;
        }
        message = "RomM " + (version.empty() ? std::string {"server"} : version) + ", " +
                  std::to_string(platforms.size()) + " platforms (" + std::to_string(mapped) +
                  " supported by ES-DE)";
        return true;
    }

    bool Client::getPlatforms(std::vector<Platform>& platforms, std::string& error)
    {
        platforms.clear();
        std::string response;
        if (!get("/api/platforms", response, error))
            return false;

        rapidjson::Document doc;
        doc.Parse(response.c_str());
        if (doc.HasParseError()) {
            error = std::string {"Invalid JSON from server: "} +
                    rapidjson::GetParseError_En(doc.GetParseError());
            return false;
        }

        // Older versions return an array, potential newer versions could return a page.
        const rapidjson::Value* array {&doc};
        if (doc.IsObject()) {
            if (const rapidjson::Value* items {jsonMember(doc, "items")})
                array = items;
        }
        if (!array->IsArray()) {
            error = "Unexpected response from /api/platforms";
            return false;
        }

        for (auto& item : array->GetArray()) {
            Platform platform;
            platform.id = static_cast<int>(jsonInt(item, "id"));
            platform.slug = jsonString(item, "slug");
            platform.fsSlug = jsonString(item, "fs_slug");
            platform.name = jsonString(item, "display_name");
            if (platform.name.empty())
                platform.name = jsonString(item, "name");
            platform.romCount = static_cast<int>(jsonInt(item, "rom_count"));
            platform.esdeSystem = mapPlatform(platform.slug, platform.fsSlug);
            if (platform.id > 0)
                platforms.emplace_back(platform);
        }

        std::sort(platforms.begin(), platforms.end(),
                  [](const Platform& a, const Platform& b) { return a.name < b.name; });
        return true;
    }

    bool Client::importPlatforms(const std::vector<Platform>& platforms,
                                 Progress& progress,
                                 ImportStats& stats,
                                 std::string& error)
    {
        const bool importMedia {Settings::getInstance()->getBool("RomMImportMedia")};
        const bool importVideos {Settings::getInstance()->getBool("RomMImportVideos")};
        const auto extractedExtensions {getExtractedExtensions()};
        const std::string mediaDirectory {FileData::getMediaDirectory()};

        progress.itemsNow = 0;
        progress.itemsTotal = 0;
        for (auto& platform : platforms)
            progress.itemsTotal += platform.romCount;

        for (auto& platform : platforms) {
            if (progress.cancel)
                break;
            if (platform.esdeSystem.empty())
                continue;

            progress.setStatus(platform.name + " -> " + platform.esdeSystem);
            LOG(LogInfo) << "RomM: Importing platform \"" << platform.name << "\" ("
                         << platform.slug << ") into ES-DE system \"" << platform.esdeSystem
                         << "\"";

            const std::string romPath {getSystemROMPath(platform.esdeSystem)};
            const std::string gamelistPath {getSystemGamelistPath(platform.esdeSystem)};
            std::string extractedExtension;
            if (extractedExtensions.find(platform.esdeSystem) != extractedExtensions.end())
                extractedExtension = extractedExtensions.at(platform.esdeSystem);

            if (!Utils::FileSystem::createDirectory(romPath)) {
                error = "Couldn't create directory \"" + romPath + "\"";
                return false;
            }

            // Load the existing gamelist.xml file, or create a new document.
            pugi::xml_document gamelist;
            if (Utils::FileSystem::exists(gamelistPath) &&
                Utils::FileSystem::getFileSize(gamelistPath) > 0) {
#if defined(_WIN64)
                const pugi::xml_parse_result result {
                    gamelist.load_file(Utils::String::stringToWideString(gamelistPath).c_str())};
#else
                const pugi::xml_parse_result result {gamelist.load_file(gamelistPath.c_str())};
#endif
                if (!result) {
                    error = "Couldn't parse \"" + gamelistPath + "\": " + result.description();
                    return false;
                }
            }
            pugi::xml_node root {gamelist.child("gameList")};
            if (!root) {
                gamelist.prepend_child(pugi::node_declaration).append_attribute("version") =
                    "1.0";
                root = gamelist.append_child("gameList");
            }

            std::map<std::string, pugi::xml_node> existingEntries;
            for (pugi::xml_node game {root.child("game")}; game; game = game.next_sibling("game"))
                existingEntries[game.child("path").text().get()] = game;

            bool gamelistChanged {false};
            int offset {0};
            const int limit {250};

            while (!progress.cancel) {
                std::string response;
                const std::string request {
                    "/api/roms?platform_ids=" + std::to_string(platform.id) +
                    "&order_by=name&order_dir=asc&limit=" + std::to_string(limit) +
                    "&offset=" + std::to_string(offset) +
                    "&with_char_index=false&with_filter_values=false&with_rom_id_index=false"};
                if (!get(request, response, error))
                    return false;

                rapidjson::Document doc;
                doc.Parse(response.c_str());
                if (doc.HasParseError()) {
                    error = "Invalid JSON returned for platform " + platform.name;
                    return false;
                }

                const rapidjson::Value* items {&doc};
                long long total {-1};
                if (doc.IsObject()) {
                    items = jsonMember(doc, "items");
                    total = jsonInt(doc, "total", -1);
                }
                if (items == nullptr || !items->IsArray()) {
                    error = "Unexpected response from /api/roms";
                    return false;
                }

                for (auto& rom : items->GetArray()) {
                    if (progress.cancel)
                        break;
                    ++progress.itemsNow;

                    const int romId {static_cast<int>(jsonInt(rom, "id"))};
                    const std::string fsName {jsonString(rom, "fs_name")};
                    std::string name {jsonString(rom, "name")};
                    if (name.empty())
                        name = jsonString(rom, "fs_name_no_ext");
                    progress.setDetail(name);

                    if (romId <= 0 || fsName.empty()) {
                        ++stats.failed;
                        continue;
                    }

                    // Multi-file ROMs (e.g. multi-disc games) are served as a ZIP file.
                    const bool multiFile {jsonBool(rom, "has_multiple_files") ||
                                          jsonBool(rom, "multi")};
                    std::string serverFileName {sanitizeFileName(fsName)};
                    if (multiFile)
                        serverFileName.append(".zip");

                    Placeholder placeholder;
                    placeholder.romId = romId;
                    std::string localFileName {serverFileName};
                    if (!extractedExtension.empty()) {
                        const std::string serverExtension {
                            Utils::FileSystem::getExtension(serverFileName)};
                        if (serverExtension.size() > 1 &&
                            Utils::String::toLower(serverExtension.substr(1)) !=
                                Utils::String::toLower(extractedExtension)) {
                            placeholder.archiveExtension = serverExtension.substr(1);
                            localFileName = Utils::FileSystem::getStem(serverFileName) + "." +
                                            extractedExtension;
                        }
                    }

                    const std::string localPath {romPath + "/" + localFileName};
                    if (Utils::FileSystem::exists(localPath)) {
                        auto existing = readPlaceholder(localPath);
                        if (!existing) {
                            // The real game file is already present, don't touch it.
                            ++stats.skipped;
                        }
                        else if (existing->romId != placeholder.romId ||
                                 existing->archiveExtension != placeholder.archiveExtension) {
                            writePlaceholder(localPath, placeholder);
                            ++stats.updated;
                        }
                        else {
                            ++stats.skipped;
                        }
                    }
                    else {
                        if (!writePlaceholder(localPath, placeholder)) {
                            LOG(LogError) << "RomM: Couldn't write placeholder \"" << localPath
                                          << "\"";
                            ++stats.failed;
                            continue;
                        }
                        ++stats.imported;
                    }

                    // Gamelist entry. Only created for new entries so that any changes the
                    // user made in ES-DE are retained.
                    const std::string gamelistEntryPath {"./" + localFileName};
                    if (existingEntries.find(gamelistEntryPath) == existingEntries.end()) {
                        pugi::xml_node game {root.append_child("game")};
                        setChild(game, "path", gamelistEntryPath);
                        setChild(game, "name", name);

                        std::string summary {jsonString(rom, "summary")};
                        summary = Utils::String::replace(summary, "\r", "");
                        setChild(game, "desc", summary);

                        if (const rapidjson::Value* meta {jsonMember(rom, "metadatum")}) {
                            const long long releaseDate {
                                jsonInt(*meta, "first_release_date", -1)};
                            if (releaseDate > 0) {
                                // RomM uses milliseconds since the epoch.
                                const std::time_t seconds {
                                    static_cast<std::time_t>(releaseDate / 1000)};
                                std::tm tm {};
#if defined(_WIN64)
                                gmtime_s(&tm, &seconds);
#else
                                gmtime_r(&seconds, &tm);
#endif
                                char date[32];
                                std::strftime(date, sizeof(date), "%Y%m%dT000000", &tm);
                                setChild(game, "releasedate", date);
                            }
                            if (const rapidjson::Value* rating {
                                    jsonMember(*meta, "average_rating")}) {
                                if (rating->IsNumber()) {
                                    // RomM uses 0-100 and ES-DE uses 0-1.
                                    const double value {
                                        std::clamp(rating->GetDouble() / 100.0, 0.0, 1.0)};
                                    std::ostringstream stream;
                                    stream << std::fixed << std::setprecision(2) << value;
                                    setChild(game, "rating", stream.str());
                                }
                            }
                            if (const rapidjson::Value* genres {jsonMember(*meta, "genres")}) {
                                if (genres->IsArray() && !genres->Empty() &&
                                    (*genres)[0].IsString())
                                    setChild(game, "genre", (*genres)[0].GetString());
                            }
                            if (const rapidjson::Value* companies {
                                    jsonMember(*meta, "companies")}) {
                                if (companies->IsArray() && !companies->Empty()) {
                                    // Same order as used by RomM-ES.
                                    if ((*companies)[0].IsString())
                                        setChild(game, "publisher", (*companies)[0].GetString());
                                    if (companies->Size() > 1 && (*companies)[1].IsString())
                                        setChild(game, "developer", (*companies)[1].GetString());
                                }
                            }
                            setChild(game, "players", jsonString(*meta, "player_count"));
                        }
                        existingEntries[gamelistEntryPath] = game;
                        gamelistChanged = true;
                    }

                    // Media files.
                    if (importMedia) {
                        const rapidjson::Value* ssMetadata {jsonMember(rom, "ss_metadata")};
                        const std::string stem {Utils::FileSystem::getStem(localFileName)};
                        for (auto& source : sMediaSources) {
                            if (progress.cancel)
                                break;
                            if (source.isVideo && !importVideos)
                                continue;
                            std::string mediaURL;
                            if (source.ssMetadata) {
                                if (ssMetadata != nullptr)
                                    mediaURL = jsonString(*ssMetadata, source.field.c_str());
                            }
                            else if (source.field == "merged_screenshots") {
                                if (const rapidjson::Value* screenshots {
                                        jsonMember(rom, "merged_screenshots")}) {
                                    if (screenshots->IsArray() && !screenshots->Empty() &&
                                        (*screenshots)[0].IsString())
                                        mediaURL = (*screenshots)[0].GetString();
                                }
                            }
                            else {
                                mediaURL = jsonString(rom, source.field.c_str());
                            }
                            if (mediaURL.empty())
                                continue;

                            // Relative paths are served from the RomM resources directory.
                            if (mediaURL.compare(0, 4, "http") != 0 &&
                                mediaURL.compare(0, 8, "/assets/") != 0) {
                                mediaURL = "/assets/romm/resources/" +
                                           (mediaURL.front() == '/' ? mediaURL.substr(1) :
                                                                      mediaURL);
                            }

                            std::string extension;
                            {
                                std::string urlPath {mediaURL.substr(0, mediaURL.find('?'))};
                                extension = Utils::String::toLower(
                                    Utils::FileSystem::getExtension(urlPath));
                                if (extension.empty() || extension.size() > 5)
                                    extension = source.isVideo ? ".mp4" : ".png";
                            }

                            const std::string mediaPath {mediaDirectory + platform.esdeSystem +
                                                         "/" + source.esdeDirectory + "/" +
                                                         stem + extension};
                            if (Utils::FileSystem::exists(mediaPath))
                                continue;

                            std::string mediaError;
                            if (download(mediaURL, mediaPath, nullptr, mediaError))
                                ++stats.mediaFiles;
                            else
                                LOG(LogDebug) << "RomM: Couldn't download media \"" << mediaURL
                                              << "\": " << mediaError;
                        }
                    }
                }

                const int received {static_cast<int>(items->Size())};
                offset += received;
                if (received < limit || (total >= 0 && offset >= total))
                    break;
            }

            if (gamelistChanged) {
#if defined(_WIN64)
                const bool saved {
                    gamelist.save_file(Utils::String::stringToWideString(gamelistPath).c_str())};
#else
                const bool saved {gamelist.save_file(gamelistPath.c_str())};
#endif
                if (!saved) {
                    error = "Couldn't write \"" + gamelistPath + "\"";
                    return false;
                }
            }
        }

        if (progress.cancel) {
            error = "Import cancelled";
            return false;
        }
        return true;
    }

    bool Client::downloadGame(const std::string& placeholderPath,
                              const std::string& esdeSystem,
                              Progress& progress,
                              std::string& error)
    {
        const auto placeholder = readPlaceholder(placeholderPath);
        if (!placeholder) {
            error = "Not a RomM placeholder file";
            return false;
        }

        progress.setStatus("Requesting game information");

        std::string response;
        if (!get("/api/roms/" + std::to_string(placeholder->romId), response, error)) {
            if (error.compare(0, 8, "HTTP 404") == 0)
                error = "The game (RomM ID " + std::to_string(placeholder->romId) +
                        ") no longer exists on the server, run the RomM importer again";
            return false;
        }

        rapidjson::Document rom;
        rom.Parse(response.c_str());
        if (rom.HasParseError() || !rom.IsObject()) {
            error = "Invalid JSON returned for RomM ID " + std::to_string(placeholder->romId);
            return false;
        }

        const std::string fsName {jsonString(rom, "fs_name")};
        const bool multiFile {jsonBool(rom, "has_multiple_files") || jsonBool(rom, "multi")};
        std::string serverFileName {sanitizeFileName(fsName)};
        if (multiFile)
            serverFileName.append(".zip");

        // Verify that the ID still points to the same game, as IDs change if a game is
        // removed and rescanned on the server.
        const std::string placeholderName {Utils::FileSystem::getFileName(placeholderPath)};
        const std::string expectedServerName {
            placeholder->archiveExtension.empty() ?
                placeholderName :
                Utils::FileSystem::getStem(placeholderPath) + "." + placeholder->archiveExtension};

        if (Utils::String::toLower(expectedServerName) != Utils::String::toLower(serverFileName)) {
            error = "The RomM ID " + std::to_string(placeholder->romId) + " now refers to \"" +
                    fsName + "\" instead of \"" + expectedServerName +
                    "\", run the RomM importer again";
            return false;
        }

        const std::string platformSystem {
            mapPlatform(jsonString(rom, "platform_slug"), jsonString(rom, "platform_fs_slug"))};
        if (!esdeSystem.empty() && !platformSystem.empty() && platformSystem != esdeSystem) {
            LOG(LogWarning) << "RomM: Platform of RomM ID " << placeholder->romId << " maps to \""
                            << platformSystem << "\" but the game was launched from \""
                            << esdeSystem << "\"";
        }

        const long long size {jsonInt(rom, "fs_size_bytes", 0)};
        progress.bytesTotal = size;
        progress.setStatus("Downloading " + (size > 0 ? formatBytes(size) : std::string {""}));
        progress.setDetail(serverFileName);

        const std::string contentURL {"/api/roms/" + std::to_string(placeholder->romId) +
                                      "/content/" + urlEncode(fsName)};
        const std::string directory {Utils::FileSystem::getParent(placeholderPath)};

        if (placeholder->archiveExtension.empty()) {
            // Download to a temporary file and then replace the placeholder with it.
            const std::string downloadPath {placeholderPath + ".romm"};
            if (!download(contentURL, downloadPath, &progress, error))
                return false;
            // Re-check so a file that was replaced while downloading is never overwritten.
            if (!readPlaceholder(placeholderPath)) {
                Utils::FileSystem::removeFile(downloadPath);
                error = "The placeholder file was modified during the download";
                return false;
            }
            if (!moveFile(downloadPath, placeholderPath, true)) {
                Utils::FileSystem::removeFile(downloadPath);
                error = "Couldn't replace the placeholder file \"" + placeholderPath + "\"";
                return false;
            }
        }
        else {
            const std::string archivePath {directory + "/" + serverFileName};
            if (!download(contentURL, archivePath, &progress, error))
                return false;

            progress.setStatus("Extracting");
            const std::string extractPath {placeholderPath + ".romm"};
            const std::string wantedExtension {Utils::FileSystem::getExtension(placeholderPath)};
            const bool extracted {extractFromZip(archivePath, placeholderName,
                                                 wantedExtension.empty() ? "" :
                                                                           wantedExtension.substr(1),
                                                 extractPath, &progress, error)};
            if (!extracted) {
                Utils::FileSystem::removeFile(extractPath);
                return false;
            }
            if (!moveFile(extractPath, placeholderPath, true)) {
                Utils::FileSystem::removeFile(extractPath);
                error = "Couldn't replace the placeholder file \"" + placeholderPath + "\"";
                return false;
            }
            if (Settings::getInstance()->getBool("RomMDeleteArchiveAfterExtraction") &&
                Utils::String::toLower(archivePath) != Utils::String::toLower(placeholderPath))
                Utils::FileSystem::removeFile(archivePath);
        }

        LOG(LogInfo) << "RomM: Downloaded \"" << serverFileName << "\" to \"" << placeholderPath
                     << "\"";
        return true;
    }

    // -------------------------------------------------------------------------------------
    // ZIP extraction (stored and deflate, including ZIP64).
    // -------------------------------------------------------------------------------------

#if defined(ROMM_ZIP_SUPPORT)
    namespace
    {
        uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t le32(const unsigned char* p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        uint64_t le64(const unsigned char* p)
        {
            return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
        }

        bool fileSeek(std::FILE* file, uint64_t offset)
        {
#if defined(_WIN64)
            return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
            return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        uint64_t fileSize(std::FILE* file)
        {
#if defined(_WIN64)
            _fseeki64(file, 0, SEEK_END);
            return static_cast<uint64_t>(_ftelli64(file));
#else
            fseeko(file, 0, SEEK_END);
            return static_cast<uint64_t>(ftello(file));
#endif
        }

        struct ZipEntry {
            std::string name;
            uint16_t method {0};
            uint16_t flags {0};
            uint32_t crc {0};
            uint64_t compressedSize {0};
            uint64_t uncompressedSize {0};
            uint64_t localHeaderOffset {0};
        };
    } // namespace
#endif

    bool extractFromZip(const std::string& archivePath,
                        const std::string& entryName,
                        const std::string& wantedExtension,
                        const std::string& destinationPath,
                        Progress* progress,
                        std::string& error)
    {
#if !defined(ROMM_ZIP_SUPPORT)
        error = "This build of ES-DE does not support ZIP extraction";
        return false;
#else
        std::FILE* file {openFile(archivePath, "rb")};
        if (file == nullptr) {
            error = "Couldn't open archive \"" + archivePath + "\"";
            return false;
        }
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> fileGuard {file, std::fclose};

        // Locate the end of central directory record.
        const uint64_t size {fileSize(file)};
        const uint64_t searchSize {std::min<uint64_t>(size, 65535 + 22)};
        std::vector<unsigned char> tail(static_cast<size_t>(searchSize));
        if (!fileSeek(file, size - searchSize) ||
            std::fread(tail.data(), 1, tail.size(), file) != tail.size()) {
            error = "Couldn't read archive \"" + archivePath + "\"";
            return false;
        }

        long long eocd {-1};
        for (long long i {static_cast<long long>(tail.size()) - 22}; i >= 0; --i) {
            if (le32(&tail[static_cast<size_t>(i)]) == 0x06054b50) {
                eocd = i;
                break;
            }
        }
        if (eocd < 0) {
            error = "\"" + Utils::FileSystem::getFileName(archivePath) +
                    "\" is not a ZIP archive (only ZIP extraction is supported)";
            return false;
        }

        const unsigned char* e {&tail[static_cast<size_t>(eocd)]};
        uint64_t entryCount {le16(e + 10)};
        uint64_t cdSize {le32(e + 12)};
        uint64_t cdOffset {le32(e + 16)};

        // ZIP64 end of central directory locator.
        if (eocd >= 20 && le32(&tail[static_cast<size_t>(eocd - 20)]) == 0x07064b50) {
            const uint64_t zip64EocdOffset {le64(&tail[static_cast<size_t>(eocd - 20 + 8)])};
            unsigned char z[56];
            if (fileSeek(file, zip64EocdOffset) && std::fread(z, 1, 56, file) == 56 &&
                le32(z) == 0x06064b50) {
                entryCount = le64(z + 32);
                cdSize = le64(z + 40);
                cdOffset = le64(z + 48);
            }
        }

        if (cdSize > 64 * 1024 * 1024 || cdOffset + cdSize > size) {
            error = "Invalid ZIP central directory";
            return false;
        }

        std::vector<unsigned char> cd(static_cast<size_t>(cdSize));
        if (!fileSeek(file, cdOffset) || std::fread(cd.data(), 1, cd.size(), file) != cd.size()) {
            error = "Couldn't read ZIP central directory";
            return false;
        }

        std::vector<ZipEntry> entries;
        size_t pos {0};
        for (uint64_t i {0}; i < entryCount && pos + 46 <= cd.size(); ++i) {
            const unsigned char* h {&cd[pos]};
            if (le32(h) != 0x02014b50)
                break;
            ZipEntry entry;
            entry.flags = le16(h + 8);
            entry.method = le16(h + 10);
            entry.crc = le32(h + 16);
            entry.compressedSize = le32(h + 20);
            entry.uncompressedSize = le32(h + 24);
            const uint16_t nameLength {le16(h + 28)};
            const uint16_t extraLength {le16(h + 30)};
            const uint16_t commentLength {le16(h + 32)};
            entry.localHeaderOffset = le32(h + 42);
            if (pos + 46 + nameLength + extraLength + commentLength > cd.size())
                break;
            entry.name.assign(reinterpret_cast<const char*>(h + 46), nameLength);

            // ZIP64 extended information extra field.
            const unsigned char* extra {h + 46 + nameLength};
            size_t extraPos {0};
            while (extraPos + 4 <= extraLength) {
                const uint16_t id {le16(extra + extraPos)};
                const uint16_t length {le16(extra + extraPos + 2)};
                if (id == 0x0001) {
                    const unsigned char* data {extra + extraPos + 4};
                    size_t dataPos {0};
                    if (entry.uncompressedSize == 0xFFFFFFFF && dataPos + 8 <= length) {
                        entry.uncompressedSize = le64(data + dataPos);
                        dataPos += 8;
                    }
                    if (entry.compressedSize == 0xFFFFFFFF && dataPos + 8 <= length) {
                        entry.compressedSize = le64(data + dataPos);
                        dataPos += 8;
                    }
                    if (entry.localHeaderOffset == 0xFFFFFFFF && dataPos + 8 <= length)
                        entry.localHeaderOffset = le64(data + dataPos);
                }
                extraPos += 4 + length;
            }

            pos += 46 + nameLength + extraLength + commentLength;
            if (!entry.name.empty() && entry.name.back() != '/')
                entries.emplace_back(entry);
        }

        // Find the entry to extract: an exact name match, otherwise the first entry with the
        // wanted extension.
        const ZipEntry* selected {nullptr};
        const std::string wantedName {Utils::String::toLower(entryName)};
        for (auto& entry : entries) {
            if (Utils::String::toLower(Utils::FileSystem::getFileName(entry.name)) == wantedName) {
                selected = &entry;
                break;
            }
        }
        if (selected == nullptr && !wantedExtension.empty()) {
            const std::string wantedSuffix {"." + Utils::String::toLower(wantedExtension)};
            for (auto& entry : entries) {
                const std::string lower {Utils::String::toLower(entry.name)};
                if (lower.size() > wantedSuffix.size() &&
                    lower.compare(lower.size() - wantedSuffix.size(), wantedSuffix.size(),
                                  wantedSuffix) == 0) {
                    selected = &entry;
                    break;
                }
            }
        }
        if (selected == nullptr) {
            error = "No file with the extension \"" + wantedExtension + "\" found in \"" +
                    Utils::FileSystem::getFileName(archivePath) + "\"";
            return false;
        }
        if (selected->flags & 0x0001) {
            error = "Encrypted ZIP archives are not supported";
            return false;
        }
        if (selected->method != 0 && selected->method != 8) {
            error = "Unsupported ZIP compression method " + std::to_string(selected->method);
            return false;
        }

        // Local file header.
        unsigned char local[30];
        if (!fileSeek(file, selected->localHeaderOffset) || std::fread(local, 1, 30, file) != 30 ||
            le32(local) != 0x04034b50) {
            error = "Invalid ZIP local file header";
            return false;
        }
        const uint64_t dataOffset {selected->localHeaderOffset + 30 + le16(local + 26) +
                                   le16(local + 28)};
        if (!fileSeek(file, dataOffset)) {
            error = "Invalid ZIP data offset";
            return false;
        }

        std::FILE* output {openFile(destinationPath, "wb")};
        if (output == nullptr) {
            error = "Couldn't open \"" + destinationPath + "\" for writing";
            return false;
        }
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> outputGuard {output, std::fclose};

        if (progress != nullptr) {
            progress->bytesNow = 0;
            progress->bytesTotal = static_cast<long long>(selected->uncompressedSize);
        }

        constexpr size_t chunkSize {256 * 1024};
        std::vector<unsigned char> in(chunkSize);
        std::vector<unsigned char> out(chunkSize);
        uLong crc {crc32(0L, Z_NULL, 0)};
        uint64_t remaining {selected->compressedSize};
        uint64_t written {0};

        z_stream stream {};
        if (selected->method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            error = "Couldn't initialize zlib";
            return false;
        }

        bool success {true};
        bool streamEnd {false};
        while (remaining > 0 && !streamEnd) {
            if (progress != nullptr && progress->cancel) {
                error = "Extraction cancelled";
                success = false;
                break;
            }
            const size_t toRead {static_cast<size_t>(std::min<uint64_t>(remaining, chunkSize))};
            if (std::fread(in.data(), 1, toRead, file) != toRead) {
                error = "Unexpected end of ZIP archive";
                success = false;
                break;
            }
            remaining -= toRead;

            if (selected->method == 0) {
                crc = crc32(crc, in.data(), static_cast<uInt>(toRead));
                if (std::fwrite(in.data(), 1, toRead, output) != toRead) {
                    error = "Couldn't write extracted file, disk full?";
                    success = false;
                    break;
                }
                written += toRead;
            }
            else {
                stream.next_in = in.data();
                stream.avail_in = static_cast<uInt>(toRead);
                while (stream.avail_in > 0 || !streamEnd) {
                    stream.next_out = out.data();
                    stream.avail_out = static_cast<uInt>(out.size());
                    const int result {inflate(&stream, Z_NO_FLUSH)};
                    if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
                        error = "ZIP data is corrupt";
                        success = false;
                        break;
                    }
                    const size_t produced {out.size() - stream.avail_out};
                    if (produced > 0) {
                        crc = crc32(crc, out.data(), static_cast<uInt>(produced));
                        if (std::fwrite(out.data(), 1, produced, output) != produced) {
                            error = "Couldn't write extracted file, disk full?";
                            success = false;
                            break;
                        }
                        written += produced;
                    }
                    if (result == Z_STREAM_END) {
                        streamEnd = true;
                        break;
                    }
                    if (produced == 0 && stream.avail_in == 0)
                        break;
                }
                if (!success)
                    break;
            }
            if (progress != nullptr)
                progress->bytesNow = static_cast<long long>(written);
        }

        if (selected->method == 8)
            inflateEnd(&stream);

        if (success && written != selected->uncompressedSize) {
            error = "ZIP entry size mismatch";
            success = false;
        }
        if (success && crc != selected->crc) {
            error = "ZIP entry CRC mismatch, the archive is corrupt";
            success = false;
        }
        if (std::fflush(output) != 0 && success) {
            error = "Couldn't write extracted file, disk full?";
            success = false;
        }
        return success;
#endif
    }

} // namespace RomM
