// PlaySession (Phase 31): see Scene/PlaySession.h. The snapshot
// is the Phase 30 GBQ serialization held in memory — capturing
// and restoring a scene is exactly a serialize/deserialize with
// no file in between, so Play/Stop inherits the format's
// bit-exact floats and validate-before-touch load contract.

#include "Scene/PlaySession.h"

#include "Core/Logger.h"
#include "Scene/Scene.h"

namespace engine {

bool PlaySession::enterPlay(Scene& scene) {
    if (m_mode == EditorMode::Play) {
        ENGINE_LOG_WARNING("PlaySession: enterPlay ignored — already playing");
        return false;
    }
    m_snapshot = scene.saveToGbqBytes();
    m_mode = EditorMode::Play;
    ENGINE_LOG_INFO("Play mode: snapshot captured (" << m_snapshot.size() << " bytes, " << scene.entityCount()
                                                    << " entities); simulation running");
    return true;
}

bool PlaySession::exitPlay(Scene& scene) {
    if (m_mode != EditorMode::Play) {
        return false; // stopping while editing is a harmless no-op
    }
    if (!scene.loadFromGbqBytes(m_snapshot)) {
        // The loader left the live scene untouched; stay in Play
        // so the editor never presents a mutated scene as "edit"
        // state.
        ENGINE_LOG_ERROR("PlaySession: snapshot restore FAILED — staying in Play mode");
        return false;
    }
    m_snapshot.clear();
    m_snapshot.shrink_to_fit();
    m_mode = EditorMode::Edit;
    ENGINE_LOG_INFO("Edit mode: pre-play snapshot restored (" << scene.entityCount() << " entities)");
    return true;
}

} // namespace engine
