/*
 * Copyright © 2011-2017 Shobhit Kumar
 * Copyright © 2011 Intel Corporation
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

#ifndef WESTON_COMPOSITOR_HWC_H
#define WESTON_COMPOSITOR_HWC_H

#include "compositor.h"
#include "plugin-registry.h"

#ifdef  __cplusplus
extern "C" {
#endif

#define WESTON_HWC_BACKEND_CONFIG_VERSION 1

#define WESTON_HWC_OUTPUT_API_NAME "weston_hwc_output_api_v1"

/** The backend configuration struct.
 *
 * weston_hwc_backend_config contains the configuration used by a HWC
 * backend.
 */
struct weston_hwc_backend_config {
	struct weston_backend_config base;
};

struct weston_hwc_output_api {
	int (*set_mode)(struct weston_output *output);
};

static inline const struct weston_hwc_output_api *
weston_hwc_output_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor, WESTON_HWC_OUTPUT_API_NAME,
				    sizeof(struct weston_hwc_output_api));

	return (const struct weston_hwc_output_api *)api;
}

#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_HWC_H */
