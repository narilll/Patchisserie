// The editor window's key routing (src/editor/keys.hpp): what its editor, its fullscreen and Rack's window get.
#include "check.hpp"
#include "editor/keys.hpp"

using editor::KeyRoute;
using editor::routeKey;

TEST(keys_route_editor_window) {
	int asked = 0;
	auto editor = [&](bool takes) {
		return [&asked, takes] {
			asked++;
			return takes;
		};
	};
	CHECK(routeKey(GLFW_KEY_F11, editor(true)) == KeyRoute::ToggleFullscreen);
	CHECK(asked == 0);                                                  // F11 never reaches the editor
	CHECK(routeKey(GLFW_KEY_ESCAPE, editor(true)) == KeyRoute::Editor); // completions open: Esc closes them only
	CHECK(routeKey(GLFW_KEY_ESCAPE, editor(false)) == KeyRoute::LeaveFullscreen); // never Rack's (its own fullscreen)
	CHECK(routeKey(GLFW_KEY_S, editor(false)) == KeyRoute::Rack);                 // Cmd/Ctrl+S: Rack saves the patch
	CHECK(routeKey(GLFW_KEY_BACKSPACE, editor(true)) == KeyRoute::Editor);        // not Rack's: it deletes modules
	CHECK(asked == 4);
}
