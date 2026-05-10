cmake_minimum_required(VERSION 3.21)

project(AIAgent VERSION 2.7.2)

# Win10 NT target — required by x3daudio.h's Win8+ guard.
add_compile_definitions(
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000000
)

find_package(SkyrimScripting.CMake CONFIG REQUIRED)
find_package(SkyrimScripting.Plugin CONFIG REQUIRED)

add_skse_plugin(${PROJECT_NAME}
# Windows symlink: mklink /D "C:\Games\" "C:\Program Files (x86)"
#DATA_FOLDER "E:/SteamLibrary/steamapps/common/Skyrim Special Edition/Data"
#DATA_FOLDER "C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition/Data"
DATA_FOLDER "C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition/Data"

SOURCES Replacements.cpp Misc.cpp SPGResponse.cpp Conf.cpp Commands.cpp Papyrus.cpp  md5.cpp Voicerec.cpp HTTPUploader.cpp HerikaEyes.cpp AudioManager.cpp SpeakManager.cpp HTTPManager.cpp SpatialAwareness.cpp ThreadPool.cpp Plugin.cpp ScriptProxy.cpp PrismaUIBridge.cpp MusicManager.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE SkyrimScriptingPlugin::SkyrimScripting.Plugin)
