//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  PlatformUtilUWP.h
//
//  Platform utility functions for the UWP (Universal Windows Platform) build, primarily
//  targeting Xbox consoles in Developer Mode.
//
//  UWP applications can't start other processes. Games are instead launched by activating
//  the emulator's URI protocol, for example:
//  retroarch:?cmd=retroarch -L cores\snes9x_libretro.dll "E:\ROMs\snes\game.sfc"&launchOnExit=esde:
//  The emulator then activates the esde: protocol when it exits, which brings ES-DE back.
//

#ifndef ES_CORE_UTILS_PLATFORM_UTIL_UWP_H
#define ES_CORE_UTILS_PLATFORM_UTIL_UWP_H

#if defined(ES_UWP)

#include <string>

namespace Utils
{
    namespace Platform
    {
        namespace UWP
        {
            // The marker that FileData puts in place of %EMULATOR_<NAME>% when the emulator
            // is found via an uwpprotocol find rule.
            inline const std::string sProtocolMarker {"uwpprotocol:"};

            // The protocol ES-DE registers in its package manifest, emulators use it to
            // return to ES-DE when they exit.
            inline const std::string sReturnProtocol {"esde:"};

            // Builds the URI from an uwpprotocol find rule template and the launch arguments.
            // The template can contain these variables:
            //   %ARGS%        - The URL-encoded launch arguments (emulator command line).
            //   %ARGSRAW%     - The launch arguments without URL encoding.
            //   %RETURNURI%   - The URL-encoded ES-DE return protocol (esde:).
            // If the template contains no %ARGS% or %ARGSRAW% variable, the URL-encoded
            // arguments are appended at the end.
            std::string buildLaunchURI(const std::string& uriTemplate,
                                       const std::string& arguments);

            // Activates the URI using Windows::System::Launcher. Returns 0 if the launch
            // request was successfully submitted.
            int launchURI(const std::string& uri);

            // True if the string starts with an URI scheme like "retroarch:" (but not with
            // a drive letter like "E:").
            bool isURIScheme(const std::string& string);

            // Percent-encoding for URI query values.
            std::string urlEncode(const std::string& string);

            // ES-DE home directory. This is E:\ (so the application data ends up in
            // E:\ES-DE as with other Xbox Dev Mode frontends) if that drive is writable,
            // otherwise the LocalState folder of the application package.
            std::string getDefaultHomePath();

            // Folder of the installed application package (for the resources directory).
            std::string getInstallationPath();

            // LocalState folder of the application package.
            std::string getLocalFolderPath();

            // True when running on an Xbox console.
            bool isXbox();

            // Exits the application.
            void exitApplication();

        } // namespace UWP

    } // namespace Platform

} // namespace Utils

#endif // ES_UWP

#endif // ES_CORE_UTILS_PLATFORM_UTIL_UWP_H
