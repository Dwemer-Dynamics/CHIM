#include <SKSE/SKSE.h>

SKSEPluginVersion = []() {
    SKSE::PluginVersionData versionData;
    versionData.PluginVersion({AIAGENT_VERSION_MAJOR, AIAGENT_VERSION_MINOR, AIAGENT_VERSION_PATCH, 0});
    versionData.PluginName("AIAgent");
    versionData.AuthorName("Dwemer Dynamics");
    versionData.UsesAddressLibrary();
    versionData.UsesNoStructs();
    return versionData;
}();

SKSE_EXPORT bool SKSEPlugin_Query(SKSE::QueryInterface*, SKSE::PluginInfo* pluginInfo)
{
    pluginInfo->infoVersion = SKSE::PluginInfo::kVersion;
    pluginInfo->name = SKSEPlugin_Version.GetPluginName().data();
    pluginInfo->version = SKSEPlugin_Version.GetPluginVersion().pack();
    return true;
}
