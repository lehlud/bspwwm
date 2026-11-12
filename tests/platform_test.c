/* SPDX-License-Identifier: BSD-2-Clause
 * Exercise production static event handlers with real wlroots objects.
 * This is not a second compositor implementation or a production test hook.
 */
#define main compositor_main
#include "../wayland/server.c"
#undef main
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

static void key(struct wlr_keyboard *k, uint32_t code, bool pressed) {
	struct wlr_keyboard_key_event event = {
		.time_msec = 1, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(k, &event);
}

int main(void) {
	wlr_log_init(WLR_ERROR, NULL);
	assert(setenv("WLR_RENDERER", "pixman", 1) == 0);
	assert(setenv("XKB_DEFAULT_LAYOUT", "us", 1) == 0);
	struct server s = {.split_ratio = 0.5, .ipc_fd = -1};
	wl_list_init(&s.outputs); wl_list_init(&s.views); wl_list_init(&s.keyboards);
	wl_list_init(&s.pointers); wl_list_init(&s.orphan_desktops); wl_list_init(&s.ipc_clients);
	s.display = wl_display_create(); assert(s.display);
	s.backend = wlr_headless_backend_create(wl_display_get_event_loop(s.display)); assert(s.backend);
	s.renderer = wlr_renderer_autocreate(s.backend); assert(s.renderer);
	s.allocator = wlr_allocator_autocreate(s.backend, s.renderer); assert(s.allocator);
	s.layout = wlr_output_layout_create(s.display);
	s.scene = wlr_scene_create();
	s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.layout);
	s.seat = wlr_seat_create(s.display, "test-seat");
	s.cursor = wlr_cursor_create(); wlr_cursor_attach_output_layout(s.cursor, s.layout);
	s.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	LISTEN(&s.backend->events.new_output, s.new_output, new_output);
	LISTEN(&s.layout->events.change, s.layout_change, layout_change);
	LISTEN(&s.backend->events.new_input, s.new_input, new_input);
	LISTEN(&s.cursor->events.motion, s.motion, cursor_motion);
	assert(wlr_backend_start(s.backend));
	struct wlr_output *a = wlr_headless_add_output(s.backend, 800, 600); assert(a);
	struct wlr_output *b = wlr_headless_add_output(s.backend, 1024, 768); assert(b);
	assert(wl_list_length(&s.outputs) == 2);
	struct output *oa = wl_container_of(s.outputs.next, oa, link);
	struct desktop *d = oa->current;
	assert(s.active_output == oa && !d->monocle);
	static const struct wlr_keyboard_impl keyboard_impl = {.name = "test"};
	struct wlr_keyboard keyboard_device;
	wlr_keyboard_init(&keyboard_device, &keyboard_impl, "test-keyboard");
	wl_signal_emit_mutable(&s.backend->events.new_input, &keyboard_device.base);
	assert(s.seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD);
	struct keyboard *k = wl_container_of(s.keyboards.next, k, link);
	key(&keyboard_device, KEY_LEFTMETA, true);
	key(&keyboard_device, KEY_SPACE, true);
	assert(d->monocle && k->consumed[KEY_SPACE]);
	key(&keyboard_device, KEY_SPACE, false);
	assert(!k->consumed[KEY_SPACE]);
	key(&keyboard_device, KEY_2, true); key(&keyboard_device, KEY_2, false);
	assert(!strcmp(oa->current->name, "2"));
	focus_desktop(d);
	key(&keyboard_device, KEY_LEFTSHIFT, true);
	key(&keyboard_device, KEY_3, true);
	assert(k->consumed[KEY_3]); /* shifted # must still resolve to desktop 3 */
	key(&keyboard_device, KEY_3, false);
	key(&keyboard_device, KEY_LEFTSHIFT, false);
	key(&keyboard_device, KEY_LEFTMETA, false);
	wlr_keyboard_finish(&keyboard_device);
	assert(wl_list_empty(&s.keyboards));
	assert(!(s.seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD));
	static const struct wlr_pointer_impl pointer_impl = {.name = "test"};
	struct wlr_pointer pointer_device;
	wlr_pointer_init(&pointer_device, &pointer_impl, "test-pointer");
	wl_signal_emit_mutable(&s.backend->events.new_input, &pointer_device.base);
	assert(s.seat->capabilities & WL_SEAT_CAPABILITY_POINTER);
	struct wlr_pointer_motion_event motion = {
		.pointer = &pointer_device, .time_msec = 2, .delta_x = 20, .delta_y = 20,
	};
	wl_signal_emit_mutable(&pointer_device.events.motion, &motion);
	assert(s.cursor->x >= 0 && s.cursor->y >= 0);
	wlr_pointer_finish(&pointer_device);
	assert(wl_list_empty(&s.pointers));
	assert(!(s.seat->capabilities & WL_SEAT_CAPABILITY_POINTER));
	uint32_t desktop_id = d->id;
	wlr_output_destroy(a);
	assert(wl_list_length(&s.outputs) == 1 && d->output == s.active_output);
	wlr_output_destroy(b);
	assert(wl_list_empty(&s.outputs) && !s.active_output && !d->output);
	assert(wl_list_length(&s.orphan_desktops) == 4);
	struct wlr_output *c = wlr_headless_add_output(s.backend, 640, 480); assert(c);
	assert(d->id == desktop_id && d->output == s.active_output);
	assert(wl_list_empty(&s.orphan_desktops));
	assert(wl_list_length(&s.active_output->desktops) == 4);
	s.shutting_down = true;
	unlisten(&s.new_input); unlisten(&s.new_output); unlisten(&s.layout_change); unlisten(&s.motion);
	wlr_backend_destroy(s.backend);
	wlr_scene_node_destroy(&s.scene->tree.node);
	wlr_cursor_destroy(s.cursor); wlr_xcursor_manager_destroy(s.cursor_mgr);
	wlr_allocator_destroy(s.allocator); wlr_renderer_destroy(s.renderer);
	struct desktop *it, *tmp;
	wl_list_for_each_safe(it, tmp, &s.orphan_desktops, link) { wl_list_remove(&it->link); free(it); }
	wl_display_destroy(s.display);
	puts("Platform events passed: keyboard/pointer hotplug, shifted bindings, output removal/reappearance");
	return 0;
}
