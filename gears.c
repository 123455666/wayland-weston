/*
 * Copyright © 2008 Kristian Høgsberg
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <cairo.h>
#include <glib.h>
#include <cairo-drm.h>

#include <GL/gl.h>
#include <eagle.h>

#include "wayland-client.h"
#include "wayland-glib.h"

#include "window.h"

static const char gem_device[] = "/dev/dri/card0";
static const char socket_name[] = "\0wayland";

struct gears {
	struct window *window;

	struct display *d;
	struct wl_compositor *compositor;
	struct rectangle rectangle;

	EGLDisplay display;
	EGLConfig config;
	EGLSurface surface;
	EGLContext context;
	int resized;
	GLfloat angle;
	cairo_surface_t *cairo_surface;

	GLint gear_list[3];
};

struct gear_template {
	GLfloat material[4];
	GLfloat inner_radius;
	GLfloat outer_radius;
	GLfloat width;
	GLint teeth;
	GLfloat tooth_depth;
};

const static struct gear_template gear_templates[] = {
	{ { 0.8, 0.1, 0.0, 1.0 }, 1.0, 4.0, 1.0, 20, 0.7 },
	{ { 0.0, 0.8, 0.2, 1.0 }, 0.5, 2.0, 2.0, 10, 0.7 },
	{ { 0.2, 0.2, 1.0, 1.0 }, 1.3, 2.0, 0.5, 10, 0.7 }, 
};

static GLfloat light_pos[4] = {5.0, 5.0, 10.0, 0.0};

static void die(const char *msg)
{
	fprintf(stderr, "%s", msg);
	exit(EXIT_FAILURE);
}

static void
make_gear(const struct gear_template *t)
{
	GLint i;
	GLfloat r0, r1, r2;
	GLfloat angle, da;
	GLfloat u, v, len;

	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, t->material);

	r0 = t->inner_radius;
	r1 = t->outer_radius - t->tooth_depth / 2.0;
	r2 = t->outer_radius + t->tooth_depth / 2.0;

	da = 2.0 * M_PI / t->teeth / 4.0;

	glShadeModel(GL_FLAT);

	glNormal3f(0.0, 0.0, 1.0);

	/* draw front face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		if (i < t->teeth) {
			glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
		}
	}
	glEnd();

	/* draw front sides of teeth */
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / t->teeth / 4.0;
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), t->width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
	}
	glEnd();

	glNormal3f(0.0, 0.0, -1.0);

	/* draw back face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		if (i < t->teeth) {
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
			glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		}
	}
	glEnd();

	/* draw back sides of teeth */
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / t->teeth / 4.0;
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
	}
	glEnd();

	/* draw outward faces of teeth */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
		u = r2 * cos(angle + da) - r1 * cos(angle);
		v = r2 * sin(angle + da) - r1 * sin(angle);
		len = sqrt(u * u + v * v);
		u /= len;
		v /= len;
		glNormal3f(v, -u, 0.0);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -t->width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -t->width * 0.5);
		u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
		v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
		glNormal3f(v, -u, 0.0);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
	}

	glVertex3f(r1 * cos(0), r1 * sin(0), t->width * 0.5);
	glVertex3f(r1 * cos(0), r1 * sin(0), -t->width * 0.5);

	glEnd();

	glShadeModel(GL_SMOOTH);

	/* draw inside radius cylinder */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glNormal3f(-cos(angle), -sin(angle), 0.0);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
	}
	glEnd();
}

static void
draw_gears(struct gears *gears)
{
	GLfloat view_rotx = 20.0, view_roty = 30.0, view_rotz = 0.0;

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glPushMatrix();

	glTranslatef(0.0, 0.0, -50);

	glRotatef(view_rotx, 1.0, 0.0, 0.0);
	glRotatef(view_roty, 0.0, 1.0, 0.0);
	glRotatef(view_rotz, 0.0, 0.0, 1.0);

	glPushMatrix();
	glTranslatef(-3.0, -2.0, 0.0);
	glRotatef(gears->angle, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[0]);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(3.1, -2.0, 0.0);
	glRotatef(-2.0 * gears->angle - 9.0, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[1]);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(-3.1, 4.2, 0.0);
	glRotatef(-2.0 * gears->angle - 25.0, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[2]);
	glPopMatrix();

	glPopMatrix();

	glFlush();
}

static void
resize_window(struct gears *gears)
{
	uint32_t name, stride;

	/* Constrain child size to be square and at least 300x300 */
	window_get_child_rectangle(gears->window, &gears->rectangle);
	if (gears->rectangle.width > gears->rectangle.height)
		gears->rectangle.height = gears->rectangle.width;
	else
		gears->rectangle.width = gears->rectangle.height;
	if (gears->rectangle.width < 300) {
		gears->rectangle.width = 300;
		gears->rectangle.height = 300;
	}
	window_set_child_size(gears->window, &gears->rectangle);

	window_draw(gears->window);

	if (gears->cairo_surface != NULL)
		cairo_surface_destroy(gears->cairo_surface);

	gears->cairo_surface = window_create_surface(gears->window,
						     &gears->rectangle);

	name = cairo_drm_surface_get_name(gears->cairo_surface);
	stride = cairo_drm_surface_get_stride(gears->cairo_surface),
	gears->surface = eglCreateSurfaceForName(gears->display,
						 gears->config,
						 name,
						 gears->rectangle.width,
						 gears->rectangle.height,
						 stride, NULL);

	eglMakeCurrent(gears->display,
		       gears->surface, gears->surface, gears->context);

	glViewport(0, 0, gears->rectangle.width, gears->rectangle.height);
	gears->resized = 0;
}

static void
resize_handler(struct window *window, void *data)
{
	struct gears *gears = data;

	/* Right now, resizing the window from the per-frame callback
	 * is fine, since the window drawing code is so slow that we
	 * can't draw more than one window per frame anyway.  However,
	 * once we implement faster resizing, this will show lag
	 * between pointer motion and window size even if resizing is
	 * fast.  We need to keep processing motion events and posting
	 * new frames as fast as possible so when the server
	 * composites the next frame it will have the most recent size
	 * possible, like what we do for window moves. */

	gears->resized = 1;
}

static void
handle_acknowledge(void *data,
		   struct wl_compositor *compositor,
		   uint32_t key, uint32_t frame)
{
	struct gears *gears = data;

	if (key != 0)
		return;

	if (gears->resized)
		resize_window(gears);

	draw_gears(gears);
}

static void
handle_frame(void *data,
	     struct wl_compositor *compositor,
	     uint32_t frame, uint32_t timestamp)
{
	struct gears *gears = data;

	window_copy_surface(gears->window,
			    &gears->rectangle,
			    gears->cairo_surface);

	wl_compositor_commit(gears->compositor, 0);

	gears->angle = timestamp / 20.0;
}

static const struct wl_compositor_listener compositor_listener = {
	handle_acknowledge,
	handle_frame,
};

static const EGLint config_attribs[] = {
	EGL_DEPTH_SIZE, 24,
	EGL_CONFIG_CAVEAT, EGL_NONE,
	EGL_RED_SIZE, 8,
	EGL_NONE		
};

static struct gears *
gears_create(struct display *display)
{
	const int x = 200, y = 200, width = 450, height = 500;
	EGLint major, minor, count;
	EGLConfig configs[64];
	struct udev *udev;
	struct udev_device *device;
	struct gears *gears;
	int i;

	udev = udev_new();
	device = udev_device_new_from_syspath(udev, "/sys/class/drm/card0");

	gears = malloc(sizeof *gears);
	memset(gears, 0, sizeof *gears);
	gears->d = display;
	gears->window = window_create(display, "Wayland Gears",
				      x, y, width, height);

	gears->display = eglCreateDisplayNative(device);
	if (gears->display == NULL)
		die("failed to create egl display\n");

	if (!eglInitialize(gears->display, &major, &minor))
		die("failed to initialize display\n");

	if (!eglGetConfigs(gears->display, configs, 64, &count))
		die("failed to get configs\n");

	if (!eglChooseConfig(gears->display, config_attribs, &gears->config, 1, NULL))
		die("failed to pick a config\n");

	gears->context = eglCreateContext(gears->display, gears->config, NULL, NULL);
	if (gears->context == NULL)
		die("failed to create context\n");

	resize_window(gears);

	for (i = 0; i < 3; i++) {
		gears->gear_list[i] = glGenLists(1);
		glNewList(gears->gear_list[i], GL_COMPILE);
		make_gear(&gear_templates[i]);
		glEndList();
	}

	glEnable(GL_NORMALIZE);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -1.0, 1.0, 5.0, 200.0);
	glMatrixMode(GL_MODELVIEW);

	glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 0.92);

	gears->compositor = display_get_compositor(display);

	draw_gears(gears);

	handle_frame(gears, gears->compositor, 0, 0);

	window_set_resize_handler(gears->window, resize_handler, gears);

	wl_compositor_add_listener(gears->compositor,
				   &compositor_listener, gears);

	return gears;
}

int main(int argc, char *argv[])
{
	struct wl_display *display;
	struct display *d;
	int fd;
	GMainLoop *loop;
	GSource *source;
	struct gears *gears;

	fd = open(gem_device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "drm open failed: %m\n");
		return -1;
	}

	display = wl_display_create(socket_name, sizeof socket_name);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	d = display_create(display, fd);

	loop = g_main_loop_new(NULL, FALSE);
	source = wl_glib_source_new(display);
	g_source_attach(source, NULL);

	gears = gears_create(d);

	g_main_loop_run(loop);

	return 0;
}
