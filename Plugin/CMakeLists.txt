cmake_minimum_required(VERSION 3.21)

project(AIAgent VERSION 3.2.2)

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

SOURCES Replacements.cpp Misc.cpp SPGResponse.cpp Conf.cpp Commands.cpp Papyrus.cpp  md5.cpp Voicerec.cpp HTTPUploader.cpp HerikaEyes.cpp AudioManager.cpp SpeakManager.cpp HTTPManager.cpp SpatialAwareness.cpp SpatialSnapshotManager.cpp PlayerConversationRouter.cpp ThreadPool.cpp Plugin.cpp ScriptProxy.cpp PrismaUIBridge.cpp MusicManager.cpp VRItemAwareness.cpp ResourceFileReader.cpp DynamicDiaryBook.cpp ServerPluginSync.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE SkyrimScriptingPlugin::SkyrimScripting.Plugin)

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

