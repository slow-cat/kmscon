#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "eloop.h"
#include "shl_hook.h"
#include "shl_llog.h"
#include "uterm_input.h"
#include "uterm_input_internal.h"

#define LIBINPUT_WHEEL_NOTCH_UNITS 15.0

static int libinput_open_restricted(const char *path, int flags, void *data)
{
	(void)data;
	return open(path, flags | O_CLOEXEC);
}

static void libinput_close_restricted(int fd, void *data)
{
	(void)data;
	close(fd);
}

static const struct libinput_interface libinput_interface = {
	.open_restricted = libinput_open_restricted,
	.close_restricted = libinput_close_restricted,
};

static bool input_has_pointer_caps(unsigned int capabilities)
{
	return ((capabilities & (UTERM_DEVICE_HAS_REL | UTERM_DEVICE_HAS_MOUSE_BTN)) ==
		(UTERM_DEVICE_HAS_REL | UTERM_DEVICE_HAS_MOUSE_BTN)) ||
	       ((capabilities & (UTERM_DEVICE_HAS_ABS | UTERM_DEVICE_HAS_TOUCH)) ==
		(UTERM_DEVICE_HAS_ABS | UTERM_DEVICE_HAS_TOUCH)) ||
	       ((capabilities & (UTERM_DEVICE_HAS_ABS | UTERM_DEVICE_HAS_MOUSE_BTN)) ==
		(UTERM_DEVICE_HAS_ABS | UTERM_DEVICE_HAS_MOUSE_BTN));
}

static const char *libinput_node_basename(const char *node)
{
	const char *base;

	if (!node)
		return NULL;

	base = strrchr(node, '/');
	return base ? base + 1 : node;
}

static enum libinput_config_status libinput_apply_tap_config(struct libinput_device *device,
							     int tap_mode)
{
	if (tap_mode < 0 || libinput_device_config_tap_get_finger_count(device) == 0)
		return LIBINPUT_CONFIG_STATUS_SUCCESS;

	return libinput_device_config_tap_set_enabled(
		device, tap_mode ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
}

static enum libinput_config_status
libinput_apply_natural_scroll_config(struct libinput_device *device, int natural_scroll_mode)
{
	if (natural_scroll_mode < 0 ||
	    !libinput_device_config_scroll_has_natural_scroll(device))
		return LIBINPUT_CONFIG_STATUS_SUCCESS;

	return libinput_device_config_scroll_set_natural_scroll_enabled(device,
									 natural_scroll_mode);
}

static void libinput_configure_device(struct uterm_input *input, struct libinput_device *device)
{
	enum libinput_config_status ret;
	int natural_supported;
	int natural_enabled;

	if (!device)
		return;

	if (libinput_device_config_accel_is_available(device)) {
		ret = libinput_device_config_accel_set_speed(device,
						      input->libinput_accel_speed / 100.0);
		if (ret != LIBINPUT_CONFIG_STATUS_SUCCESS)
			llog_warn(input, "cannot set libinput accel on %s (%d)",
				  libinput_device_get_name(device), ret);
	}

	ret = libinput_apply_tap_config(device, input->libinput_tap);
	if (ret != LIBINPUT_CONFIG_STATUS_SUCCESS)
		llog_warn(input, "cannot set libinput tap on %s (%d)",
			  libinput_device_get_name(device), ret);

	natural_supported = libinput_device_config_scroll_has_natural_scroll(device);
	ret = libinput_apply_natural_scroll_config(device, input->libinput_natural_scroll);
	if (ret != LIBINPUT_CONFIG_STATUS_SUCCESS)
		llog_warn(input, "cannot set natural scroll on %s (%d)",
			  libinput_device_get_name(device), ret);
	natural_enabled = natural_supported ?
		libinput_device_config_scroll_get_natural_scroll_enabled(device) : 0;
	llog_debug(input,
		   "libinput config device=%s accel=%d tap=%d natural_req=%d natural_supported=%d natural_enabled=%d wheel_step=%.2f finger_step=%.2f",
		   libinput_device_get_name(device), input->libinput_accel_speed,
		   input->libinput_tap, input->libinput_natural_scroll, natural_supported,
		   natural_enabled, input->libinput_scroll_step_wheel,
		   input->libinput_scroll_step_finger);
}

static uint8_t libinput_map_button(uint32_t button)
{
	switch (button) {
	case BTN_LEFT:
		return 0;
	case BTN_RIGHT:
		return 1;
	case BTN_MIDDLE:
		return 2;
	default:
		return BUTTON_NONE;
	}
}

static int32_t clamp_pointer_value(int32_t value, int32_t max)
{
	if (value < 0)
		return 0;
	if (value > max)
		return max;
	return value;
}

static void libinput_send_pointer_event(struct uterm_input *input,
					const struct uterm_input_pointer_event *event)
{
	shl_hook_call(input->pointer_hook, input, (void *)event);
}

static void libinput_send_sync(struct uterm_input *input)
{
	struct uterm_input_pointer_event event = {0};
	struct itimerspec spec = {0};

	event.event = UTERM_SYNC;
	libinput_send_pointer_event(input, &event);

	spec.it_value.tv_sec = 20;
	ev_timer_update(input->hide_pointer, &spec);
}

static void libinput_emit_motion(struct uterm_input *input)
{
	struct uterm_input_pointer_event event = {0};

	event.event = UTERM_MOVED;
	event.pointer_x = input->pointer_x;
	event.pointer_y = input->pointer_y;
	if (input->pointer_button != BUTTON_NONE) {
		event.button = input->pointer_button;
		event.pressed = true;
	}

	libinput_send_pointer_event(input, &event);
}

static void libinput_emit_button(struct uterm_input *input, uint8_t button, bool pressed)
{
	struct uterm_input_pointer_event event = {0};
	struct timespec tp;
	uint64_t elapsed;

	event.event = UTERM_BUTTON;
	event.button = button;
	event.pressed = pressed;
	event.pointer_x = input->pointer_x;
	event.pointer_y = input->pointer_y;

	if (button == 0 && pressed) {
		clock_gettime(CLOCK_MONOTONIC, &tp);
		elapsed = (tp.tv_sec - input->pointer_last_click.tv_sec) * 1000 +
			  (tp.tv_nsec - input->pointer_last_click.tv_nsec) / 1000000;
		event.double_click = elapsed < 500;
		input->pointer_last_click = tp;
	}

	if (pressed)
		input->pointer_button = button;
	else if (input->pointer_button == button)
		input->pointer_button = BUTTON_NONE;

	libinput_send_pointer_event(input, &event);
}

static void libinput_emit_wheel(struct uterm_input *input, int32_t wheel)
{
	struct uterm_input_pointer_event event = {0};

	event.event = UTERM_WHEEL;
	event.wheel = wheel;
	event.pointer_x = input->pointer_x;
	event.pointer_y = input->pointer_y;
	libinput_send_pointer_event(input, &event);
}

static double libinput_scroll_to_uterm(double value)
{
	/*
	 * libinput defines positive vertical scroll values as "down".
	 * kmscon's existing UTERM_WHEEL convention uses positive values for
	 * scroll-up. Convert once at the backend boundary and leave natural
	 * scroll entirely to libinput device configuration.
	 */
	return -value;
}

static void libinput_emit_scroll_steps(struct uterm_input *input, double step)
{
	while (input->pointer_scroll_vertical >= step) {
		libinput_emit_wheel(input, 1);
		input->pointer_scroll_vertical -= step;
	}
	while (input->pointer_scroll_vertical <= -step) {
		libinput_emit_wheel(input, -1);
		input->pointer_scroll_vertical += step;
	}
}

static void libinput_handle_motion(struct uterm_input *input, struct libinput_event_pointer *event)
{
	input->pointer_x = clamp_pointer_value(input->pointer_x +
						 (int32_t)lround(libinput_event_pointer_get_dx(event)),
						 input->pointer_max_x);
	input->pointer_y = clamp_pointer_value(input->pointer_y +
						 (int32_t)lround(libinput_event_pointer_get_dy(event)),
						 input->pointer_max_y);
	libinput_emit_motion(input);
}

static void libinput_handle_absolute_motion(struct uterm_input *input,
					    struct libinput_event_pointer *event)
{
	uint32_t width = (uint32_t)input->pointer_max_x + 1;
	uint32_t height = (uint32_t)input->pointer_max_y + 1;

	input->pointer_x = clamp_pointer_value(
		(int32_t)libinput_event_pointer_get_absolute_x_transformed(event, width),
		input->pointer_max_x);
	input->pointer_y = clamp_pointer_value(
		(int32_t)libinput_event_pointer_get_absolute_y_transformed(event, height),
		input->pointer_max_y);
	libinput_emit_motion(input);
}

static void libinput_handle_button(struct uterm_input *input, struct libinput_event_pointer *event)
{
	uint8_t button = libinput_map_button(libinput_event_pointer_get_button(event));

	if (button == BUTTON_NONE)
		return;

	libinput_emit_button(input, button,
			     libinput_event_pointer_get_button_state(event) ==
				     LIBINPUT_BUTTON_STATE_PRESSED);
}

static void libinput_handle_scroll(struct uterm_input *input, enum libinput_event_type type,
				   struct libinput_event_pointer *event)
{
	double step;

	if (!libinput_event_pointer_has_axis(event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
		return;

	switch (type) {
	case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
		step = input->libinput_scroll_step_wheel;
		input->pointer_scroll_vertical +=
			libinput_scroll_to_uterm(
				libinput_event_pointer_get_scroll_value_v120(
					event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) /
			120.0 * LIBINPUT_WHEEL_NOTCH_UNITS;
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
	case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
		step = input->libinput_scroll_step_finger;
		input->pointer_scroll_vertical +=
			libinput_scroll_to_uterm(
				libinput_event_pointer_get_scroll_value(
					event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL));
		break;
	default:
		return;
	}

	libinput_emit_scroll_steps(input, step);
}

static void libinput_remove_stored_device(struct uterm_input *input, struct libinput_device *device)
{
	struct shl_dlist *iter;
	struct uterm_input_li_dev *entry;

	shl_dlist_for_each(iter, &input->libinput_devices)
	{
		entry = shl_dlist_entry(iter, struct uterm_input_li_dev, list);
		if (entry->device != device)
			continue;

		shl_dlist_unlink(&entry->list);
		libinput_device_unref(entry->device);
		free(entry->node);
		free(entry);
		return;
	}
}

static void libinput_refresh_stored_device(struct uterm_input *input, struct libinput_device *device)
{
	struct shl_dlist *iter;
	struct uterm_input_li_dev *entry;
	const char *sysname;

	if (!input || !device)
		return;

	sysname = libinput_device_get_sysname(device);
	if (!sysname)
		return;

	shl_dlist_for_each(iter, &input->libinput_devices)
	{
		entry = shl_dlist_entry(iter, struct uterm_input_li_dev, list);
		if (strcmp(libinput_node_basename(entry->node), sysname))
			continue;

		if (entry->device == device)
			return;

		libinput_device_unref(entry->device);
		entry->device = libinput_device_ref(device);
		return;
	}
}

static void libinput_dispatch_events(struct uterm_input *input)
{
	struct libinput_event *event;
	bool need_sync = false;

	if (!input->libinput)
		return;

	if (libinput_dispatch(input->libinput) < 0)
		return;

	while ((event = libinput_get_event(input->libinput))) {
		enum libinput_event_type type = libinput_event_get_type(event);

		switch (type) {
		case LIBINPUT_EVENT_POINTER_MOTION: {
			struct libinput_event_pointer *pev = libinput_event_get_pointer_event(event);
			libinput_handle_motion(input, pev);
			need_sync = true;
			break;
		}
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
			struct libinput_event_pointer *pev = libinput_event_get_pointer_event(event);
			libinput_handle_absolute_motion(input, pev);
			need_sync = true;
			break;
		}
		case LIBINPUT_EVENT_POINTER_BUTTON: {
			struct libinput_event_pointer *pev = libinput_event_get_pointer_event(event);
			libinput_handle_button(input, pev);
			need_sync = true;
			break;
		}
		case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
		case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
		case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
			struct libinput_event_pointer *pev = libinput_event_get_pointer_event(event);
			libinput_handle_scroll(input, type, pev);
			need_sync = true;
			break;
		}
		case LIBINPUT_EVENT_DEVICE_ADDED:
			libinput_refresh_stored_device(input, libinput_event_get_device(event));
			libinput_configure_device(input, libinput_event_get_device(event));
			break;
		case LIBINPUT_EVENT_DEVICE_REMOVED:
			libinput_remove_stored_device(input, libinput_event_get_device(event));
			break;
		default:
			break;
		}

		libinput_event_destroy(event);
	}

	if (need_sync)
		libinput_send_sync(input);
}

static void libinput_fd_event(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_input *input = data;

	if (mask & (EV_HUP | EV_ERR)) {
		if (input->libinput_fd) {
			ev_eloop_rm_fd(input->libinput_fd);
			input->libinput_fd = NULL;
		}
		return;
	}

	libinput_dispatch_events(input);
}

/**
 * Initialize the libinput pointer backend.
 *
 * @param input Input context to update.
 * @return 0 on success, negative error code on failure.
 */
int libinput_init(struct uterm_input *input)
{
	if (!input)
		return -EINVAL;

	shl_dlist_init(&input->libinput_devices);
	input->pointer_button = BUTTON_NONE;
	input->libinput = libinput_path_create_context(&libinput_interface, input);
	if (!input->libinput)
		return -ENOMEM;
	libinput_suspend(input->libinput);
	input->libinput_suspended = true;

	return 0;
}

/**
 * Destroy the libinput pointer backend and tracked devices.
 *
 * @param input Input context to update.
 */
void libinput_destroy(struct uterm_input *input)
{
	struct shl_dlist *iter;
	struct uterm_input_li_dev *entry;

	if (!input || !input->libinput)
		return;

	if (input->libinput_fd) {
		ev_eloop_rm_fd(input->libinput_fd);
		input->libinput_fd = NULL;
	}

	while ((iter = input->libinput_devices.next) != &input->libinput_devices) {
		entry = shl_dlist_entry(iter, struct uterm_input_li_dev, list);
		shl_dlist_unlink(&entry->list);
		libinput_path_remove_device(entry->device);
		libinput_device_unref(entry->device);
		free(entry->node);
		free(entry);
	}

	libinput_unref(input->libinput);
	input->libinput = NULL;
}

/**
 * Add a pointer device to the libinput path backend.
 *
 * @param input Input context to update.
 * @param node Device node path.
 * @param capabilities Probed device capabilities.
 * @param mouse Whether pointer support is enabled.
 * @return true if the device is handled by libinput.
 */
bool libinput_add_device(struct uterm_input *input, const char *node, unsigned int capabilities,
			 bool mouse)
{
	struct libinput_device *device;
	struct uterm_input_li_dev *entry;

	if (!input || !input->libinput || !node || !mouse || !input_has_pointer_caps(capabilities))
		return false;

	device = libinput_path_add_device(input->libinput, node);
	if (!device)
		return false;

	libinput_configure_device(input, device);

	entry = malloc(sizeof(*entry));
	if (!entry) {
		libinput_path_remove_device(device);
		return false;
	}

	memset(entry, 0, sizeof(*entry));
	entry->node = strdup(node);
	if (!entry->node) {
		free(entry);
		libinput_path_remove_device(device);
		return false;
	}

	entry->device = libinput_device_ref(device);
	shl_dlist_link(&input->libinput_devices, &entry->list);
	libinput_dispatch_events(input);
	return true;
}

/**
 * Remove a pointer device from the libinput path backend.
 *
 * @param input Input context to update.
 * @param node Device node path.
 * @return true if a tracked device was removed.
 */
bool libinput_remove_device(struct uterm_input *input, const char *node)
{
	struct shl_dlist *iter;
	struct uterm_input_li_dev *entry;

	if (!input || !input->libinput || !node)
		return false;

	shl_dlist_for_each(iter, &input->libinput_devices)
	{
		entry = shl_dlist_entry(iter, struct uterm_input_li_dev, list);
		if (strcmp(entry->node, node))
			continue;

		shl_dlist_unlink(&entry->list);
		libinput_path_remove_device(entry->device);
		libinput_device_unref(entry->device);
		free(entry->node);
		free(entry);
		libinput_dispatch_events(input);
		return true;
	}

	return false;
}

/**
 * Resume libinput event processing and bind its fd to the eloop.
 *
 * @param input Input context to update.
 * @return 0 on success, negative error code on failure.
 */
int libinput_wake_up(struct uterm_input *input)
{
	int fd;

	if (!input || !input->libinput)
		return 0;

	if (input->libinput_suspended) {
		if (libinput_resume(input->libinput) < 0)
			return -EFAULT;
		input->libinput_suspended = false;
	}

	fd = libinput_get_fd(input->libinput);
	if (fd < 0)
		return -EFAULT;

	if (!input->libinput_fd) {
		if (ev_eloop_new_fd(input->eloop, &input->libinput_fd, fd, EV_READABLE,
				    libinput_fd_event, input))
			return -EFAULT;
	}

	libinput_dispatch_events(input);
	return 0;
}

/**
 * Suspend libinput event processing and unbind its fd from the eloop.
 *
 * @param input Input context to update.
 */
void libinput_sleep(struct uterm_input *input)
{
	if (!input || !input->libinput)
		return;

	if (input->libinput_fd) {
		ev_eloop_rm_fd(input->libinput_fd);
		input->libinput_fd = NULL;
	}

	if (!input->libinput_suspended) {
		libinput_suspend(input->libinput);
		input->libinput_suspended = true;
	}
}
