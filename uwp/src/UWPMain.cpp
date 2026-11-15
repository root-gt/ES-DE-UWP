//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  UWPMain.cpp
//
//  Entry point for the UWP (Xbox Dev Mode) build.
//
//  ES-DE's main() is renamed to SDL_main() by SDL_main.h on WinRT. SDL_WinRTRunApp() creates
//  the CoreApplication view, runs SDL_main() on the UI thread and handles activation events,
//  including the esde: protocol activation that emulators use to return to ES-DE.
//

#include <SDL2/SDL_main.h>

#include <windows.h>

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return SDL_WinRTRunApp(SDL_main, nullptr);
}
