#pragma once

#include <string>

#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/workspace/State.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/state/workspace/Target.hpp>

// Hyprland 0.56/0.57 refactors removed several helpers from CCompositor and
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

    // Hyprland 0.57 ("workspace refactor"): IDs are a variant (numbered or
    // special), workspaces are looked up/created through targets.
    inline bool isNumbered(const PHLWORKSPACE& ws, int id) {
        const auto N = ws ? ws->numberedID() : std::nullopt;
        return N && sc<int>(*N) == id;
    }

    // Existing workspace for a target, or a new one on `monitor`.
    inline PHLWORKSPACE getOrCreateWorkspace(const State::Workspace::STarget& target, const PHLMONITOR& monitor) {
        if (!target.valid())
            return nullptr;
        if (const auto WS = State::workspaceState()->find(target))
            return WS;
        return monitor ? State::workspaceState()->create(target, monitor) : nullptr;
    }

    // From a dispatcher-style string ("3", "+1", "name:x", "special:y")
    inline PHLWORKSPACE getOrCreateWorkspace(const std::string& str, const PHLMONITOR& monitor) {
        return getOrCreateWorkspace(State::Workspace::resolver()->getWorkspaceTargetFromString(str, monitor ? std::optional<PHLMONITOR>{monitor} : std::nullopt), monitor);
    }

    // Numbered workspace `id`
    inline PHLWORKSPACE getOrCreateWorkspace(int id, const PHLMONITOR& monitor) {
        return getOrCreateWorkspace(std::to_string(id), monitor);
    }
}
