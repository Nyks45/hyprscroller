#pragma once

#include <string>

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

// Hyprland 0.56's "view" refactor removed several helpers from CCompositor and
// moved global state into State:: trackers. These restore the ones we need.
namespace hyprscrolling_compat {
    inline PHLMONITOR monitorFromCursor() {
        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        for (auto& m : State::monitorState()->monitors()) {
            // logicalBox() accounts for scaling/transforms, like the rest of the plugin.
            if (m && m->logicalBox().containsPoint(COORDS))
                return m;
        }
        return nullptr;
    }

    inline PHLWORKSPACE workspaceByID(const WORKSPACEID& id) {
        for (auto& w : State::workspaceState()->workspacesCopy()) {
            if (w && w->m_id == id)
                return w;
        }
        return nullptr;
    }

    // Existing workspace with that id, or a new one on `monitor`.
    inline PHLWORKSPACE getOrCreateWorkspace(const WORKSPACEID& id, const std::string& name, const PHLMONITOR& monitor) {
        if (const auto WS = workspaceByID(id))
            return WS;
        return State::workspaceState()->create(id, monitor ? monitor->m_id : 0, name);
    }
}
