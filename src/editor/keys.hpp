#pragma once
// Where Filo's editor window (editor::Window, view.cpp) sends a key: pure, test/test_keys.cpp tests it. GLFW's key
// codes, without its GL header (test/ has none; Rack's code includes GLEW's first).
#include <functional>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

namespace editor {

enum class KeyRoute { Editor, ToggleFullscreen, LeaveFullscreen, Rack };

/** F11 toggles the window's fullscreen before the editor sees it; the editor gets the rest (`editor()` hands it over:
true if it took it). What it lets through, Rack's shortcuts (save, quit, open, new) and Esc with no completions open:
Esc leaves the window's fullscreen (or does nothing), Rack's window gets the others. */
inline KeyRoute routeKey(int key, const std::function<bool()>& editor) {
	if (key == GLFW_KEY_F11)
		return KeyRoute::ToggleFullscreen;
	if (editor())
		return KeyRoute::Editor;
	return key == GLFW_KEY_ESCAPE ? KeyRoute::LeaveFullscreen : KeyRoute::Rack;
}

} // namespace editor
