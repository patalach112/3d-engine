#include "Scene/Systems/RenderSystem.h"

#include "Core/Jobs/JobSystem.h"
#include "Core/Logger.h"
#include "Graphics/Frustum.h"
#include "Graphics/Swapchain.h"
#include "Scene/Scene.h"

#include <optional>

namespace engine {
namespace {

struct MeshWork {
    const TransformComponent* transform;
    const MeshComponent* mesh;
};

// Evaluates one entity: model matrix, world bounds, frustum test.
// Writes into a disjoint slot, so it is safe to run concurrently.
void evaluateMesh(const MeshWork& work, const Frustum& frustum, bool cullingEnabled,
                  std::optional<DrawItem>& outItem, char& outCulled) {
    const glm::mat4 model = work.transform->modelMatrix();
    outCulled = 0;
    if (cullingEnabled) {
        const AABB worldBounds = work.mesh->localBounds.transformed(model);
        if (!frustum.intersects(worldBounds)) {
            outCulled = 1;
            return;
        }
    }
    outItem = DrawItem{model};
}

} // namespace

RenderSystem::RenderSystem(Renderer& renderer, const Swapchain& swapchain, JobSystem* jobs,
                           bool frustumCulling, size_t parallelMinItems)
    : m_renderer(renderer), m_swapchain(swapchain), m_jobs(jobs), m_frustumCulling(frustumCulling),
      m_parallelMinItems(parallelMinItems) {}

PreparedFrame RenderSystem::prepareFrame(Scene& scene, float aspectRatio,
                                         const FramePrepareOptions& options) {
    PreparedFrame frame;

    const CameraComponent* activeCamera = scene.findActiveCamera();
    if (activeCamera == nullptr) {
        return frame;
    }
    frame.hasCamera = true;
    frame.view = activeCamera->camera.viewMatrix();
    frame.projection = activeCamera->camera.projectionMatrix(aspectRatio);

    std::vector<MeshWork> work;
    scene.registry().view<TransformComponent, MeshComponent>().each(
        [&work](const TransformComponent& transform, const MeshComponent& mesh) {
            if (mesh.visible) {
                work.push_back(MeshWork{&transform, &mesh});
            }
        });
    frame.meshCount = static_cast<uint32_t>(work.size());

    const Frustum frustum{frame.projection * frame.view};
    std::vector<std::optional<DrawItem>> evaluated(work.size());
    std::vector<char> culledFlags(work.size(), 0); // not vector<bool>: slots are written concurrently

    const auto evaluateRange = [&work, &frustum, &options, &evaluated, &culledFlags](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            evaluateMesh(work[i], frustum, options.frustumCulling, evaluated[i], culledFlags[i]);
        }
    };

    if (options.jobs != nullptr && work.size() >= options.parallelMinItems) {
        // parallelFor returning is the synchronization barrier: every slot
        // below is fully written before compaction reads it.
        options.jobs->parallelFor(work.size(), 0, evaluateRange);
    } else {
        evaluateRange(0, work.size());
    }

    // Sequential compaction keeps draw order deterministic.
    frame.drawItems.reserve(work.size());
    for (size_t i = 0; i < work.size(); ++i) {
        if (culledFlags[i] != 0) {
            ++frame.culledCount;
        } else if (evaluated[i].has_value()) {
            frame.drawItems.push_back(*evaluated[i]);
        }
    }
    return frame;
}

void RenderSystem::render(Scene& scene) {
    const VkExtent2D extent = m_swapchain.extent();
    const float aspectRatio = extent.height > 0
                                  ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                                  : 1.0f;

    FramePrepareOptions options;
    options.jobs = m_jobs;
    options.frustumCulling = m_frustumCulling;
    options.parallelMinItems = m_parallelMinItems;

    const PreparedFrame frame = prepareFrame(scene, aspectRatio, options);
    if (!frame.hasCamera) {
        if (!m_warnedNoCamera) {
            m_warnedNoCamera = true;
            ENGINE_LOG_WARNING("RenderSystem: scene has no active camera; skipping rendering "
                               "until one is added");
        }
        m_lastDrawnCount = 0;
        m_lastCulledCount = 0;
        return;
    }
    m_warnedNoCamera = false;
    m_lastDrawnCount = static_cast<uint32_t>(frame.drawItems.size());
    m_lastCulledCount = frame.culledCount;

    m_renderer.drawFrame(frame.view, frame.projection, frame.drawItems);
}

} // namespace engine
