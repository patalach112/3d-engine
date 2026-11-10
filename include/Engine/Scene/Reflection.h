#pragma once

#include "Core/Export.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Component reflection (Phase 25, step 1).
//
// A deliberately small, zero-overhead metadata system: for each
// reflected type, a static table describes its inspectable fields
// (name, display name, type, byte offset). Tables are produced by
// the ENGINE_REFLECT_* macros and cost nothing at runtime until
// someone iterates them — the upcoming editor generates inspectors
// from exactly this data, and gameplay code can read/mutate fields
// generically (tools, console, tests) without hardcoding layouts.
//
//   ENGINE_REFLECT_BEGIN(TransformComponent)
//       ENGINE_REFLECT_PROPERTY(position, "Position")
//       ENGINE_REFLECT_PROPERTY(scale, "Scale")
//   ENGINE_REFLECT_END()
//
// The block generates an inline reflectType() overload in the
// type's namespace, so blocks may live in headers next to the type.
// Built-in component blocks are in ComponentReflection.h;
// ReflectionRegistry additionally collects types by name for
// tooling that does not know the C++ type at compile time.
// ---------------------------------------------------------------------------

enum class PropertyType : uint8_t { Float, Int32, UInt32, Bool, Vec3 };

struct PropertyInfo {
    // Names are string_views into stringized macro literals: content
    // comparison works across binary boundaries (module/host), where
    // const char* pointer identity would not.
    std::string_view name;
    std::string_view displayName;
    PropertyType type = PropertyType::Float;
    size_t offset = 0; // byte offset of the field inside the component
};

struct TypeInfo {
    std::string_view name;
    std::span<const PropertyInfo> properties;

    [[nodiscard]] const PropertyInfo* findProperty(std::string_view propertyName) const {
        for (const PropertyInfo& property : properties) {
            if (propertyName == property.name) {
                return &property;
            }
        }
        return nullptr;
    }
};

namespace detail {

template <typename T>
struct PropertyTypeOf;
template <>
struct PropertyTypeOf<float> {
    static constexpr PropertyType value = PropertyType::Float;
};
template <>
struct PropertyTypeOf<int32_t> {
    static constexpr PropertyType value = PropertyType::Int32;
};
template <>
struct PropertyTypeOf<uint32_t> {
    static constexpr PropertyType value = PropertyType::UInt32;
};
template <>
struct PropertyTypeOf<bool> {
    static constexpr PropertyType value = PropertyType::Bool;
};
template <>
struct PropertyTypeOf<glm::vec3> {
    static constexpr PropertyType value = PropertyType::Vec3;
};

template <typename Class, typename Member>
constexpr PropertyType propertyTypeOf(Member Class::*) {
    return PropertyTypeOf<Member>::value;
}

} // namespace detail

// Each ENGINE_REFLECT_* block generates an overload of reflectType()
// in the reflected type's own namespace; reflect<T>() finds it by
// ADL. Types without a block simply fail to compile at the call.
template <typename T>
[[nodiscard]] const TypeInfo& reflect() {
    return reflectType(static_cast<const T*>(nullptr));
}

// Typed field access through a reflected offset. The instance is
// the component object; memcpy keeps this strict-aliasing safe.
template <typename T>
[[nodiscard]] T readProperty(const void* instance, const PropertyInfo& property) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(instance) + property.offset, sizeof(T));
    return value;
}

template <typename T>
void writeProperty(void* instance, const PropertyInfo& property, const T& value) {
    std::memcpy(static_cast<std::byte*>(instance) + property.offset, &value, sizeof(T));
}

// Name -> TypeInfo directory for tools (editor, console, tests).
// Registration replaces any previous entry with the same name, so
// re-registering after a hot reload is a no-op semantically.
class ENGINE_SCENE_API ReflectionRegistry {
public:
    [[nodiscard]] static ReflectionRegistry& instance();

    void registerType(const TypeInfo& info);
    // Removes the entry pointing at exactly this table (a module's
    // reflected types live in its binary; unregister on unload so
    // the registry never dereferences unmapped statics). Pointer
    // identity makes a retiring generation unable to remove its
    // replacement's table. Returns true when an entry was removed.
    bool unregisterType(const TypeInfo& expected);
    [[nodiscard]] const TypeInfo* find(std::string_view name) const;
    [[nodiscard]] std::span<const TypeInfo* const> types() const { return m_types; }

private:
    ReflectionRegistry() = default;
    std::vector<const TypeInfo*> m_types;
};

template <typename T>
void registerReflection() {
    ReflectionRegistry::instance().registerType(reflect<T>());
}

} // namespace engine

#define ENGINE_REFLECT_BEGIN(Type)                                                                 \
    inline const ::engine::TypeInfo& reflectType(const Type*) {                                    \
        using ReflectedType = Type;                                                                \
        static constexpr std::string_view reflectedTypeName = #Type;                               \
        static const ::engine::PropertyInfo properties[] = {
#define ENGINE_REFLECT_PROPERTY(field, displayName)                                                \
    ::engine::PropertyInfo {                                                                       \
        #field, displayName, ::engine::detail::propertyTypeOf(&ReflectedType::field),              \
            offsetof(ReflectedType, field)                                                         \
    }                                                                                              \
    ,
#define ENGINE_REFLECT_END()                                                                       \
    }                                                                                              \
    ;                                                                                              \
    static const ::engine::TypeInfo typeInfo{reflectedTypeName, properties};                       \
    return typeInfo;                                                                               \
    }
