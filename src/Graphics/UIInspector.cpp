#include "Graphics/UIInspector.h"

#include <cstdio>
#include <string>

namespace engine {

bool inspectComponent(UIContext& ui, const UIRect& rect, const TypeInfo& type, void* instance,
                      const UIInspectorOptions& options) {
    if (instance == nullptr || ui.batch() == nullptr) {
        return false;
    }
    bool changed = false;
    ui.beginPanel(type.name, rect, UIRectStyle{8.0f, 1.0f, glm::vec4{}});
    ui.label(type.name);

    for (const PropertyInfo& property : type.properties) {
        switch (property.type) {
        case PropertyType::Float: {
            float value = readProperty<float>(instance, property);
            if (ui.slider(property.displayName, &value, options.floatMin, options.floatMax)) {
                writeProperty(instance, property, value);
                changed = true;
            }
            break;
        }
        case PropertyType::Bool: {
            bool value = readProperty<bool>(instance, property);
            if (ui.checkbox(property.displayName, &value)) {
                writeProperty(instance, property, value);
                changed = true;
            }
            break;
        }
        case PropertyType::Vec3: {
            glm::vec3 value = readProperty<glm::vec3>(instance, property);
            // Transform fields get editor-grade ranges: a rotation
            // clamped to +/-10 degrees (the old options default) or
            // a scale that cannot go below -10 is useless in a 3D
            // editor. Everything else keeps the caller's range.
            float rangeMin = options.componentMin;
            float rangeMax = options.componentMax;
            if (type.name == "TransformComponent") {
                if (property.name == "rotationDegrees") {
                    rangeMin = -3600.0f; // ten full turns each way
                    rangeMax = 3600.0f;
                } else if (property.name == "scale") {
                    rangeMin = 0.001f; // never zero/negative via sliders
                    rangeMax = 1000.0f;
                } else if (property.name == "position") {
                    rangeMin = -5000.0f;
                    rangeMax = 5000.0f;
                }
            }
            if (ui.vec3Sliders(property.displayName, &value, rangeMin, rangeMax)) {
                writeProperty(instance, property, value);
                changed = true;
            }
            break;
        }
        case PropertyType::Int32: {
            char text[96];
            std::snprintf(text, sizeof(text), "%.*s: %d",
                          static_cast<int>(property.displayName.size()), property.displayName.data(),
                          static_cast<int>(readProperty<int32_t>(instance, property)));
            ui.label(text);
            break;
        }
        case PropertyType::UInt32: {
            char text[96];
            std::snprintf(text, sizeof(text), "%.*s: %u",
                          static_cast<int>(property.displayName.size()), property.displayName.data(),
                          static_cast<unsigned>(readProperty<uint32_t>(instance, property)));
            ui.label(text);
            break;
        }
        }
    }

    ui.endPanel();
    return changed;
}

} // namespace engine
