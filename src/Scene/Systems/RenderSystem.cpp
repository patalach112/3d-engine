#include "Scene/Systems/RenderSystem.h"

#include "Core/Logger.h"
#include "Graphics/Swapchain.h"
#include "Scene/Scene.h"

namespace engine {

RenderSystem::RenderSystem(Renderer& renderer, const Swapchain& swapchain)
    : m_renderer(renderer), m_swapchain(swapchain) {}

PreparedFrame RenderSystem::prepareFrame(Scene& scene, float aspectRatio) {
    PreparedFrame frame;

    const CameraComponent* activeCamera = scene.findActiveCamera();
    if (activeCamera == nullptr) {
        return frame;
    }
    frame.hasCamera = true;
    frame.view = activeCamera->camera.viewMatrix();
    frame.projection = activeCamera->camera.projectionMatrix(aspectRatio);

    scene.registry().view<TransformComponent, MeshComponent>().each(
        [&frame](const TransformComponent& transform, const MeshComponent& mesh) {
            if (mesh.visible) {
                frame.drawItems.push_back(DrawItem{transform.modelMatrix()});
            }
        });
    return frame;
}

void RenderSystem::render(Scene& scene) {
    const VkExtent2D extent = m_swapchain.extent();
    const float aspectRatio = extent.height > 0
                                  ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                                  : 1.0f;

    const PreparedFrame frame = prepareFrame(scene, aspectRatio);
    if (!frame.hasCamera) {
        if (!m_warnedNoCamera) {
            m_warnedNoCamera = true;
            ENGINE_LOG_WARNING("RenderSystem: scene has no active camera; skipping rendering "
                               "until one is added");
        }
        return;
    }
    m_warnedNoCamera = false;

    m_renderer.drawFrame(frame.view, frame.projection, frame.drawItems);
}

} // namespace engine
