/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#define _GNU_SOURCE

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <linux/input.h>

#include "wayland-server.h"
#include "compositor.h"

/* The plan here is to generate a random anonymous socket name and
 * advertise that through a service on the session dbus.
 */
static const char *option_socket_name = NULL;
static const char *option_background = "background.jpg";
static const char *option_geometry = "1024x640";
static int option_connector = 0;

static const GOptionEntry option_entries[] = {
	{ "background", 'b', 0, G_OPTION_ARG_STRING,
	  &option_background, "Background image" },
	{ "connector", 'c', 0, G_OPTION_ARG_INT,
	  &option_connector, "KMS connector" },
	{ "geometry", 'g', 0, G_OPTION_ARG_STRING,
	  &option_geometry, "Geometry" },
	{ "socket", 's', 0, G_OPTION_ARG_STRING,
	  &option_socket_name, "Socket Name" },
	{ NULL }
};

static void
wlsc_matrix_init(struct wlsc_matrix *matrix)
{
	static const struct wlsc_matrix identity = {
		{ 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }
	};

	memcpy(matrix, &identity, sizeof identity);
}

static void
wlsc_matrix_multiply(struct wlsc_matrix *m, const struct wlsc_matrix *n)
{
	struct wlsc_matrix tmp;
	const GLfloat *row, *column;
	div_t d;
	int i, j;

	for (i = 0; i < 16; i++) {
		tmp.d[i] = 0;
		d = div(i, 4);
		row = m->d + d.quot * 4;
		column = n->d + d.rem;
		for (j = 0; j < 4; j++)
			tmp.d[i] += row[j] * column[j * 4];
	}
	memcpy(m, &tmp, sizeof tmp);
}

static void
wlsc_matrix_translate(struct wlsc_matrix *matrix, GLfloat x, GLfloat y, GLfloat z)
{
	struct wlsc_matrix translate = {
		{ 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 }
	};

	wlsc_matrix_multiply(matrix, &translate);
}

static void
wlsc_matrix_scale(struct wlsc_matrix *matrix, GLfloat x, GLfloat y, GLfloat z)
{
	struct wlsc_matrix scale = {
		{ x, 0, 0, 0,  0, y, 0, 0,  0, 0, z, 0,  0, 0, 0, 1 }
	};

	wlsc_matrix_multiply(matrix, &scale);
}

static void
wlsc_matrix_transform(struct wlsc_matrix *matrix, struct wlsc_vector *v)
{
	int i, j;
	struct wlsc_vector t;

	for (i = 0; i < 4; i++) {
		t.f[i] = 0;
		for (j = 0; j < 4; j++)
			t.f[i] += v->f[j] * matrix->d[i + j * 4];
	}

	*v = t;
}

static struct wlsc_surface *
wlsc_surface_create(struct wlsc_compositor *compositor,
		    int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wlsc_surface *surface;

	surface = malloc(sizeof *surface);
	if (surface == NULL)
		return NULL;

	wl_list_init(&surface->surface.destroy_listener_list);
	wl_list_init(&surface->link);
	surface->mapped = 0;

	glGenTextures(1, &surface->texture);
	glBindTexture(GL_TEXTURE_2D, surface->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	surface->compositor = compositor;
	surface->visual = NULL;
	surface->x = x;
	surface->y = y;
	surface->width = width;
	surface->height = height;
	wlsc_matrix_init(&surface->matrix);
	wlsc_matrix_scale(&surface->matrix, width, height, 1);
	wlsc_matrix_translate(&surface->matrix, x, y, 0);

	wlsc_matrix_init(&surface->matrix_inv);
	wlsc_matrix_translate(&surface->matrix_inv, -x, -y, 0);
	wlsc_matrix_scale(&surface->matrix_inv, 1.0 / width, 1.0 / height, 1);

	return surface;
}

uint32_t
get_time(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
destroy_surface(struct wl_resource *resource, struct wl_client *client)
{
	struct wlsc_surface *surface =
		container_of(resource, struct wlsc_surface, surface.resource);
	struct wlsc_compositor *compositor = surface->compositor;
	struct wl_listener *l, *next;
	uint32_t time;

	wl_list_remove(&surface->link);
	glDeleteTextures(1, &surface->texture);

	time = get_time();
	wl_list_for_each_safe(l, next,
			      &surface->surface.destroy_listener_list, link)
		l->func(l, &surface->surface, time);

	free(surface);

	wlsc_compositor_schedule_repaint(compositor);
}

uint32_t *
wlsc_load_image(const char *filename, int width, int height)
{
	GdkPixbuf *pixbuf;
	GError *error = NULL;
	int stride, i, n_channels;
	unsigned char *pixels, *end, *argb_pixels, *s, *d;

	pixbuf = gdk_pixbuf_new_from_file_at_scale(filename,
						   width, height,
						   FALSE, &error);
	if (error != NULL) {
		fprintf(stderr, "failed to load image: %s\n", error->message);
		g_error_free(error);
		return NULL;
	}

	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);
	n_channels = gdk_pixbuf_get_n_channels(pixbuf);

	argb_pixels = malloc (height * width * 4);
	if (argb_pixels == NULL) {
		g_object_unref(pixbuf);
		return NULL;
	}

	if (n_channels == 4) {
		for (i = 0; i < height; i++) {
			s = pixels + i * stride;
			end = s + width * 4;
			d = argb_pixels + i * width * 4;
			while (s < end) {
				unsigned int t;

#define MULT(_d,c,a,t) \
	do { t = c * a + 0x7f; _d = ((t >> 8) + t) >> 8; } while (0)
				
				MULT(d[0], s[2], s[3], t);
				MULT(d[1], s[1], s[3], t);
				MULT(d[2], s[0], s[3], t);
				d[3] = s[3];
				s += 4;
				d += 4;
			}
		}
	} else if (n_channels == 3) {
		for (i = 0; i < height; i++) {
			s = pixels + i * stride;
			end = s + width * 3;
			d = argb_pixels + i * width * 4;
			while (s < end) {
				d[0] = s[2];
				d[1] = s[1];
				d[2] = s[0];
				d[3] = 0xff;
				s += 3;
				d += 4;
			}
		}
	}

	g_object_unref(pixbuf);

	return (uint32_t *) argb_pixels;
}

static struct wl_buffer *
create_buffer_from_png(struct wlsc_compositor *ec,
		       const char *filename, int width, int height)
{
	uint32_t *pixels;
	struct wl_buffer *buffer;

	pixels = wlsc_load_image(filename, width, height);
	if(pixels == NULL)
	    return NULL;

	buffer = ec->create_buffer(ec, width, height,
				   &ec->compositor.premultiplied_argb_visual,
				   pixels);

	free(pixels);

	return buffer;
}

static const struct {
	const char *filename;
	int hotspot_x, hotspot_y;
} pointer_images[] = {
	{ DATADIR "/wayland/bottom_left_corner.png",	 6, 30 },
	{ DATADIR "/wayland/bottom_right_corner.png",	28, 28 },
	{ DATADIR "/wayland/bottom_side.png",		16, 20 },
	{ DATADIR "/wayland/grabbing.png",		20, 17 },
	{ DATADIR "/wayland/left_ptr.png",		10,  5 },
	{ DATADIR "/wayland/left_side.png",		10, 20 },
	{ DATADIR "/wayland/right_side.png",		30, 19 },
	{ DATADIR "/wayland/top_left_corner.png",	 8,  8 },
	{ DATADIR "/wayland/top_right_corner.png",	26,  8 },
	{ DATADIR "/wayland/top_side.png",		18,  8 },
	{ DATADIR "/wayland/xterm.png",			15, 15 }
};

static void
create_pointer_images(struct wlsc_compositor *ec)
{
	int i, count;
	const int width = 32, height = 32;

	count = ARRAY_LENGTH(pointer_images);
	ec->pointer_buffers = malloc(count * sizeof *ec->pointer_buffers);
	for (i = 0; i < count; i++) {
		ec->pointer_buffers[i] =
			create_buffer_from_png(ec,
					       pointer_images[i].filename,
					       width, height);
	}
}

static struct wlsc_surface *
background_create(struct wlsc_output *output, const char *filename)
{
	struct wlsc_surface *background;
	struct wl_buffer *buffer;

	background = wlsc_surface_create(output->compositor,
					 output->x, output->y,
					 output->width, output->height);
	if (background == NULL)
		return NULL;

	buffer = create_buffer_from_png(output->compositor,
					filename,
					output->width, output->height);
	if (buffer == NULL) {
		free(background);
		return NULL;
	}
	buffer->attach(buffer, &background->surface);

	return background;
}

static void
wlsc_surface_draw(struct wlsc_surface *es, struct wlsc_output *output)
{
	struct wlsc_compositor *ec = es->compositor;
	struct wlsc_matrix tmp;

	tmp = es->matrix;
	wlsc_matrix_multiply(&tmp, &output->matrix);
	glUniformMatrix4fv(ec->proj_uniform, 1, GL_FALSE, tmp.d);
	glUniform1i(ec->tex_uniform, 0);

	if (es->visual == &ec->compositor.argb_visual) {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_BLEND);
	} else if (es->visual == &ec->compositor.premultiplied_argb_visual) {
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_BLEND);
	} else {
		glDisable(GL_BLEND);
	}

	glBindTexture(GL_TEXTURE_2D, es->texture);
	glBindBuffer(GL_ARRAY_BUFFER, ec->vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
			      5 * sizeof(GLfloat), NULL);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
			      5 * sizeof(GLfloat), (GLfloat *) 0 + 3);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void
wlsc_surface_raise(struct wlsc_surface *surface)
{
	struct wlsc_compositor *compositor = surface->compositor;

	wl_list_remove(&surface->link);
	wl_list_insert(&compositor->surface_list, &surface->link);
}

void
wlsc_surface_update_matrix(struct wlsc_surface *es)
{
	wlsc_matrix_init(&es->matrix);
	wlsc_matrix_scale(&es->matrix, es->width, es->height, 1);
	wlsc_matrix_translate(&es->matrix, es->x, es->y, 0);

	wlsc_matrix_init(&es->matrix_inv);
	wlsc_matrix_translate(&es->matrix_inv, -es->x, -es->y, 0);
	wlsc_matrix_scale(&es->matrix_inv,
			  1.0 / es->width, 1.0 / es->height, 1);
}

void
wlsc_compositor_finish_frame(struct wlsc_compositor *compositor, int msecs)
{
	wl_display_post_frame(compositor->wl_display, msecs);
	wl_event_source_timer_update(compositor->timer_source, 5);
}

static void
wlsc_output_repaint(struct wlsc_output *output)
{
	struct wlsc_compositor *ec = output->compositor;
	struct wlsc_surface *es;
	struct wlsc_input_device *eid;

	glViewport(0, 0, output->width, output->height);

	if (output->background)
		wlsc_surface_draw(output->background, output);
	else
		glClear(GL_COLOR_BUFFER_BIT);

	wl_list_for_each_reverse(es, &ec->surface_list, link)
		wlsc_surface_draw(es, output);

	if (ec->focus)
		wl_list_for_each(eid, &ec->input_device_list, link)
			wlsc_surface_draw(eid->sprite, output);
}

static void
repaint(void *data)
{
	struct wlsc_compositor *ec = data;
	struct wlsc_output *output;

	if (!ec->repaint_needed) {
		ec->repaint_on_timeout = 0;
		return;
	}

	wl_list_for_each(output, &ec->output_list, link)
		wlsc_output_repaint(output);

	ec->present(ec);

	ec->repaint_needed = 0;
}

void
wlsc_compositor_schedule_repaint(struct wlsc_compositor *compositor)
{
	compositor->repaint_needed = 1;
	if (compositor->repaint_on_timeout)
		return;

	wl_event_source_timer_update(compositor->timer_source, 1);
	compositor->repaint_on_timeout = 1;
}

static void
surface_destroy(struct wl_client *client,
		struct wl_surface *surface)
{
	wl_resource_destroy(&surface->resource, client);
}

static void
surface_attach(struct wl_client *client,
	       struct wl_surface *surface, struct wl_buffer *buffer,
	       int32_t x, int32_t y)
{
	struct wlsc_surface *es = (struct wlsc_surface *) surface;

	buffer->attach(buffer, surface);
	es->buffer = buffer;
	es->x += x;
	es->y += y;
	es->width = buffer->width;
	es->height = buffer->height;
	wlsc_surface_update_matrix(es);
}

static void
surface_map_toplevel(struct wl_client *client,
		     struct wl_surface *surface)
{
	struct wlsc_surface *es = (struct wlsc_surface *) surface;

	if (es->mapped)
		return;

	es->x = 10 + random() % 400;
	es->y = 10 + random() % 400;

	wlsc_surface_update_matrix(es);
	wl_list_insert(&es->compositor->surface_list, &es->link);
	wlsc_compositor_schedule_repaint(es->compositor);
	es->mapped = 1;
}

static void
surface_map_transient(struct wl_client *client,
		      struct wl_surface *surface, struct wl_surface *parent,
		      int x, int y, uint32_t flags)
{
	struct wlsc_surface *es = (struct wlsc_surface *) surface;
	struct wlsc_surface *pes = (struct wlsc_surface *) parent;

	if (es->mapped)
		return;

	es->x = pes->x + x;
	es->y = pes->y + y;

	wlsc_surface_update_matrix(es);
	wl_list_insert(&es->compositor->surface_list, &es->link);
	wlsc_compositor_schedule_repaint(es->compositor);
	es->mapped = 1;
}

static void
surface_damage(struct wl_client *client,
	       struct wl_surface *surface,
	       int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wlsc_surface *es = (struct wlsc_surface *) surface;

	es->buffer->damage(es->buffer, surface, x, y, width, height);
	wlsc_compositor_schedule_repaint(es->compositor);
}

const static struct wl_surface_interface surface_interface = {
	surface_destroy,
	surface_attach,
	surface_map_toplevel,
	surface_map_transient,
	surface_damage
};

static void
wlsc_input_device_attach(struct wlsc_input_device *device,
			 struct wl_buffer *buffer, int x, int y)
{
	struct wlsc_compositor *ec =
		(struct wlsc_compositor *) device->input_device.compositor;

	buffer->attach(buffer, &device->sprite->surface);
	device->hotspot_x = x;
	device->hotspot_y = y;

	device->sprite->x = device->input_device.x - device->hotspot_x;
	device->sprite->y = device->input_device.y - device->hotspot_y;
	device->sprite->width = buffer->width;
	device->sprite->height = buffer->height;
	wlsc_surface_update_matrix(device->sprite);

	wlsc_compositor_schedule_repaint(ec);
}


void
wlsc_input_device_set_pointer_image(struct wlsc_input_device *device,
				    enum wlsc_pointer_type type)
{
	struct wlsc_compositor *compositor =
		(struct wlsc_compositor *) device->input_device.compositor;

	wlsc_input_device_attach(device,
				 compositor->pointer_buffers[type],
				 pointer_images[type].hotspot_x,
				 pointer_images[type].hotspot_y);
}

static void
compositor_create_surface(struct wl_client *client,
			  struct wl_compositor *compositor, uint32_t id)
{
	struct wlsc_compositor *ec = (struct wlsc_compositor *) compositor;
	struct wlsc_surface *surface;

	surface = wlsc_surface_create(ec, 0, 0, 0, 0);
	if (surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	surface->surface.resource.destroy = destroy_surface;

	surface->surface.resource.object.id = id;
	surface->surface.resource.object.interface = &wl_surface_interface;
	surface->surface.resource.object.implementation =
		(void (**)(void)) &surface_interface;
	surface->surface.client = client;

	wl_client_add_resource(client, &surface->surface.resource);
}

const static struct wl_compositor_interface compositor_interface = {
	compositor_create_surface,
};

static void
wlsc_surface_transform(struct wlsc_surface *surface,
		       int32_t x, int32_t y, int32_t *sx, int32_t *sy)
{
	struct wlsc_vector v = { { x, y, 0, 1 } };

	wlsc_matrix_transform(&surface->matrix_inv, &v);
	*sx = v.f[0] * surface->width;
	*sy = v.f[1] * surface->height;
}

struct wlsc_surface *
pick_surface(struct wl_input_device *device, int32_t *sx, int32_t *sy)
{
	struct wlsc_compositor *ec =
		(struct wlsc_compositor *) device->compositor;
	struct wlsc_surface *es;

	wl_list_for_each(es, &ec->surface_list, link) {
		wlsc_surface_transform(es, device->x, device->y, sx, sy);
		if (0 <= *sx && *sx < es->width &&
		    0 <= *sy && *sy < es->height)
			return es;
	}

	return NULL;
}


static void
motion_grab_motion(struct wl_grab *grab,
		   uint32_t time, int32_t x, int32_t y)
{
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) grab->input_device;
	struct wlsc_surface *es =
		(struct wlsc_surface *) device->input_device.pointer_focus;
	int32_t sx, sy;

	wlsc_surface_transform(es, x, y, &sx, &sy);
	wl_client_post_event(es->surface.client,
			     &device->input_device.object,
			     WL_INPUT_DEVICE_MOTION,
			     time, x, y, sx, sy);
}

static void
motion_grab_button(struct wl_grab *grab,
		   uint32_t time, int32_t button, int32_t state)
{
	wl_client_post_event(grab->input_device->pointer_focus->client,
			     &grab->input_device->object,
			     WL_INPUT_DEVICE_BUTTON,
			     time, button, state);
}

static void
motion_grab_end(struct wl_grab *grab, uint32_t time)
{
}

static const struct wl_grab_interface motion_grab_interface = {
	motion_grab_motion,
	motion_grab_button,
	motion_grab_end
};

void
notify_motion(struct wl_input_device *device, uint32_t time, int x, int y)
{
	struct wlsc_surface *es;
	struct wlsc_compositor *ec =
		(struct wlsc_compositor *) device->compositor;
	struct wlsc_output *output;
	const struct wl_grab_interface *interface;
	struct wlsc_input_device *wd = (struct wlsc_input_device *) device;
	int32_t sx, sy;

	/* FIXME: We need some multi head love here. */
	output = container_of(ec->output_list.next, struct wlsc_output, link);
	if (x < output->x)
		x = 0;
	if (y < output->y)
		y = 0;
	if (x >= output->x + output->width)
		x = output->x + output->width - 1;
	if (y >= output->y + output->height)
		y = output->y + output->height - 1;

	device->x = x;
	device->y = y;

	if (device->grab) {
		interface = device->grab->interface;
		interface->motion(device->grab, time, x, y);
	} else {
		es = pick_surface(device, &sx, &sy);
		wl_input_device_set_pointer_focus(device,
						  &es->surface,
						  time, x, y, sx, sy);
		if (es)
			wl_client_post_event(es->surface.client,
					     &device->object,
					     WL_INPUT_DEVICE_MOTION,
					     time, x, y, sx, sy);
	}

	wd->sprite->x = device->x - wd->hotspot_x;
	wd->sprite->y = device->y - wd->hotspot_y;
	wlsc_surface_update_matrix(wd->sprite);

	wlsc_compositor_schedule_repaint(ec);
}

void
notify_button(struct wl_input_device *device,
	      uint32_t time, int32_t button, int32_t state)
{
	struct wlsc_input_device *wd = (struct wlsc_input_device *) device;
	struct wlsc_surface *surface;
	struct wlsc_compositor *compositor =
		(struct wlsc_compositor *) device->compositor;

	surface = (struct wlsc_surface *) device->pointer_focus;

	if (state && surface && device->grab == NULL) {
		wlsc_surface_raise(surface);

		if (wd->selection)
			wlsc_selection_set_focus(wd->selection,
						 &surface->surface, time);

		wl_input_device_start_grab(device,
					   &device->motion_grab,
					   button, time);
		wl_input_device_set_keyboard_focus(device,
						   &surface->surface,
						   time);
	}

	if (state && surface && button == BTN_LEFT &&
	    (wd->modifier_state & MODIFIER_SUPER))
		shell_move(NULL,
			   (struct wl_shell *) &compositor->shell,
			   &surface->surface, device, time);
	else if (state && surface && button == BTN_MIDDLE &&
		 (wd->modifier_state & MODIFIER_SUPER))
		shell_resize(NULL,
			     (struct wl_shell *) &compositor->shell,
			     &surface->surface, device, time,
			     WL_SHELL_RESIZE_BOTTOM_RIGHT);

	if (device->grab)
		device->grab->interface->button(device->grab, time,
						button, state);

	if (!state && device->grab && device->grab_button == button)
		wl_input_device_end_grab(device, time);
}

void
notify_key(struct wl_input_device *device,
	   uint32_t time, uint32_t key, uint32_t state)
{
	struct wlsc_input_device *wd = (struct wlsc_input_device *) device;
	struct wlsc_compositor *compositor =
		(struct wlsc_compositor *) device->compositor;
	uint32_t *k, *end;
	uint32_t modifier;

	switch (key | wd->modifier_state) {
	case KEY_BACKSPACE | MODIFIER_CTRL | MODIFIER_ALT:
		wl_display_terminate(compositor->wl_display);
		return;
	}

	switch (key) {
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		modifier = MODIFIER_CTRL;
		break;

	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		modifier = MODIFIER_ALT;
		break;

	case KEY_LEFTMETA:
	case KEY_RIGHTMETA:
		modifier = MODIFIER_SUPER;
		break;

	default:
		modifier = 0;
		break;
	}

	if (state)
		wd->modifier_state |= modifier;
	else
		wd->modifier_state &= ~modifier;

	end = device->keys.data + device->keys.size;
	for (k = device->keys.data; k < end; k++) {
		if (*k == key)
			*k = *--end;
	}
	device->keys.size = (void *) end - device->keys.data;
	if (state) {
		k = wl_array_add(&device->keys, sizeof *k);
		*k = key;
	}

	if (device->keyboard_focus != NULL)
		wl_client_post_event(device->keyboard_focus->client,
				     &device->object,
				     WL_INPUT_DEVICE_KEY, time, key, state);
}

void
notify_pointer_focus(struct wl_input_device *device,
		     uint32_t time, struct wlsc_output *output,
		     int32_t x, int32_t y)
{
	struct wlsc_input_device *wd = (struct wlsc_input_device *) device;
	struct wlsc_compositor *compositor =
		(struct wlsc_compositor *) device->compositor;
	struct wlsc_surface *es;
	int32_t sx, sy;

	if (output) {
		device->x = x;
		device->y = y;
		es = pick_surface(device, &sx, &sy);
		wl_input_device_set_pointer_focus(device,
						  &es->surface,
						  time, x, y, sx, sy);

		compositor->focus = 1;

		wd->sprite->x = device->x - wd->hotspot_x;
		wd->sprite->y = device->y - wd->hotspot_y;
		wlsc_surface_update_matrix(wd->sprite);
	} else {
		wl_input_device_set_pointer_focus(device, NULL,
						  time, 0, 0, 0, 0);
		compositor->focus = 0;
	}

	wlsc_compositor_schedule_repaint(compositor);
}

void
notify_keyboard_focus(struct wl_input_device *device_base,
		      uint32_t time, struct wlsc_output *output,
		      struct wl_array *keys)
{
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) device_base;

	if (output) {
		wl_array_copy(&device->input_device.keys, keys);
		wl_input_device_set_keyboard_focus(&device->input_device,
						   device->saved_keyboard_focus,
						   time);
	} else {
		device->saved_keyboard_focus =
			device->input_device.keyboard_focus;
		wl_input_device_set_keyboard_focus(&device->input_device,
						   NULL, time);
	}
}


static void
input_device_attach(struct wl_client *client,
		    struct wl_input_device *device_base,
		    uint32_t time,
		    struct wl_buffer *buffer, int32_t x, int32_t y)
{
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) device_base;

	if (time < device->input_device.pointer_focus_time)
		return;
	if (device->input_device.pointer_focus == NULL)
		return;
	if (device->input_device.pointer_focus->client != client)
		return;

	if (buffer == NULL) {
		wlsc_input_device_set_pointer_image(device,
						    WLSC_POINTER_LEFT_PTR);
		return;
	}

	wlsc_input_device_attach(device, buffer, x, y);
}

const static struct wl_input_device_interface input_device_interface = {
	input_device_attach,
};

void
wlsc_input_device_init(struct wlsc_input_device *device,
		       struct wlsc_compositor *ec)
{
	wl_input_device_init(&device->input_device, &ec->compositor);

	device->input_device.object.interface = &wl_input_device_interface;
	device->input_device.object.implementation =
		(void (**)(void)) &input_device_interface;
	wl_display_add_object(ec->wl_display, &device->input_device.object);
	wl_display_add_global(ec->wl_display, &device->input_device.object, NULL);

	device->sprite = wlsc_surface_create(ec,
					     device->input_device.x,
					     device->input_device.y, 32, 32);
	device->hotspot_x = 16;
	device->hotspot_y = 16;

	device->input_device.motion_grab.interface = &motion_grab_interface;

	wl_list_insert(ec->input_device_list.prev, &device->link);

	wlsc_input_device_set_pointer_image(device, WLSC_POINTER_LEFT_PTR);
}

static void
wlsc_output_post_geometry(struct wl_client *client, struct wl_object *global)
{
	struct wlsc_output *output =
		container_of(global, struct wlsc_output, object);

	wl_client_post_event(client, global,
			     WL_OUTPUT_GEOMETRY,
			     output->x, output->y,
			     output->width, output->height);
}

static const char vertex_shader[] =
	"uniform mat4 proj;\n"
	"attribute vec4 position;\n"
	"attribute vec2 texcoord;\n"
	"varying vec2 v_texcoord;\n"
	"void main()\n"
	"{\n"
	"   gl_Position = proj * position;\n"
	"   v_texcoord = texcoord;\n"
	"}\n";

static const char fragment_shader[] =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"void main()\n"
	"{\n"
	"   gl_FragColor = texture2D(tex, v_texcoord)\n;"
	"}\n";

static int
init_shaders(struct wlsc_compositor *ec)
{
	GLuint v, f, program;
	const char *p;
	char msg[512];
	GLfloat vertices[4 * 5];
	GLint status;

	p = vertex_shader;
	v = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(v, 1, &p, NULL);
	glCompileShader(v);
	glGetShaderiv(v, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(v, sizeof msg, NULL, msg);
		fprintf(stderr, "vertex shader info: %s\n", msg);
		return -1;
	}

	p = fragment_shader;
	f = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(f, 1, &p, NULL);
	glCompileShader(f);
	glGetShaderiv(f, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(f, sizeof msg, NULL, msg);
		fprintf(stderr, "fragment shader info: %s\n", msg);
		return -1;
	}

	program = glCreateProgram();
	glAttachShader(program, v);
	glAttachShader(program, f);
	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "texcoord");

	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(program, sizeof msg, NULL, msg);
		fprintf(stderr, "link info: %s\n", msg);
		return -1;
	}

	glUseProgram(program);
	ec->proj_uniform = glGetUniformLocation(program, "proj");
	ec->tex_uniform = glGetUniformLocation(program, "tex");

	vertices[ 0] = 0.0;
	vertices[ 1] = 0.0;
	vertices[ 2] = 0.0;
	vertices[ 3] = 0.0;
	vertices[ 4] = 0.0;

	vertices[ 5] = 0.0;
	vertices[ 6] = 1.0;
	vertices[ 7] = 0.0;
	vertices[ 8] = 0.0;
	vertices[ 9] = 1.0;

	vertices[10] = 1.0;
	vertices[11] = 0.0;
	vertices[12] = 0.0;
	vertices[13] = 1.0;
	vertices[14] = 0.0;

	vertices[15] = 1.0;
	vertices[16] = 1.0;
	vertices[17] = 0.0;
	vertices[18] = 1.0;
	vertices[19] = 1.0;

	glGenBuffers(1, &ec->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, ec->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);

	return 0;
}

void
wlsc_output_init(struct wlsc_output *output, struct wlsc_compositor *c,
		 int x, int y, int width, int height)
{
	output->compositor = c;
	output->x = x;
	output->y = y;
	output->width = width;
	output->height = height;

	output->background =
		background_create(output, option_background);

	wlsc_matrix_init(&output->matrix);
	wlsc_matrix_translate(&output->matrix,
			      -output->x - output->width / 2.0,
			      -output->y - output->height / 2.0, 0);
	wlsc_matrix_scale(&output->matrix,
			  2.0 / output->width, 2.0 / output->height, 1);

	output->object.interface = &wl_output_interface;
	wl_display_add_object(c->wl_display, &output->object);
	wl_display_add_global(c->wl_display, &output->object,
			      wlsc_output_post_geometry);
}

int
wlsc_compositor_init(struct wlsc_compositor *ec, struct wl_display *display)
{
	struct wl_event_loop *loop;
	const char *extensions;

	ec->wl_display = display;

	wl_compositor_init(&ec->compositor, &compositor_interface, display);

	wlsc_shm_init(ec);

	wlsc_shell_init(ec);

	wl_list_init(&ec->surface_list);
	wl_list_init(&ec->input_device_list);
	wl_list_init(&ec->output_list);

	create_pointer_images(ec);

	screenshooter_create(ec);

	extensions = (const char *) glGetString(GL_EXTENSIONS);
	if (!strstr(extensions, "GL_EXT_texture_format_BGRA8888")) {
		fprintf(stderr,
			"GL_EXT_texture_format_BGRA8888 not available\n");
		return -1;
	}

	glGenFramebuffers(1, &ec->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ec->fbo);
	glActiveTexture(GL_TEXTURE0);
	if (init_shaders(ec) < 0)
		return -1;

	loop = wl_display_get_event_loop(ec->wl_display);
	ec->timer_source = wl_event_loop_add_timer(loop, repaint, ec);
	wlsc_compositor_schedule_repaint(ec);

	return 0;
}

static void on_term_signal(int signal_number, void *data)
{
	struct wlsc_compositor *ec = data;

	wl_display_terminate(ec->wl_display);
}

int main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wlsc_compositor *ec;
	struct wl_event_loop *loop;
	GError *error = NULL;
	GOptionContext *context;
	int width, height;

	g_type_init(); /* GdkPixbuf needs this, it seems. */

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, option_entries, "Wayland");
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		fprintf(stderr, "option parsing failed: %s\n", error->message);
		exit(EXIT_FAILURE);
	}
	if (sscanf(option_geometry, "%dx%d", &width, &height) != 2) {
		fprintf(stderr, "invalid geometry option: %s \n",
			option_geometry);
		exit(EXIT_FAILURE);
	}

	display = wl_display_create();

	ec = NULL;

#if BUILD_WAYLAND_COMPOSITOR
	if (getenv("WAYLAND_DISPLAY"))
		ec = wayland_compositor_create(display, width, height);
#endif

#if BUILD_X11_COMPOSITOR
	if (ec == NULL && getenv("DISPLAY"))
		ec = x11_compositor_create(display, width, height);
#endif

#if BUILD_DRM_COMPOSITOR
	if (ec == NULL)
		ec = drm_compositor_create(display, option_connector);
#endif

	if (ec == NULL) {
		fprintf(stderr, "failed to create compositor\n");
		exit(EXIT_FAILURE);
	}

	if (wl_display_add_socket(display, option_socket_name)) {
		fprintf(stderr, "failed to add socket: %m\n");
		exit(EXIT_FAILURE);
	}

	loop = wl_display_get_event_loop(ec->wl_display);
	wl_event_loop_add_signal(loop, SIGTERM, on_term_signal, ec);
	wl_event_loop_add_signal(loop, SIGINT, on_term_signal, ec);

	wl_display_run(display);

	wl_display_destroy(display);

	ec->destroy(ec);

	return 0;
}
