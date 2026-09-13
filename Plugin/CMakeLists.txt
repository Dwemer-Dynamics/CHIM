cmake_minimum_required(VERSION 3.21)

project(AIAgent VERSION 3.4.0)

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "Enable CommonLibSSE Xbyak support" FORCE)
set(COMMONLIB_PREBUILT ON CACHE BOOL "Use the pinned CommonLibSSE release build when compatible" FORCE)
add_subdirectory(lib/CommonLibSSE-NG)

find_package(SkyrimScripting.CMake CONFIG REQUIRED)

add_library(SkyrimScripting.Plugin INTERFACE)
add_library(SkyrimScriptingPlugin::SkyrimScripting.Plugin ALIAS SkyrimScripting.Plugin)
target_include_directories(SkyrimScripting.Plugin INTERFACE lib/SkyrimScripting.Plugin/include)
target_link_libraries(SkyrimScripting.Plugin INTERFACE CommonLibSSE::CommonLibSSE)

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

add_library(${PROJECT_NAME} SHARED
    ChimInteraction.cpp Replacements.cpp Misc.cpp SPGResponse.cpp Conf.cpp Commands.cpp DirectorScene.cpp Papyrus.cpp md5.cpp Voicerec.cpp
    HTTPUploader.cpp HerikaEyes.cpp AudioManager.cpp SpeakManager.cpp HTTPManager.cpp SpatialAwareness.cpp
    SpatialSnapshotManager.cpp SpatialDoorState.cpp PlayerConversationRouter.cpp ThreadPool.cpp Plugin.cpp PluginVersion.cpp ScriptProxy.cpp
    PrismaUIBridge.cpp MusicManager.cpp VRItemAwareness.cpp ResourceFileReader.cpp DynamicDiaryBook.cpp ServerPluginSync.cpp SupportReportLauncher.cpp
)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    AIAGENT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    AIAGENT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    AIAGENT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
target_skyrim_precompile_headers(${PROJECT_NAME})
target_skyrim_data_folder(${PROJECT_NAME} SKYRIM_FOLDER "${AIAGENT_SKYRIM_FOLDER}")
target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    SkyrimScriptingPlugin::SkyrimScripting.Plugin
)

include(CTest)
if(BUILD_TESTING)
    add_executable(AIAgentItemIdentifierTests tests/ItemIdentifierUtilsTests.cpp)
    target_include_directories(AIAgentItemIdentifierTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(AIAgentItemIdentifierTests PRIVATE cxx_std_23)
    add_test(NAME AIAgentItemIdentifierTests COMMAND AIAgentItemIdentifierTests)

    add_executable(AIAgentHeadVoiceVolumeTests tests/HeadVoiceVolumeUtilsTests.cpp)
    target_include_directories(AIAgentHeadVoiceVolumeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(AIAgentHeadVoiceVolumeTests PRIVATE cxx_std_23)
    add_test(NAME AIAgentHeadVoiceVolumeTests COMMAND AIAgentHeadVoiceVolumeTests)

    add_executable(AIAgentPlayerConversationRoutingPolicyTests tests/PlayerConversationRoutingPolicyTests.cpp)
    target_include_directories(AIAgentPlayerConversationRoutingPolicyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(AIAgentPlayerConversationRoutingPolicyTests PRIVATE cxx_std_23)
    add_test(NAME AIAgentPlayerConversationRoutingPolicyTests COMMAND AIAgentPlayerConversationRoutingPolicyTests)

    add_executable(AIAgentActorTargetIdentifierTests tests/ActorTargetIdentifierUtilsTests.cpp)
    target_include_directories(AIAgentActorTargetIdentifierTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(AIAgentActorTargetIdentifierTests PRIVATE cxx_std_23)
    add_test(NAME AIAgentActorTargetIdentifierTests COMMAND AIAgentActorTargetIdentifierTests)

    add_executable(AIAgentChatboxModePolicyTests tests/ChatboxModePolicyTests.cpp)
    target_include_directories(AIAgentChatboxModePolicyTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(AIAgentChatboxModePolicyTests PRIVATE cxx_std_23)
    add_test(NAME AIAgentChatboxModePolicyTests COMMAND AIAgentChatboxModePolicyTests)
endif()

