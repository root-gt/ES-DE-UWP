//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  PlatformUtilUWP.cpp
//
//  Platform utility functions for the UWP (Universal Windows Platform) build, primarily
//  targeting Xbox consoles in Developer Mode.
//
//  The WinRT parts use C++/WinRT, which is included with the Windows SDK. Define
//  ES_UWP_NO_WINRT to build only the portable parts (used for testing on other platforms).
//

#if defined(ES_UWP)

#include "utils/PlatformUtilUWP.h"

#include "Log.h"
#include "utils/StringUtil.h"

#include <iomanip>
#include <sstream>

#if !defined(ES_UWP_NO_WINRT)
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.h>

#include <cstdio>
#endif

namespace Utils
{
    namespace Platform
    {
        namespace UWP
        {
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

            bool isURIScheme(const std::string& string)
            {
                const size_t colonPos {string.find(':')};
                // Single letters are drive letters.
                if (colonPos == std::string::npos || colonPos < 2)
                    return false;
                if (!std::isalpha(static_cast<unsigned char>(string.front())))
                    return false;
                for (size_t i {1}; i < colonPos; ++i) {
                    const unsigned char c {static_cast<unsigned char>(string[i])};
                    if (!std::isalnum(c) && c != '+' && c != '-' && c != '.')
                        return false;
                }
                return true;
            }

            std::string buildLaunchURI(const std::string& uriTemplate,
                                       const std::string& arguments)
            {
                std::string uri {uriTemplate};
                const std::string trimmedArguments {Utils::String::trim(arguments)};
                const bool hasArgs {uri.find("%ARGS%") != std::string::npos ||
                                    uri.find("%ARGSRAW%") != std::string::npos};

                uri = Utils::String::replace(uri, "%RETURNURI%", urlEncode(sReturnProtocol));

                if (hasArgs) {
                    // The emulator's own command (e.g. "retroarch") is part of the template,
                    // so a space is needed between it and the arguments. That space is part
                    // of the template as well, so remove it again if there are no arguments.
                    if (trimmedArguments.empty()) {
                        uri = Utils::String::replace(uri, " %ARGS%", "");
                        uri = Utils::String::replace(uri, "%20%ARGS%", "");
                        uri = Utils::String::replace(uri, " %ARGSRAW%", "");
                    }
                    uri = Utils::String::replace(uri, "%ARGSRAW%", trimmedArguments);
                    uri = Utils::String::replace(uri, "%ARGS%", urlEncode(trimmedArguments));
                }
                else if (!trimmedArguments.empty()) {
                    uri.append(urlEncode(trimmedArguments));
                }

                // Spaces between the command and %ARGS% in the template must be encoded too.
                uri = Utils::String::replace(uri, " ", "%20");
                return uri;
            }

#if !defined(ES_UWP_NO_WINRT)

            int launchURI(const std::string& uri)
            {
                LOG(LogInfo) << "Launching URI: " << uri;
                try {
                    const winrt::Windows::Foundation::Uri launchUri {winrt::to_hstring(uri)};

                    winrt::Windows::System::LauncherOptions options;
                    // Don't show the "which app do you want to use" dialog, just fail if there
                    // is no app registered for the protocol.
                    options.DisplayApplicationPicker(false);

                    auto operation {winrt::Windows::System::Launcher::LaunchUriAsync(launchUri,
                                                                                    options)};
                    // This runs on the UI thread so blocking with get() is not permitted,
                    // the result is only logged.
                    operation.Completed(
                        [uri](const winrt::Windows::Foundation::IAsyncOperation<bool>& result,
                              const winrt::Windows::Foundation::AsyncStatus status) {
                            bool launched {false};
                            if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
                                launched = result.GetResults();
                            if (!launched) {
                                LOG(LogError)
                                    << "Couldn't launch \"" << uri
                                    << "\", is the emulator installed and does it support "
                                       "protocol activation?";
                            }
                        });
                    return 0;
                }
                catch (const winrt::hresult_error& error) {
                    LOG(LogError) << "Couldn't launch URI \"" << uri
                                  << "\": " << winrt::to_string(error.message());
                    return 1;
                }
            }

            std::string getLocalFolderPath()
            {
                try {
                    return Utils::String::replace(
                        winrt::to_string(winrt::Windows::Storage::ApplicationData::Current()
                                             .LocalFolder()
                                             .Path()),
                        "\\", "/");
                }
                catch (...) {
                    return "";
                }
            }

            std::string getInstallationPath()
            {
                try {
                    return Utils::String::replace(
                        winrt::to_string(winrt::Windows::ApplicationModel::Package::Current()
                                             .InstalledLocation()
                                             .Path()),
                        "\\", "/");
                }
                catch (...) {
                    return "";
                }
            }

            std::string getDefaultHomePath()
            {
                // Check that E: is writable by creating and removing a test file.
                const wchar_t* testFile {L"E:\\.es-de-write-test"};
                FILE* file {nullptr};
                if (_wfopen_s(&file, testFile, L"wb") == 0 && file != nullptr) {
                    fclose(file);
                    _wremove(testFile);
                    return "E:";
                }
                return getLocalFolderPath();
            }

            bool isXbox()
            {
                try {
                    return winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo()
                               .DeviceFamily() == L"Windows.Xbox";
                }
                catch (...) {
                    return false;
                }
            }

            void exitApplication()
            {
                winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
            }

#endif // !ES_UWP_NO_WINRT

        } // namespace UWP

    } // namespace Platform

} // namespace Utils

#endif // ES_UWP
