// ES-DE UWP overlay port: replacements for the EXR and RAW plugins.
//
// OpenEXR and LibRaw can't be built for UWP. These plugins only provide the format name,
// so that the formats keep their FREE_IMAGE_FORMAT IDs, but they never identify, load or
// save any image.

#include "FreeImage.h"
#include "Plugin.h"

static const char* DLL_CALLCONV FormatEXR() { return "EXR"; }
static const char* DLL_CALLCONV DescriptionEXR() { return "ILM OpenEXR (disabled)"; }
static const char* DLL_CALLCONV ExtensionEXR() { return "exr"; }

static const char* DLL_CALLCONV FormatRAW() { return "RAW"; }
static const char* DLL_CALLCONV DescriptionRAW() { return "RAW camera image (disabled)"; }
static const char* DLL_CALLCONV ExtensionRAW() { return "3fr,arw,cr2,crw,dng,nef,orf,raf,rw2"; }

void DLL_CALLCONV InitEXR(Plugin* plugin, int /*format_id*/)
{
    plugin->format_proc = FormatEXR;
    plugin->description_proc = DescriptionEXR;
    plugin->extension_proc = ExtensionEXR;
}

void DLL_CALLCONV InitRAW(Plugin* plugin, int /*format_id*/)
{
    plugin->format_proc = FormatRAW;
    plugin->description_proc = DescriptionRAW;
    plugin->extension_proc = ExtensionRAW;
}
