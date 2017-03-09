/*
 * Copyright © 2011 - 2017 Shobhit Kumar
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <assert.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <libudev.h>

#include <gpudevice.h>
#include <gbm.h>
#include <hwclayer.h>

#include "compositor.h"
#include "compositor-hwc.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "gl-renderer.h"
#include "weston-egl-ext.h"
#include "pixman-renderer.h"
#include "libbacklight.h"
#include "libinput-seat.h"
#include "launcher-util.h"
#include "presentation-time-server-protocol.h"
#include "linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "libweston/compositor.h"

#define MAX_FRAMES	2
#define	MAX_LAYERS	1

struct hwc_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct wl_event_loop *loop;
	struct udev *udev;
	struct wl_event_source *hwc_source;

	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_hwc_source;

	struct wl_listener session_listener;

	struct udev_input input;
	struct gbm_device *gbm;
	struct hwc_handle *handle;
	int drm_fd;
	uint32_t gbm_format;
};

struct hwc_layer {
	struct gbm_import_fd_data import_data;
	struct gbm_bo *bo;
	void *fence;
	void *priv_data;
} hwc_layer;

struct hwc_frames {
	struct hwc_layer *hl[MAX_LAYERS];
} hwc_frames; /* double buffered */

struct hwc_output {
	struct weston_output base;
	struct weston_mode mode;
	struct weston_plane fb_plane;
	struct hwc_frames frame[MAX_FRAMES];
	struct hwc_frames *current, *next;
};

static const char default_seat[] = "seat0";
static struct gl_renderer_interface *gl_renderer;

static inline struct hwc_backend *
to_hwc_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct hwc_backend, base);
}

static inline struct hwc_output *
to_hwc_output(struct weston_output *base)
{
	return container_of(base, struct hwc_output, base);
}

static struct gbm_device *
create_gbm_device(int fd)
{
	struct gbm_device *gbm;

	gl_renderer = weston_load_module("gl-renderer.so",
					 "gl_renderer_interface");
	if (!gl_renderer)
		return NULL;

	/* GBM will load a dri driver, but even though they need symbols from
	 * libglapi, in some version of Mesa they are not linked to it. Since
	 * only the gl-renderer module links to it, the call above won't make
	 * these symbols globally available, and loading the DRI driver fails.
	 * Workaround this by dlopen()'ing libglapi with RTLD_GLOBAL. */
	dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);

	gbm = gbm_create_device(fd);

	return gbm;
}

static int
hwc_backend_create_gl_renderer(struct hwc_backend *b)
{
	EGLint format[2] = {
		b->gbm_format,
		0,
	};
	int n_formats = 1;

	if (format[1])
		n_formats = 3;
	if (gl_renderer->display_create(b->compositor,
					EGL_PLATFORM_GBM_KHR,
					(void *)b->gbm,
					NULL,
					gl_renderer->opaque_attribs,
					format,
					n_formats) < 0) {
		return -1;
	}

	return 0;
}

static int
on_hwc_input(int fd, uint32_t mask, void *data)
{
	return 1;
}

static void
hwc_restore(struct weston_compositor *ec)
{
	weston_launcher_restore(ec->launcher);
}

static void
hwc_destroy(struct weston_compositor *ec)
{
	struct hwc_backend *b = to_hwc_backend(ec);

	wl_event_source_remove(b->hwc_source);

	/* Destroy HWC layers here */

	weston_compositor_shutdown(ec);

	if (b->gbm)
		gbm_device_destroy(b->gbm);

	weston_launcher_destroy(ec->launcher);

	close(b->drm_fd);
	free(b);
}

static int
hwc_output_set_mode(struct weston_output *base)
{
	/* set the current enabled mode */
	/* Only preferred mode is supported as of now */
	/* TBD: */

	return 0;
}

static const struct weston_hwc_output_api api = {
	hwc_output_set_mode,
	NULL,
	NULL,
};

void
hwc_output_destroy(struct weston_output *base)
{
	return;
}

static void
hwc_output_start_repaint_loop(struct weston_output *output_base)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);
}

static int
hwc_output_repaint(struct weston_output *output_base,
		   pixman_region32_t *damage)
{
	struct hwc_output *output = to_hwc_output(output_base);
	struct hwc_backend *b = to_hwc_backend(output->base.compositor);

	if (!output->next) {
	struct gbm_bo *bo;

	output->base.compositor->renderer->repaint_output(&output->base, damage);

	bo = gbm_surface_lock_front_buffer(output->gbm_surface);
	if (!bo) {
		weston_log("failed to lock front buffer: %m\n");
		return;
	}

	output->next = drm_fb_get_from_bo(bo, b, output->gbm_format);
	if (!output->next) {
		weston_log("failed to get drm_fb for bo\n");
		gbm_surface_release_buffer(output->gbm_surface, bo);
		return;
	}

	return 0;
}

static void
hwc_assign_planes(struct weston_output *output_base)
{
}

static void
hwc_set_dpms(struct weston_output *output_base, enum dpms_enum level)
{
}

static int
hw_output_switch_mode(struct weston_output *output_base, struct weston_mode *mode)
{
}

static void
hwc_output_set_gamma(struct weston_output *output_base,
		     uint16_t size, uint16_t *r, uint16_t *g, uint16_t *b)
{
}

int
hwc_output_enable(struct weston_output *base)
{
	struct hwc_output *output = to_hwc_output(base);
	struct hwc_backend *b = to_hwc_backend(base->compositor);
	struct hwc_layer *l;
	struct frames *f;

	for (int i = 0; i < MAX_FRAMES; i++) {
		f = &output->frame[i];
		l = &f->hl[0];

		/* Primary plane */
		hwclayer_init(&l->priv_data);
		hwclayer_set_transform(l->priv_data, 0);
		hwclayer_set_crop(l->priv_data, 0, 0, output->mode.width, output->mode.height);
		hwclayer_set_frame(l->priv_data, 0, 0, output->mode.width, output->mode.height);

		l->import_data.format = b->gbm_format;
		l->bo = gbm_bo_create(b->gbm, output->mode.width, output->mode.height,
											 b->gbm_format, GBM_BO_USE_RENDERING);
		if (!l->bo) {
			weston_log("Error: HWC: Failed to create gbm_bo\n");
			return -1;
		}

		l->import_data.fd = gbm_bo_get_fd(l->bo);
		l->import_data.stride = gbm_bo_get_stride(l->bo);
		l->import_data.width = output->mode.width;
		l->import_data.height = output->mode.height;
		l->import_data.format = gbm_bo_get_format(l->bo);

		hwclayer_set_native_handle(l->priv_data, &l->import_data);
	}

	output->base.start_repaint_loop = hwc_output_start_repaint_loop;
	output->base.repaint = hwc_output_repaint;
	output->base.assign_planes = hwc_assign_planes;
	output->base.set_dpms = hwc_set_dpms;
	output->base.switch_mode = hwc_output_switch_mode;

	output->base.gamma_size = output->original_crtc->gamma_size;
	output->base.set_gamma = hwc_output_set_gamma;

	output->base.subpixel = DRM_MODE_SUBPIXEL_NONE;

	output->base.connection_internal = 1;

	weston_plane_init(&output->fb_plane, b->compositor, 0, 0);

	weston_compositor_stack_plane(b->compositor, &output->fb_plane,
				      &b->compositor->primary_plane);

	return 0;
}

int
hwc_output_disable(struct weston_output *base)
{
	return 0;
}

int hwc_create_output(struct hwc_backend *b)
{
	struct hwc_output *output;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->base.name = strdup("hwcdev");
	output->base.destroy = hwc_output_destroy;
	output->base.disable = hwc_output_disable;
	output->base.enable = hwc_output_enable;

	weston_output_init(&output->base, b->compositor);

	/* only preferred mode */
	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = b->handle->width;
	output->mode.height = b->handle->height;
	output->mode.refresh = b->handle->refresh_rate;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;

	weston_compositor_add_pending_output(&output->base, b->compositor);

	return 0;
}

static struct hwc_backend *
hwc_backend_create(struct weston_compositor *compositor,
		   struct weston_hwc_backend_config *config)
{
	struct hwc_backend *b;
	hwc_handle *handle;
	int fd, ret;

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	if (!device_initialize(&handle)) {
		weston_log("Error: HWC: Failed to initialize GPU device\n");
		goto release;
	}

	weston_log("Info: HWC: GPU device succesfully initialized\n");

	if (get_display(handle) != 0) {
		weston_log("Error: HWC: Failed to get display device\n");
		goto hwcerr;
	}

  fd = open("/dev/dri/renderD128", O_RDWR);
	if(!(b->gbm = create_gbm_device(fd))) {
    weston_log("Error: HWC: Failed to initialize GBM\n");
    close(fd);
		goto hwcerr;
  }

	b->drm_fd = fd;
	b->handle = handle;
	b->compositor = compositor;

	b->gbm_format = GBM_FORMAT_ARGB8888;

	if (hwc_backend_create_gl_renderer(b)) {
		gbm_device_destroy(b->gbm);
		goto hwcerr;
	}

	b->base.destroy = hwc_destroy;
	b->base.restore = hwc_restore;

	weston_setup_vt_switch_bindings(compositor);

	b->loop = wl_display_get_event_loop(compositor->wl_display);
	b->hwc_source =
		wl_event_loop_add_fd(b->loop, b->drm_fd,
				     WL_EVENT_READABLE, on_hwc_input, b);

	hwc_create_output(b);

	compositor->backend = &b->base;

	ret = weston_plugin_api_register(compositor, WESTON_HWC_OUTPUT_API_NAME,
					 &api, sizeof(api));

	if (ret < 0) {
		weston_log("Erroe: HWC: Failed to register output API.\n");
		goto westonerr;
	}

#if 0
	compositor->launcher = weston_launcher_connect(compositor, 2,
						       default_seat, true);
	if (compositor->launcher == NULL) {
		weston_log("fatal: hwc backend should be run "
			   "using weston-launch binary or as root\n");
		weston_launcher_destroy(compositor->launcher);
		weston_compositor_shutdown(compositor);
		goto release;
	}
#endif

	weston_log("Warn: HWC: Backend support is still WIP\n");
	return b;

westonerr:
	wl_event_source_remove(b->hwc_source);
hwcerr:
	device_destroy(handle);

release:
	free(b);

	return NULL;
}

static void
config_init_to_defaults(struct weston_hwc_backend_config *config)
{
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct hwc_backend *b;
	struct weston_hwc_backend_config config = {{ 0, }};

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_HWC_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_hwc_backend_config)) {
		weston_log("Error: HWC backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = hwc_backend_create(compositor, &config);
	if (b == NULL)
		return -1;

	return 0;
}
