/*
 * Copyright © 2011 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "compositor.h"

void
wlsc_data_source_unref(struct wlsc_data_source *source)
{
	source->refcount--;
	if (source->refcount == 0)
		free(source);
}

static void
data_offer_accept(struct wl_client *client, struct wl_resource *resource,
		  uint32_t time, const char *mime_type)
{
	struct wlsc_data_source *source = resource->data;

	/* FIXME: Check that client is currently focused by the input
	 * device that is currently dragging this data source.  Should
	 * this be a wl_data_device request? */

	wl_resource_post_event(&source->resource,
			       WL_DATA_SOURCE_TARGET, mime_type);
}

static void
data_offer_receive(struct wl_client *client, struct wl_resource *resource,
		   const char *mime_type, int32_t fd)
{
	struct wlsc_data_source *source = resource->data;

	wl_resource_post_event(&source->resource,
			       WL_DATA_SOURCE_SEND, mime_type, fd);
	close(fd);
}

static void
data_offer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource, wlsc_compositor_get_time());
}

static void
destroy_data_offer(struct wl_resource *resource)
{
	struct wlsc_data_source *source = resource->data;

	wlsc_data_source_unref(source);
	free(resource);
}

static const struct wl_data_offer_interface data_offer_interface = {
	data_offer_accept,
	data_offer_receive,
	data_offer_destroy,
};

static struct wl_resource *
data_source_create_offer(struct wlsc_data_source *source, 
			 struct wl_resource *target)
{
	struct wl_resource *resource;

	resource = wl_client_new_object(target->client,
					&wl_data_offer_interface,
					&data_offer_interface, source);
	resource->destroy = destroy_data_offer;

	return resource;
}

static void
data_source_cancel(struct wlsc_data_source *source)
{
	wl_resource_post_event(&source->resource, WL_DATA_SOURCE_CANCELLED);
}

static struct wl_resource *
wlsc_data_source_send_offer(struct wlsc_data_source *source,
			    struct wl_resource *target)
{
	struct wl_resource *resource;
	char **p, **end;

	resource = source->create_offer(source, target);
	source->refcount++;

	wl_resource_post_event(target, WL_DATA_DEVICE_DATA_OFFER, resource);

	end = source->mime_types.data + source->mime_types.size;
	for (p = source->mime_types.data; p < end; p++)
		wl_resource_post_event(resource, WL_DATA_OFFER_OFFER, *p);

	return resource;
}

static void
data_source_offer(struct wl_client *client,
		  struct wl_resource *resource,
		  const char *type)
{
	struct wlsc_data_source *source = resource->data;
	char **p;

	p = wl_array_add(&source->mime_types, sizeof *p);
	if (p)
		*p = strdup(type);
	if (!p || !*p)
		wl_resource_post_no_memory(resource);
}

static void
data_source_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource, wlsc_compositor_get_time());
}

static struct wl_data_source_interface data_source_interface = {
	data_source_offer,
	data_source_destroy
};

static struct wl_resource *
find_resource(struct wl_list *list, struct wl_client *client)
{
	struct wl_resource *r;

	wl_list_for_each(r, list, link) {
		if (r->client == client)
			return r;
	}

	return NULL;
}

static void
destroy_drag_focus(struct wl_listener *listener,
		   struct wl_resource *resource, uint32_t time)
{
	struct wlsc_input_device *device =
		container_of(listener, struct wlsc_input_device,
			     drag_focus_listener);

	device->drag_focus_resource = NULL;
}

static void
drag_set_focus(struct wlsc_input_device *device,
	       struct wl_surface *surface, uint32_t time,
	       int32_t x, int32_t y)
{
	struct wl_resource *resource, *offer;

	if (device->drag_focus == surface)
		return;

	if (device->drag_focus_resource) {
		wl_resource_post_event(device->drag_focus_resource,
				       WL_DATA_DEVICE_LEAVE);
		wl_list_remove(&device->drag_focus_listener.link);
		device->drag_focus_resource = NULL;
		device->drag_focus = NULL;
	}

	if (surface)
		resource = find_resource(&device->drag_resource_list, 
					 surface->resource.client);
	if (surface && resource) {
		offer = wlsc_data_source_send_offer(device->drag_data_source,
						    resource);

		wl_resource_post_event(resource,
				       WL_DATA_DEVICE_ENTER,
				       time, surface, x, y, offer);

		device->drag_focus = surface;
		device->drag_focus_listener.func = destroy_drag_focus;
		wl_list_insert(resource->destroy_listener_list.prev,
			       &device->drag_focus_listener.link);
		device->drag_focus_resource = resource;
	}
}

static void
drag_grab_motion(struct wl_grab *grab,
		 uint32_t time, int32_t x, int32_t y)
{
	struct wlsc_input_device *device =
		container_of(grab, struct wlsc_input_device, grab);
	struct wlsc_surface *es;

	es = pick_surface(&device->input_device, &x, &y);
	drag_set_focus(device, &es->surface, time, x, y);

	if (es && device->drag_focus_resource)
		wl_resource_post_event(device->drag_focus_resource,
				       WL_DATA_DEVICE_MOTION, time, x, y);
}

static void
drag_grab_button(struct wl_grab *grab,
		 uint32_t time, int32_t button, int32_t state)
{
}

static void
drag_grab_end(struct wl_grab *grab, uint32_t time)
{
	struct wlsc_input_device *device =
		container_of(grab, struct wlsc_input_device, grab);

	if (device->drag_focus_resource)
		wl_resource_post_event(device->drag_focus_resource,
				       WL_DATA_DEVICE_DROP);

	drag_set_focus(device, NULL, time, 0, 0);
	wlsc_data_source_unref(device->drag_data_source);
	device->drag_data_source = NULL;
}

static const struct wl_grab_interface drag_grab_interface = {
	drag_grab_motion,
	drag_grab_button,
	drag_grab_end
};

static void
data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
		       struct wl_resource *source_resource,
		       struct wl_resource *surface_resource, uint32_t time)
{
	struct wlsc_input_device *device = resource->data;
	struct wlsc_surface *surface = surface_resource->data;
	struct wlsc_surface *target;
	int32_t sx, sy;

	/* FIXME: Check that client has implicit grab on the surface
	 * that matches the given time. */

	/* FIXME: Check that the data source type array isn't empty. */

	if (wl_input_device_update_grab(&device->input_device, &device->grab,
					&surface->surface, time) < 0)
		return;

	device->grab.interface = &drag_grab_interface;
	device->drag_data_source = source_resource->data;
	device->drag_data_source->refcount++;

	target = pick_surface(&device->input_device, &sx, &sy);
	wl_input_device_set_pointer_focus(&device->input_device,
					  NULL, time, 0, 0, 0, 0);
	drag_set_focus(device, &target->surface, time, sx, sy);
}

static void
data_device_attach(struct wl_client *client, struct wl_resource *resource,
		   uint32_t time,
		   struct wl_resource *buffer, int32_t x, int32_t y)
{
}

static void
destroy_selection_data_source(struct wl_listener *listener,
			      struct wl_resource *resource, uint32_t time)
{
	struct wlsc_input_device *device =
		container_of(listener, struct wlsc_input_device,
			     selection_data_source_listener);
	struct wl_resource *data_device, *focus;

	device->selection_data_source = NULL;

	focus = device->input_device.keyboard_focus_resource;
	if (focus) {
		data_device = find_resource(&device->drag_resource_list,
					    focus->client);
		wl_resource_post_event(data_device,
				       WL_DATA_DEVICE_SELECTION, NULL);
	}
}

void
wlsc_input_device_set_selection(struct wlsc_input_device *device,
				struct wlsc_data_source *source, uint32_t time)
{
	struct wl_resource *data_device, *focus, *offer;

	if (device->selection_data_source) {
		device->selection_data_source->cancel(device->selection_data_source);
		/* FIXME: All non-active clients will probably hold a
		 * reference to the selection data source, and thus it
		 * won't get destroyed until every client has been
		 * activated and seen the new selection event. */
		wl_list_remove(&device->selection_data_source_listener.link);
		wlsc_data_source_unref(device->selection_data_source);
		device->selection_data_source = NULL;
	}

	device->selection_data_source = source;
	source->refcount++;

	focus = device->input_device.keyboard_focus_resource;
	if (focus) {
		data_device = find_resource(&device->drag_resource_list,
					    focus->client);
		if (data_device) {
			offer = wlsc_data_source_send_offer(device->selection_data_source,
							    data_device);
			wl_resource_post_event(data_device,
					       WL_DATA_DEVICE_SELECTION,
					       offer);
		}
	}

	device->selection_data_source_listener.func =
		destroy_selection_data_source;
	wl_list_insert(source->resource.destroy_listener_list.prev,
		       &device->selection_data_source_listener.link);
}

static void
data_device_set_selection(struct wl_client *client,
			  struct wl_resource *resource,
			  struct wl_resource *source_resource, uint32_t time)
{
	if (!source_resource)
		return;

	wlsc_input_device_set_selection(resource->data,
					source_resource->data, time);
}

static const struct wl_data_device_interface data_device_interface = {
	data_device_start_drag,
	data_device_attach,
	data_device_set_selection,
};

static void
destroy_data_source(struct wl_resource *resource)
{
	struct wlsc_data_source *source =
		container_of(resource, struct wlsc_data_source, resource);
	char **p, **end;

	end = source->mime_types.data + source->mime_types.size;
	for (p = source->mime_types.data; p < end; p++)
		free(*p);

	wl_array_release(&source->mime_types);

	source->resource.object.id = 0;
	wlsc_data_source_unref(source);
}

static void
create_data_source(struct wl_client *client,
		   struct wl_resource *resource, uint32_t id)
{
	struct wlsc_data_source *source;

	source = malloc(sizeof *source);
	if (source == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	source->resource.destroy = destroy_data_source;
	source->resource.object.id = id;
	source->resource.object.interface = &wl_data_source_interface;
	source->resource.object.implementation =
		(void (**)(void)) &data_source_interface;
	source->resource.data = source;
	source->create_offer = data_source_create_offer;
	source->cancel = data_source_cancel;
	source->refcount = 1;

	wl_array_init(&source->mime_types);
	wl_client_add_resource(client, &source->resource);
}

static void unbind_data_device(struct wl_resource *resource)
{
	wl_list_remove(&resource->link);
	free(resource);
}

static void
get_data_device(struct wl_client *client,
		struct wl_resource *manager_resource,
		uint32_t id, struct wl_resource *input_device)
{
	struct wlsc_input_device *device = input_device->data;
	struct wl_resource *resource;

	resource =
		wl_client_add_object(client, &wl_data_device_interface,
				     &data_device_interface, id, device);
				     
	wl_list_insert(&device->drag_resource_list, &resource->link);
	resource->destroy = unbind_data_device;
}

static const struct wl_data_device_manager_interface manager_interface = {
	create_data_source,
	get_data_device
};

static void
bind_manager(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	wl_client_add_object(client, &wl_data_device_manager_interface,
			     &manager_interface, id, NULL);
}

WL_EXPORT void
wlsc_data_device_set_keyboard_focus(struct wlsc_input_device *device)
{
	struct wl_resource *data_device, *focus, *offer;
	struct wlsc_data_source *source;

	focus = device->input_device.keyboard_focus_resource;
	if (!focus)
		return;

	data_device = find_resource(&device->drag_resource_list,
				    focus->client);
	if (!data_device)
		return;

	source = device->selection_data_source;
	if (source) {
		offer = wlsc_data_source_send_offer(source, data_device);
		wl_resource_post_event(data_device,
				       WL_DATA_DEVICE_SELECTION, offer);
	}
}

WL_EXPORT int
wlsc_data_device_manager_init(struct wlsc_compositor *compositor)
{
	if (wl_display_add_global(compositor->wl_display,
				  &wl_data_device_manager_interface,
				  NULL, bind_manager) == NULL)
		return -1;

	return 0;
}
