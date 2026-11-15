//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  UWPProtocolTests.cpp
//
//  Tests for the URI protocol launching used by the UWP (Xbox Dev Mode) build. This builds
//  the portable parts of PlatformUtilUWP.cpp (ES_UWP_NO_WINRT) so it runs on any platform.
//
//  Every uwpprotocol rule in resources/systems/uwp/es_find_rules.xml is expanded with
//  real-world arguments and the result is decoded the same way RetroArch UWP does it
//  (Windows.Foundation.WwwFormUrlDecoder), which must give back the original command line.
//
//  Usage: es-de-uwp-protocol-tests <path to resources/systems/uwp/es_find_rules.xml>
//

#include "Log.h"
#include "utils/PlatformUtilUWP.h"

#include <pugixml.hpp>

#include <iostream>
#include <map>
#include <string>

// Minimal implementations of the symbols PlatformUtilUWP.cpp and StringUtil.cpp need.
LogLevel Log::getReportingLevel() { return LogError; }
std::ostringstream& Log::get(LogLevel) { return mOutStringStream; }
Log::~Log() {}
namespace Utils
{
    namespace Platform
    {
        void emergencyShutdown() {}
    } // namespace Platform
} // namespace Utils

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

    // Decoding as done by WwwFormUrlDecoder: '+' is a space and %XX is a byte.
    std::string formDecode(const std::string& string)
    {
        std::string result;
        for (size_t i {0}; i < string.size(); ++i) {
            if (string[i] == '+') {
                result.push_back(' ');
            }
            else if (string[i] == '%' && i + 2 < string.size()) {
                result.push_back(static_cast<char>(std::stoi(string.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            }
            else {
                result.push_back(string[i]);
            }
        }
        return result;
    }

    // Splits "scheme:?a=1&b=2" into its decoded query parameters.
    std::map<std::string, std::string> parseQuery(const std::string& uri)
    {
        std::map<std::string, std::string> parameters;
        const size_t queryStart {uri.find('?')};
        if (queryStart == std::string::npos)
            return parameters;
        std::string query {uri.substr(queryStart + 1)};
        size_t position {0};
        while (position <= query.size()) {
            size_t end {query.find('&', position)};
            if (end == std::string::npos)
                end = query.size();
            const std::string pair {query.substr(position, end - position)};
            const size_t equals {pair.find('=')};
            if (equals != std::string::npos)
                parameters[formDecode(pair.substr(0, equals))] = formDecode(pair.substr(equals + 1));
            position = end + 1;
        }
        return parameters;
    }

    bool isValidURI(const std::string& uri)
    {
        if (!Utils::Platform::UWP::isURIScheme(uri))
            return false;
        // No characters that are invalid in a URI may remain unencoded.
        for (unsigned char c : uri) {
            if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' ||
                c == '^' || c == '`' || c == '{' || c == '|' || c == '}')
                return false;
        }
        return true;
    }
} // namespace

int main(int argc, char* argv[])
{
    using namespace Utils::Platform::UWP;

    std::cout << "== RetroArch" << std::endl;
    const std::string retroarchTemplate {
        "retroarch:?cmd=retroarch %ARGS%&launchOnExit=%RETURNURI%"};
    const std::string arguments {
        R"(-L cores\snes9x_libretro.dll "E:\ROMs\snes\Super Metroid (USA) & Co + 100%.sfc")"};
    const std::string uri {buildLaunchURI(retroarchTemplate, arguments)};
    std::cout << "   " << uri << std::endl;
    CHECK(isValidURI(uri));
    auto query = parseQuery(uri);
    CHECK(query["cmd"] == "retroarch " + arguments);
    CHECK(query["launchOnExit"] == "esde:");

    const std::string noArguments {buildLaunchURI(retroarchTemplate, "  ")};
    std::cout << "   " << noArguments << std::endl;
    CHECK(parseQuery(noArguments)["cmd"] == "retroarch");

    std::cout << "== Helpers" << std::endl;
    CHECK(urlEncode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
    CHECK(isURIScheme("retroarch:?cmd=x"));
    CHECK(isURIScheme("ms-settings:"));
    CHECK(!isURIScheme("E:\\ROMs\\game.sfc"));
    CHECK(!isURIScheme("C:/Games"));
    CHECK(!isURIScheme("retroarch"));
    CHECK(buildLaunchURI("xboxgamelist:", "9NBLGGH4PNC7") == "xboxgamelist:9NBLGGH4PNC7");

    if (argc > 1) {
        std::cout << "== Find rules (" << argv[1] << ")" << std::endl;
        pugi::xml_document document;
        CHECK(document.load_file(argv[1]));
        int rules {0};
        for (pugi::xml_node emulator : document.child("ruleList").children("emulator")) {
            for (pugi::xml_node rule : emulator.children("rule")) {
                if (std::string {rule.attribute("type").as_string()} != "uwpprotocol")
                    continue;
                for (pugi::xml_node entry : rule.children("entry")) {
                    ++rules;
                    const std::string ruleTemplate {entry.text().get()};
                    const std::string ruleURI {buildLaunchURI(ruleTemplate, arguments)};
                    std::cout << "   " << emulator.attribute("name").as_string() << ": "
                              << ruleURI.substr(0, ruleURI.find('?')) << std::endl;
                    CHECK(isValidURI(ruleURI));
                    if (ruleTemplate.find("?cmd=") != std::string::npos) {
                        query = parseQuery(ruleURI);
                        const std::string& command {query["cmd"]};
                        CHECK(command.size() > arguments.size() &&
                              command.compare(command.size() - arguments.size(),
                                              arguments.size(), arguments) == 0);
                        if (ruleTemplate.find("%RETURNURI%") != std::string::npos)
                            CHECK(query["launchOnExit"] == "esde:");
                    }
                }
            }
        }
        CHECK(rules >= 10);
    }

    std::cout << std::endl
              << (sChecks - sFailures) << "/" << sChecks << " checks passed" << std::endl;
    return sFailures == 0 ? 0 : 1;
}
