cmake_minimum_required(VERSION 3.21)

project(AIAgent VERSION 2.8.2)

find_package(SkyrimScripting.CMake CONFIG REQUIRED)
find_package(SkyrimScripting.Plugin CONFIG REQUIRED)

set(AIAGENT_SKYRIM_FOLDER_CANDIDATES
    "$ENV{CMAKE_SKYRIM_FOLDER}"
    "C:/Games/steamapps/common/Skyrim Special Edition"
    "C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition"
    "C:/Games/steamapps/common/SkyrimVR"
    "C:/Program Files (x86)/Steam/steamapps/common/SkyrimVR"
)

set(AIAGENT_SKYRIM_FOLDER "")
foreach(candidate IN LISTS AIAGENT_SKYRIM_FOLDER_CANDIDATES)
    if(candidate AND IS_DIRECTORY "${candidate}/Data")
        set(AIAGENT_SKYRIM_FOLDER "${candidate}")
        break()
    endif()
endforeach()

if(NOT AIAGENT_SKYRIM_FOLDER)
    message(FATAL_ERROR "Unable to locate a Skyrim install with a Data directory. Set CMAKE_SKYRIM_FOLDER or install Skyrim SE/VR in a known path.")
endif()

add_skse_plugin(${PROJECT_NAME}
SKYRIM_FOLDER "${AIAGENT_SKYRIM_FOLDER}"

SOURCES Replacements.cpp Misc.cpp SPGResponse.cpp Conf.cpp Commands.cpp Papyrus.cpp  md5.cpp Voicerec.cpp HTTPUploader.cpp HerikaEyes.cpp AudioManager.cpp SpeakManager.cpp HTTPManager.cpp SpatialAwareness.cpp SpatialSnapshotManager.cpp ThreadPool.cpp Plugin.cpp ScriptProxy.cpp PrismaUIBridge.cpp MusicManager.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE SkyrimScriptingPlugin::SkyrimScripting.Plugin)

