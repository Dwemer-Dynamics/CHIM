#include "SpatialAwareness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "RE/B/BGSOpenCloseForm.h"
#include "RE/N/NavMesh.h"
#include "RE/T/TESObjectCELL.h"

namespace logger = SKSE::log;

namespace SpatialAwareness
{
    namespace
    {
        struct NavNode
        {
            RE::NiPoint3 centroid{};
            std::vector<std::pair<int, float>> edges{};
        };

        struct NavGraph
        {
            std::vector<NavNode> nodes{};
            std::unordered_map<std::uint64_t, int> triangleNodeByKey{};
        };

        enum class NavPathStatus
        {
            kUnavailable,
            kNoPath,
            kSuccess
        };

        struct NavPathQuery
        {
            NavPathStatus status = NavPathStatus::kUnavailable;
            float pathDistance = -1.0f;
        };

        std::mutex g_navCacheMutex;
        std::unordered_map<RE::FormID, std::shared_ptr<NavGraph>> g_navGraphCache;
        std::mutex g_settingsMutex;
        Settings g_settings{};
        std::mutex g_spatialEvalLogMutex;

        struct SpatialEvalLogSnapshot
        {
            std::string tier{};
            std::string reason{};
            int canCommunicate = 0;
            int volumeMilli = 0;
            int airDistanceTenths = 0;
            int pathDistanceTenths = 0;
            int pathRatioHundredths = 0;
            int navmeshPathUsed = 0;
            int navmeshPathFound = 0;
            int losFallbackUsed = 0;
            int hasLineOfSight = 0;
            int detectionLevel = 0;
            int openDoorCount = 0;
            int closedDoorCount = 0;

            bool operator==(const SpatialEvalLogSnapshot& other) const = default;
        };

        struct SpatialEvalLogState
        {
            SpatialEvalLogSnapshot snapshot{};
            std::chrono::steady_clock::time_point lastEmit{};
            bool initialized = false;
        };

        std::unordered_map<std::uint64_t, SpatialEvalLogState> g_spatialEvalLogState;
        constexpr auto kSpatialEvalLogHeartbeat = std::chrono::seconds(5);

        int QuantizeForLog(float value, float scale, int fallback)
        {
            if (!std::isfinite(value)) {
                return fallback;
            }

            return static_cast<int>(std::lround(value * scale));
        }

        std::uint64_t SpatialEvalLogKey(RE::Actor* speaker, RE::Actor* listener)
        {
            const auto speakerId = speaker ? static_cast<std::uint64_t>(speaker->GetFormID()) : 0ULL;
            const auto listenerId = listener ? static_cast<std::uint64_t>(listener->GetFormID()) : 0ULL;
            return (speakerId << 32U) | listenerId;
        }

        SpatialEvalLogSnapshot BuildSpatialEvalLogSnapshot(const char* tier, const Result& result)
        {
            SpatialEvalLogSnapshot snapshot{};
            snapshot.tier = tier ? tier : "";
            snapshot.reason = result.reason;
            snapshot.canCommunicate = result.canCommunicate ? 1 : 0;
            snapshot.volumeMilli = QuantizeForLog(result.volume, 1000.0f, -1);
            snapshot.airDistanceTenths = QuantizeForLog(result.airDistance, 10.0f, -1);
            snapshot.pathDistanceTenths = QuantizeForLog(result.pathDistance, 10.0f, -1);
            snapshot.pathRatioHundredths = QuantizeForLog(result.pathRatio, 100.0f, -100);
            snapshot.navmeshPathUsed = result.navmeshPathUsed ? 1 : 0;
            snapshot.navmeshPathFound = result.navmeshPathFound ? 1 : 0;
            snapshot.losFallbackUsed = result.losFallbackUsed ? 1 : 0;
            snapshot.hasLineOfSight = result.hasLineOfSight ? 1 : 0;
            snapshot.detectionLevel = result.detectionLevel;
            snapshot.openDoorCount = result.openDoorCount;
            snapshot.closedDoorCount = result.closedDoorCount;
            return snapshot;
        }

        bool ShouldEmitSpatialEvalLog(RE::Actor* speaker, RE::Actor* listener, const char* tier, const Result& result)
        {
            const auto snapshot = BuildSpatialEvalLogSnapshot(tier, result);
            const auto now = std::chrono::steady_clock::now();
            const auto key = SpatialEvalLogKey(speaker, listener);

            std::lock_guard<std::mutex> lock(g_spatialEvalLogMutex);
            auto& state = g_spatialEvalLogState[key];
            const bool heartbeatDue = state.initialized && (now - state.lastEmit) >= kSpatialEvalLogHeartbeat;
            const bool changed = !state.initialized || !(state.snapshot == snapshot);
            if (!changed && !heartbeatDue) {
                return false;
            }

            state.snapshot = snapshot;
            state.lastEmit = now;
            state.initialized = true;
            return true;
        }

        std::uint64_t TriangleKey(RE::FormID meshFormID, std::uint32_t triangleIndex)
        {
            return (static_cast<std::uint64_t>(meshFormID) << 32U) | static_cast<std::uint64_t>(triangleIndex);
        }

        std::uint64_t EdgeKey(RE::FormID meshFormID, std::uint16_t vertexA, std::uint16_t vertexB)
        {
            const auto low = std::min(vertexA, vertexB);
            const auto high = std::max(vertexA, vertexB);
            return (static_cast<std::uint64_t>(meshFormID) << 32U) | (static_cast<std::uint64_t>(low) << 16U) |
                   static_cast<std::uint64_t>(high);
        }

        std::string ActorLabel(RE::Actor* actor)
        {
            if (!actor) {
                return "null";
            }

            // Use GetName() (base form name, thread-safe) instead of GetDisplayFullName()
            // which accesses the extra data list and is NOT thread-safe.
            // GetDisplayFullName() crashes intermittently when called from the HTTP thread
            // while the game engine modifies actor extra data on the main thread.
            const char* name = actor->GetName();
            if (name && name[0] != '\0') {
                return name;
            }

            return std::format("form_{:08X}", actor->GetFormID());
        }

        bool IsUnavailable(RE::Actor* actor)
        {
            return !actor || actor->IsDead() || actor->IsDisabled() || actor->IsDeleted();
        }

        bool IsLinkedEdge(const RE::BSNavmeshTriangle& triangle, std::size_t edgeIndex)
        {
            switch (edgeIndex) {
            case 0:
                return triangle.triangleFlags.any(RE::BSNavmeshTriangle::TriangleFlag::kEdge0_Link);
            case 1:
                return triangle.triangleFlags.any(RE::BSNavmeshTriangle::TriangleFlag::kEdge1_Link);
            case 2:
                return triangle.triangleFlags.any(RE::BSNavmeshTriangle::TriangleFlag::kEdge2_Link);
            default:
                return false;
            }
        }

        bool TryTriangleCentroid(RE::NavMesh* navMesh, std::size_t triangleIndex, RE::NiPoint3& centroid)
        {
            if (!navMesh) {
                return false;
            }

            const auto triangleCount = navMesh->triangles.size();
            if (triangleIndex >= triangleCount) {
                return false;
            }

            const auto& triangle = navMesh->triangles[triangleIndex];
            if (triangle.triangleFlags.any(RE::BSNavmeshTriangle::TriangleFlag::kDeleted)) {
                return false;
            }

            const auto vertexCount = navMesh->vertices.size();
            const auto vertex0 = triangle.vertices[0];
            const auto vertex1 = triangle.vertices[1];
            const auto vertex2 = triangle.vertices[2];
            if (vertex0 >= vertexCount || vertex1 >= vertexCount || vertex2 >= vertexCount) {
                return false;
            }

            const RE::NiPoint3& p0 = navMesh->vertices[vertex0].location;
            const RE::NiPoint3& p1 = navMesh->vertices[vertex1].location;
            const RE::NiPoint3& p2 = navMesh->vertices[vertex2].location;
            centroid = RE::NiPoint3((p0.x + p1.x + p2.x) / 3.0f, (p0.y + p1.y + p2.y) / 3.0f,
                                    (p0.z + p1.z + p2.z) / 3.0f);
            return true;
        }

        void AddUndirectedEdge(NavGraph& graph, int nodeA, int nodeB)
        {
            if (nodeA < 0 || nodeB < 0 || nodeA == nodeB) {
                return;
            }

            if (nodeA >= static_cast<int>(graph.nodes.size()) || nodeB >= static_cast<int>(graph.nodes.size())) {
                return;
            }

            const float weight = graph.nodes[nodeA].centroid.GetDistance(graph.nodes[nodeB].centroid);
            if (!std::isfinite(weight) || weight <= 0.0f) {
                return;
            }

            graph.nodes[nodeA].edges.emplace_back(nodeB, weight);
            graph.nodes[nodeB].edges.emplace_back(nodeA, weight);
        }

        std::shared_ptr<NavGraph> BuildGraphForCell(RE::TESObjectCELL* cell)
        {
            if (!cell) {
                return nullptr;
            }

            auto* navMeshArray = cell->GetRuntimeData().navMeshes;
            if (!navMeshArray || navMeshArray->navMeshes.empty()) {
                return nullptr;
            }

            auto graph = std::make_shared<NavGraph>();

            struct MeshBuildData
            {
                RE::NavMesh* mesh = nullptr;
                RE::FormID meshFormID = 0;
                std::vector<int> triangleToNode{};
            };

            std::vector<MeshBuildData> meshes;
            meshes.reserve(navMeshArray->navMeshes.size());

            for (const auto& navMeshRef : navMeshArray->navMeshes) {
                auto* navMesh = navMeshRef.get();
                if (!navMesh) {
                    continue;
                }

                MeshBuildData meshData{};
                meshData.mesh = navMesh;
                meshData.meshFormID = navMesh->GetFormID();
                meshData.triangleToNode.assign(navMesh->triangles.size(), -1);

                for (std::size_t triIndex = 0; triIndex < navMesh->triangles.size(); ++triIndex) {
                    RE::NiPoint3 centroid{};
                    if (!TryTriangleCentroid(navMesh, triIndex, centroid)) {
                        continue;
                    }

                    const int nodeIndex = static_cast<int>(graph->nodes.size());
                    graph->nodes.push_back(NavNode{ centroid, {} });
                    meshData.triangleToNode[triIndex] = nodeIndex;
                    graph->triangleNodeByKey.emplace(
                        TriangleKey(meshData.meshFormID, static_cast<std::uint32_t>(triIndex)), nodeIndex);
                }

                meshes.push_back(std::move(meshData));
            }

            if (graph->nodes.empty()) {
                return graph;
            }

            for (const auto& meshData : meshes) {
                if (!meshData.mesh || meshData.triangleToNode.empty()) {
                    continue;
                }

                std::unordered_map<std::uint64_t, std::vector<int>> edgeToNodes;
                edgeToNodes.reserve(meshData.mesh->triangles.size() * 3U);

                for (std::size_t triIndex = 0; triIndex < meshData.mesh->triangles.size(); ++triIndex) {
                    const int nodeIndex = meshData.triangleToNode[triIndex];
                    if (nodeIndex < 0) {
                        continue;
                    }

                    const auto& triangle = meshData.mesh->triangles[triIndex];
                    edgeToNodes[EdgeKey(meshData.meshFormID, triangle.vertices[0], triangle.vertices[1])].push_back(
                        nodeIndex);
                    edgeToNodes[EdgeKey(meshData.meshFormID, triangle.vertices[1], triangle.vertices[2])].push_back(
                        nodeIndex);
                    edgeToNodes[EdgeKey(meshData.meshFormID, triangle.vertices[2], triangle.vertices[0])].push_back(
                        nodeIndex);
                }

                for (const auto& [_, nodes] : edgeToNodes) {
                    if (nodes.size() < 2U) {
                        continue;
                    }

                    for (std::size_t i = 1; i < nodes.size(); ++i) {
                        AddUndirectedEdge(*graph, nodes[i - 1], nodes[i]);
                    }
                }
            }

            // Conservative cross-mesh portal stitching:
            // Assume extraEdgeInfo entries correspond to non-local linked edges in triangle scan order.
            for (const auto& meshData : meshes) {
                if (!meshData.mesh || meshData.triangleToNode.empty()) {
                    continue;
                }

                std::size_t extraEdgeInfoIndex = 0;
                const auto triangleCount = meshData.mesh->triangles.size();

                for (std::size_t triIndex = 0; triIndex < triangleCount; ++triIndex) {
                    const int localNode = meshData.triangleToNode[triIndex];
                    if (localNode < 0) {
                        continue;
                    }

                    const auto& triangle = meshData.mesh->triangles[triIndex];
                    for (std::size_t edgeIndex = 0; edgeIndex < 3U; ++edgeIndex) {
                        if (!IsLinkedEdge(triangle, edgeIndex)) {
                            continue;
                        }

                        const auto linkedTriangle = triangle.triangles[edgeIndex];
                        const bool hasLocalNeighbor =
                            linkedTriangle < triangleCount && meshData.triangleToNode[linkedTriangle] >= 0;
                        if (hasLocalNeighbor) {
                            continue;
                        }

                        if (extraEdgeInfoIndex >= meshData.mesh->extraEdgeInfo.size()) {
                            continue;
                        }

                        const auto& extraInfo = meshData.mesh->extraEdgeInfo[extraEdgeInfoIndex++];
                        if (extraInfo.type != RE::EDGE_EXTRA_INFO_TYPE::kPortal) {
                            continue;
                        }

                        const auto otherIt = graph->triangleNodeByKey.find(
                            TriangleKey(extraInfo.portal.otherMeshID, static_cast<std::uint32_t>(extraInfo.portal.triangle)));
                        if (otherIt == graph->triangleNodeByKey.end()) {
                            continue;
                        }

                        AddUndirectedEdge(*graph, localNode, otherIt->second);
                    }
                }

                if (extraEdgeInfoIndex != meshData.mesh->extraEdgeInfo.size()) {
                    logger::debug(
                        "[SPATIAL_V1L] navmesh portal mapping partially consumed for mesh {:08X} ({}/{})",
                        meshData.meshFormID, extraEdgeInfoIndex, meshData.mesh->extraEdgeInfo.size());
                }
            }

            return graph;
        }

        std::shared_ptr<NavGraph> GetGraphForCell(RE::TESObjectCELL* cell)
        {
            if (!cell) {
                return nullptr;
            }

            // Verify cell is still attached (loaded by the engine).
            // Unloaded cells have stale navmesh data that will crash if accessed.
            if (!cell->IsAttached()) {
                logger::debug("[SPATIAL] Cell {} not attached, skipping navmesh", cell->GetFormID());
                return nullptr;
            }

            const RE::FormID cellId = cell->GetFormID();

            {
                std::lock_guard<std::mutex> lock(g_navCacheMutex);
                if (const auto it = g_navGraphCache.find(cellId); it != g_navGraphCache.end()) {
                    return it->second;
                }
            }

            std::shared_ptr<NavGraph> graph = BuildGraphForCell(cell);

            {
                std::lock_guard<std::mutex> lock(g_navCacheMutex);
                // Evict cells that are no longer loaded by the engine.
                // Interiors: only the current cell stays after a load screen.
                // Exteriors: neighboring cells in the loaded grid stay cached.
                // This prevents stale data while keeping active cells fast.
                for (auto it = g_navGraphCache.begin(); it != g_navGraphCache.end(); ) {
                    auto* cachedCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(it->first);
                    if (!cachedCell || !cachedCell->IsAttached()) {
                        it = g_navGraphCache.erase(it);
                    } else {
                        ++it;
                    }
                }
                g_navGraphCache[cellId] = graph;
            }

            return graph;
        }

        int FindNearestNode(const NavGraph& graph, const RE::NiPoint3& point, float maxSnapDistance)
        {
            int nearestNode = -1;
            float bestDistance = maxSnapDistance;
            for (int i = 0; i < static_cast<int>(graph.nodes.size()); ++i) {
                const float distance = graph.nodes[i].centroid.GetDistance(point);
                if (!std::isfinite(distance)) {
                    continue;
                }

                if (distance <= bestDistance) {
                    bestDistance = distance;
                    nearestNode = i;
                }
            }
            return nearestNode;
        }

        NavPathQuery QueryNavPathDistance(RE::TESObjectCELL* cell, const RE::NiPoint3& speakerPosition,
                                          const RE::NiPoint3& listenerPosition, const Settings& settings)
        {
            const auto graph = GetGraphForCell(cell);
            if (!graph || graph->nodes.empty()) {
                return {};
            }

            const int startNode = FindNearestNode(*graph, speakerPosition, settings.navmeshSnapDistance);
            const int goalNode = FindNearestNode(*graph, listenerPosition, settings.navmeshSnapDistance);
            if (startNode < 0 || goalNode < 0) {
                return {};
            }

            NavPathQuery query{};
            query.status = NavPathStatus::kNoPath;

            const float speakerToStart = speakerPosition.GetDistance(graph->nodes[startNode].centroid);
            const float listenerToGoal = listenerPosition.GetDistance(graph->nodes[goalNode].centroid);
            if (!std::isfinite(speakerToStart) || !std::isfinite(listenerToGoal)) {
                return query;
            }

            if (startNode == goalNode) {
                query.status = NavPathStatus::kSuccess;
                query.pathDistance = speakerToStart + listenerToGoal;
                return query;
            }

            const std::size_t nodeCount = graph->nodes.size();
            std::vector<float> bestCost(nodeCount, std::numeric_limits<float>::infinity());
            using QueueItem = std::pair<float, int>;
            std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> openSet;

            bestCost[startNode] = 0.0f;
            openSet.emplace(0.0f, startNode);

            while (!openSet.empty()) {
                const auto [cost, node] = openSet.top();
                openSet.pop();

                if (cost > bestCost[node]) {
                    continue;
                }

                if (node == goalNode) {
                    query.status = NavPathStatus::kSuccess;
                    query.pathDistance = speakerToStart + cost + listenerToGoal;
                    return query;
                }

                for (const auto& [neighbor, edgeWeight] : graph->nodes[node].edges) {
                    if (neighbor < 0 || neighbor >= static_cast<int>(nodeCount) || !std::isfinite(edgeWeight) ||
                        edgeWeight <= 0.0f) {
                        continue;
                    }

                    const float nextCost = cost + edgeWeight;
                    if (nextCost < bestCost[neighbor]) {
                        bestCost[neighbor] = nextCost;
                        openSet.emplace(nextCost, neighbor);
                    }
                }
            }

            return query;
        }

        float GetTriangulationTolerance(float airDistance, const Settings& settings)
        {
            const float scaledTolerance = airDistance * settings.doorTriangulationPercentTolerance;
            return std::max(settings.doorTriangulationAbsoluteTolerance, scaledTolerance);
        }

        bool IsBetweenActors(const RE::NiPoint3& speakerPosition, const RE::NiPoint3& listenerPosition,
                             const RE::NiPoint3& candidatePosition, float airDistance, const Settings& settings)
        {
            const float speakerToCandidate = speakerPosition.GetDistance(candidatePosition);
            const float listenerToCandidate = listenerPosition.GetDistance(candidatePosition);
            const float combinedDistance = speakerToCandidate + listenerToCandidate;
            const float tolerance = GetTriangulationTolerance(airDistance, settings);
            return combinedDistance >= (airDistance - tolerance) && combinedDistance <= (airDistance + tolerance);
        }

        bool IsClosedDoorState(RE::BGSOpenCloseForm::OPEN_STATE state)
        {
            return state == RE::BGSOpenCloseForm::OPEN_STATE::kClosed ||
                   state == RE::BGSOpenCloseForm::OPEN_STATE::kClosing;
        }

        bool IsOpenDoorState(RE::BGSOpenCloseForm::OPEN_STATE state)
        {
            return state == RE::BGSOpenCloseForm::OPEN_STATE::kOpen ||
                   state == RE::BGSOpenCloseForm::OPEN_STATE::kOpening;
        }
    }  // namespace

    Settings GetSettings()
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        return g_settings;
    }

    void SetInteriorMaxDistance(float interiorMaxDistance)
    {
        if (!std::isfinite(interiorMaxDistance) || interiorMaxDistance <= 0.0f) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings.interiorMaxDistance = interiorMaxDistance;
        logger::info("[SPATIAL_V1L] interiorMaxDistance set to {:.1f}", interiorMaxDistance);
    }

    void SetExteriorMaxDistance(float exteriorMaxDistance)
    {
        if (!std::isfinite(exteriorMaxDistance) || exteriorMaxDistance <= 0.0f) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings.exteriorMaxDistance = exteriorMaxDistance;
        logger::info("[SPATIAL_V1L] exteriorMaxDistance set to {:.1f}", exteriorMaxDistance);
    }

    PathResult EvaluatePath(RE::Actor* speaker, RE::Actor* listener, const Settings& settings)
    {
        PathResult result{};

        if (!speaker || !listener) {
            return result;
        }

        if (speaker->GetFormID() == listener->GetFormID()) {
            result.status = PathStatus::kSuccess;
            result.airDistance = 0.0f;
            result.pathDistance = 0.0f;
            return result;
        }

        if (IsUnavailable(speaker) || IsUnavailable(listener)) {
            return result;
        }

        auto* speakerCell = speaker->GetParentCell();
        auto* listenerCell = listener->GetParentCell();
        if (!speakerCell || !listenerCell) {
            return result;
        }

        const bool speakerInterior = speakerCell->IsInteriorCell();
        const bool listenerInterior = listenerCell->IsInteriorCell();
        if (speakerInterior != listenerInterior) {
            return result;
        }

        if (speakerInterior && speakerCell != listenerCell) {
            return result;
        }

        const RE::NiPoint3 speakerPosition = speaker->GetPosition();
        const RE::NiPoint3 listenerPosition = listener->GetPosition();
        const float airDistance = speakerPosition.GetDistance(listenerPosition);
        if (!std::isfinite(airDistance)) {
            return result;
        }

        result.airDistance = airDistance;
        if (airDistance > settings.maxAirDistance) {
            return result;
        }

        const auto navPath = QueryNavPathDistance(speakerCell, speakerPosition, listenerPosition, settings);
        if (navPath.status == NavPathStatus::kNoPath) {
            result.status = PathStatus::kNoPath;
            return result;
        }

        if (navPath.status == NavPathStatus::kSuccess && std::isfinite(navPath.pathDistance) &&
            navPath.pathDistance >= 0.0f) {
            result.status = PathStatus::kSuccess;
            result.pathDistance = navPath.pathDistance;
        }

        return result;
    }

    Result Evaluate(RE::Actor* speaker, RE::Actor* listener, const Settings& settings)
    {
        Result result{};
        const std::string speakerName = ActorLabel(speaker);
        const std::string listenerName = ActorLabel(listener);
        const auto finalize = [&](const char* tier) -> Result {
            if (ShouldEmitSpatialEvalLog(speaker, listener, tier, result)) {
                logger::info(
                    "[SPATIAL_V1L] {} -> {} | tier={} | reason={} | can={} | volume={:.3f} | distance={:.1f} | pathDist={:.1f} | pathRatio={:.2f} | pathUsed={} | pathFound={} | losFallback={} | los={} | detect={} | openDoors={} | closedDoors={}",
                    speakerName, listenerName, tier, result.reason, result.canCommunicate ? 1 : 0, result.volume,
                    result.airDistance, result.pathDistance, result.pathRatio, result.navmeshPathUsed ? 1 : 0,
                    result.navmeshPathFound ? 1 : 0, result.losFallbackUsed ? 1 : 0, result.hasLineOfSight ? 1 : 0,
                    result.detectionLevel, result.openDoorCount, result.closedDoorCount);
            }
            return result;
        };

        if (!speaker || !listener) {
            result.reason = "invalid_actor";
            return finalize("tier0_invalid");
        }

        if (speaker->GetFormID() == listener->GetFormID()) {
            result.reason = "same_actor";
            return finalize("tier0_same_actor");
        }

        if (IsUnavailable(speaker)) {
            result.reason = "speaker_unavailable";
            return finalize("tier0_speaker_state");
        }

        if (IsUnavailable(listener)) {
            result.reason = "listener_unavailable";
            return finalize("tier0_listener_state");
        }

        auto* speakerCell = speaker->GetParentCell();
        auto* listenerCell = listener->GetParentCell();
        if (!speakerCell || !listenerCell) {
            result.reason = "missing_cell";
            return finalize("tier0_missing_cell");
        }

        const bool speakerInterior = speakerCell->IsInteriorCell();
        const bool listenerInterior = listenerCell->IsInteriorCell();
        if (speakerInterior != listenerInterior) {
            result.reason = "interior_exterior_boundary";
            return finalize("tier0_worldspace_boundary");
        }

        if (speakerInterior && speakerCell != listenerCell) {
            result.reason = "different_interior_cells";
            return finalize("tier0_cell_boundary");
        }

        const RE::NiPoint3 speakerPosition = speaker->GetPosition();
        const RE::NiPoint3 listenerPosition = listener->GetPosition();
        const float airDistance = speakerPosition.GetDistance(listenerPosition);

        if (!std::isfinite(airDistance)) {
            result.reason = "invalid_distance";
            return finalize("tier1_invalid_distance");
        }

        result.airDistance = airDistance;

        if (airDistance > settings.maxAirDistance) {
            result.reason = "too_far";
            return finalize("tier1_too_far");
        }

        int openDoorCount = 0;
        int closedDoorCount = 0;

        const float scanRadius =
            airDistance + std::max(settings.doorTriangulationAbsoluteTolerance,
                                   airDistance * settings.doorTriangulationPercentTolerance);

        // Exterior worldspaces contain many load doors that do not form meaningful
        // audio barriers between outdoor actors. Restrict door triangulation to
        // same-cell interior conversations.
        if (speakerInterior) {
            speakerCell->ForEachReferenceInRange(
                speakerPosition, scanRadius, [&](RE::TESObjectREFR& reference) {
                    const auto* baseObject = reference.GetBaseObject();
                    if (!baseObject || baseObject->GetFormType() != RE::FormType::Door) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }

                    if (!IsBetweenActors(speakerPosition, listenerPosition, reference.GetPosition(), airDistance, settings)) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }

                    const auto openState = RE::BGSOpenCloseForm::GetOpenState(&reference);
                    if (IsClosedDoorState(openState)) {
                        ++closedDoorCount;
                        return RE::BSContainer::ForEachResult::kStop;
                    }

                    if (IsOpenDoorState(openState)) {
                        ++openDoorCount;
                    }

                    return RE::BSContainer::ForEachResult::kContinue;
                });
        }

        result.openDoorCount = openDoorCount;
        result.closedDoorCount = closedDoorCount;

        if (closedDoorCount > 0) {
            result.reason = "closed_door_between";
            return finalize("tier3_closed_door");
        }

        // Keep the instant-pass fast path, but only when there is no detected door boundary.
        // If an open door is between actors, continue through volume shaping for muffled-around-corner behavior.
        if (airDistance <= settings.immediateDistance && openDoorCount == 0) {
            result.canCommunicate = true;
            result.volume = 1.0f;
            result.reason = "immediate_proximity";
            return finalize("tier1_immediate_pass");
        }

        auto evaluateLosFallback = [&](const char* blockedReason, const char* blockedTier) -> bool {
            bool hasLineOfSight = false;
            const bool losQueryOk = speaker->HasLineOfSight(listener->AsReference(), hasLineOfSight);
            result.hasLineOfSight = losQueryOk && hasLineOfSight;
            result.losFallbackUsed = true;
            if (result.hasLineOfSight) {
                result.reason = std::string(blockedReason) + "_los_recovered";
                return true;
            }

            result.reason = blockedReason;
            (void) finalize(blockedTier);
            return false;
        };

        const auto navPath = QueryNavPathDistance(speakerCell, speakerPosition, listenerPosition, settings);
        if (navPath.status == NavPathStatus::kNoPath) {
            result.navmeshPathUsed = true;
            result.navmeshPathFound = false;
            if (!evaluateLosFallback("navmesh_no_path", "tier2_navmesh_no_path")) {
                return result;
            }
        }

        if (navPath.status == NavPathStatus::kSuccess) {
            result.navmeshPathUsed = true;
            result.navmeshPathFound = true;
            result.pathDistance = navPath.pathDistance;

            if (airDistance > 0.001f && std::isfinite(navPath.pathDistance) && navPath.pathDistance >= 0.0f) {
                result.pathRatio = navPath.pathDistance / airDistance;

                if (result.pathRatio >= settings.pathRatioReject) {
                    if (!evaluateLosFallback("path_ratio_blocked", "tier2_ratio_blocked")) {
                        return result;
                    }
                }

                if (result.pathRatio >= settings.pathRatioDistanceReject &&
                    airDistance >= settings.pathRatioDistanceRejectMinAir) {
                    if (!evaluateLosFallback("path_ratio_distance_blocked", "tier2_ratio_distance_blocked")) {
                        return result;
                    }
                }
            }
        }

        // LOS is intentionally not used for hearing checks in this mode.
        // 360-degree audibility is determined by world/cell gating, door barriers, navmesh pathing, and distance volume.
        const bool aroundCornerPass = openDoorCount > 0;
        result.reason = aroundCornerPass ? "open_door_muffled" : "distance_navmesh_clear";

        const float maxDistance = speakerInterior ? settings.interiorMaxDistance : settings.exteriorMaxDistance;
        const float distanceFactor = std::clamp(1.0f - (airDistance / std::max(maxDistance, 1.0f)),
                                                settings.minDistanceFactor, 1.0f);
        const float environmentModifier = speakerInterior ? settings.interiorBaseModifier : settings.exteriorBaseModifier;
        const float openDoorModifier = std::pow(settings.openDoorPenaltyBase, static_cast<float>(openDoorCount));
        const float cornerModifier = aroundCornerPass ? settings.aroundCornerPenalty : 1.0f;
        float pathModifier = 1.0f;
        if (!result.losFallbackUsed && result.navmeshPathFound && result.pathRatio > settings.pathComplexityStartRatio) {
            pathModifier = std::clamp(1.0f / std::max(result.pathRatio * settings.pathComplexityScale, 0.01f),
                                      settings.pathComplexityMin, 1.0f);
        }

        const float finalVolume =
            std::clamp(distanceFactor * environmentModifier * openDoorModifier * cornerModifier * pathModifier, 0.0f,
                       1.0f);
        result.volume = finalVolume;

        if (finalVolume < settings.minimumAudibleVolume) {
            result.reason = "too_quiet";
            return finalize("tier8_too_quiet");
        }

        result.canCommunicate = true;
        return finalize("tier8_pass");
    }
}
