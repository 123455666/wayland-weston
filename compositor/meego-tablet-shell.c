/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <linux/input.h>

#include "compositor.h"
#include "meego-tablet-server-protocol.h"

/*
 * TODO: Don't fade back from black until we've received a lockscreen
 * attachment.
 */

enum {
	STATE_STARTING,
	STATE_LOCKED,
	STATE_HOME,
	STATE_SWITCHER,
	STATE_TASK
};

struct meego_tablet_shell {
	struct wl_object object;

	struct wlsc_shell shell;

	struct wlsc_compositor *compositor;
	struct wl_event_source *sigchld_source;
	pid_t pid;
	struct wlsc_input_device *device;
	struct wl_client *client;

	struct wlsc_surface *surface;

	struct wlsc_surface *lockscreen_surface;
	struct wl_listener lockscreen_listener;

	struct wlsc_surface *home_surface;

	struct wlsc_surface *switcher_surface;
	struct wl_listener switcher_listener;

	struct meego_tablet_client *current_client;

	int state, previous_state;
	int long_press_active;
	struct wl_event_source *long_press_source;
};

struct meego_tablet_client {
	struct wl_resource resource;
	struct meego_tablet_shell *shell;
	struct wl_client *client;
	struct wlsc_surface *surface;
	char *name;
};

struct meego_tablet_zoom {
	struct wlsc_surface *surface;
	struct wlsc_animation animation;
	struct wlsc_spring spring;
	struct wlsc_transform transform;
	struct wl_listener listener;
};

static int
sigchld_handler(int signal_number, void *data)
{
	struct meego_tablet_shell *shell = data;
	int status;

	wait(&status);
	shell->pid = 0;

	fprintf(stderr, "meego-ux-daemon crashed, exit code %d\n", status);

	return 1;
}

static void
meego_tablet_shell_set_state(struct meego_tablet_shell *shell, int state)
{
	static const char *states[] = {
		"STARTING", "LOCKED", "HOME", "SWITCHER", "TASK"
	};

	fprintf(stderr, "switching to state %s (from %s)\n",
		states[state], states[shell->state]);
	shell->previous_state = shell->state;
	shell->state = state;
}

static void
handle_zoom_surface_destroy(struct wl_listener *listener,
			    struct wl_surface *surface, uint32_t time)
{
	struct meego_tablet_zoom *zoom =
		container_of(listener, struct meego_tablet_zoom, listener);

	wl_list_remove(&zoom->animation.link);
	fprintf(stderr, "animation surface gone\n");
	free(zoom);
}

static void
meego_tablet_zoom_frame(struct wlsc_animation *animation,
			struct wlsc_output *output, uint32_t msecs)
{
	struct meego_tablet_zoom *zoom =
		container_of(animation, struct meego_tablet_zoom, animation);
	struct wlsc_surface *es = zoom->surface;
	GLfloat scale;

	wlsc_spring_update(&zoom->spring, msecs);

	if (wlsc_spring_done(&zoom->spring)) {
		wl_list_remove(&animation->link);
		fprintf(stderr, "animation done\n");
		es->transform = NULL;
		free(zoom);
	}

	scale = zoom->spring.current;
	wlsc_matrix_init(&zoom->transform.matrix);
	wlsc_matrix_translate(&zoom->transform.matrix,
			      -es->width / 2.0, -es->height / 2.0, 0);
	wlsc_matrix_scale(&zoom->transform.matrix, scale, scale, scale);
	wlsc_matrix_translate(&zoom->transform.matrix,
			      es->width / 2.0, es->height / 2.0, 0);

	scale = 1.0 / zoom->spring.current;
	wlsc_matrix_init(&zoom->transform.inverse);
	wlsc_matrix_scale(&zoom->transform.inverse, scale, scale, scale);

	wlsc_surface_damage(es);
}

static void
meego_tablet_zoom_run(struct meego_tablet_shell *shell,
		      struct wlsc_surface *surface)
{
	struct meego_tablet_zoom *zoom;
	GLfloat scale;

	fprintf(stderr, "starting animation for surface %p\n", surface);

	zoom = malloc(sizeof *zoom);
	if (!zoom)
		return;

	zoom->surface = surface;
	surface->transform = &zoom->transform;
	scale = 0.3;
	wlsc_spring_init(&zoom->spring, 100.0, scale, 1.0);
	zoom->spring.timestamp = wlsc_compositor_get_time();
	zoom->animation.frame = meego_tablet_zoom_frame;
	meego_tablet_zoom_frame(&zoom->animation, NULL,
				zoom->spring.timestamp);

	zoom->listener.func = handle_zoom_surface_destroy;
	wl_list_insert(surface->surface.destroy_listener_list.prev,
		       &zoom->listener.link);

	wl_list_insert(shell->compositor->animation_list.prev,
		       &zoom->animation.link);
}

/* FIXME: We should be handling map, not attach...  Map is when the
 * surface becomes visible, which is what we want to catch.  Attach
 * will happen whenever the surface changes. */

static void
meego_tablet_shell_attach(struct wlsc_shell *base,
			  struct wlsc_surface *surface)
{
	struct meego_tablet_shell *shell =
		container_of(base, struct meego_tablet_shell, shell);

	if (surface == shell->lockscreen_surface) {
		if (shell->state == STATE_STARTING) {
			wlsc_compositor_fade(shell->compositor, 0.0);
			wlsc_compositor_wake(shell->compositor);
			wlsc_surface_damage(surface);
			meego_tablet_shell_set_state(shell, STATE_LOCKED);
			shell->previous_state = STATE_HOME;
		}
	} else if (surface == shell->switcher_surface) {
		/* */
	} else if (surface == shell->home_surface) {
		/* */
	} else if (shell->current_client->surface != surface &&
		   shell->current_client->client == surface->surface.client) {
		fprintf(stderr, "initial attach from surface %p, client %p\n",
			surface, surface->surface.client);
		meego_tablet_shell_set_state(shell, STATE_TASK);
		shell->current_client->surface = surface;
		surface->x = 0;
		surface->y = 0;
		meego_tablet_zoom_run(shell, surface);
	}
}

static void
handle_lockscreen_surface_destroy(struct wl_listener *listener,
				struct wl_surface *surface, uint32_t time)
{
	struct meego_tablet_shell *shell =
		container_of(listener,
			     struct meego_tablet_shell, lockscreen_listener);

	shell->lockscreen_surface = NULL;
	meego_tablet_shell_set_state(shell, shell->previous_state);
}

static void
tablet_shell_set_lockscreen(struct wl_client *client,
			    struct meego_tablet_shell *shell,
			    struct wl_surface *surface)
{
	struct wlsc_surface *es = (struct wlsc_surface *) surface;
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) shell->compositor->input_device;

	fprintf(stderr, "set lock screen\n");
	es->x = 0;
	es->y = 0;
	wlsc_surface_activate(es, device, wlsc_compositor_get_time());
	shell->lockscreen_surface = es;

	shell->lockscreen_listener.func = handle_lockscreen_surface_destroy;
	wl_list_insert(es->surface.destroy_listener_list.prev,
		       &shell->lockscreen_listener.link);
}

static void
handle_switcher_surface_destroy(struct wl_listener *listener,
				struct wl_surface *surface, uint32_t time)
{
	struct meego_tablet_shell *shell =
		container_of(listener,
			     struct meego_tablet_shell, switcher_listener);

	shell->switcher_surface = NULL;
	if (shell->state != STATE_LOCKED)
		meego_tablet_shell_set_state(shell, shell->previous_state);
}

static void
tablet_shell_set_switcher(struct wl_client *client,
			  struct meego_tablet_shell *shell,
			  struct wl_surface *surface)
{
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) shell->compositor->input_device;
	struct wlsc_surface *es = (struct wlsc_surface *) surface;

	/* FIXME: Switcher should be centered and the compositor
	 * should do the tinting of the background.  With the cache
	 * layer idea, we should be able to hit the framerate on the
	 * fade/zoom in. */
	shell->switcher_surface = es;
	shell->switcher_surface->x = 0;
	shell->switcher_surface->y = 0;

	wlsc_surface_activate(es, device, wlsc_compositor_get_time());

	fprintf(stderr, "set switcher %p, size %dx%d\n",
		es, es->width, es->height);

	shell->switcher_listener.func = handle_switcher_surface_destroy;
	wl_list_insert(es->surface.destroy_listener_list.prev,
		       &shell->switcher_listener.link);
}

static void
tablet_shell_set_homescreen(struct wl_client *client,
			    struct meego_tablet_shell *shell,
			    struct wl_surface *surface)
{
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) shell->compositor->input_device;

	shell->home_surface = (struct wlsc_surface *) surface;
	shell->home_surface->x = 0;
	shell->home_surface->y = 0;

	wlsc_surface_activate(shell->home_surface, device,
			      wlsc_compositor_get_time());
	fprintf(stderr, "set home screen\n");
}

static void
tablet_shell_show_grid(struct wl_client *client,
		       struct meego_tablet_shell *shell,
		       struct wl_surface *surface)
{
	fprintf(stderr, "tablet_shell_show_grid\n");
}

static void
tablet_shell_show_panels(struct wl_client *client,
			 struct meego_tablet_shell *shell,
			 struct wl_surface *surface)
{
	fprintf(stderr, "tablet_shell_show_panels\n");
}

static void
destroy_tablet_client(struct wl_resource *resource, struct wl_client *client)
{
	struct meego_tablet_client *tablet_client =
		container_of(resource, struct meego_tablet_client, resource);

	free(tablet_client->name);
	free(tablet_client);
}

static void
tablet_client_poke(struct wl_client *client,
		   struct meego_tablet_client *tablet_client)
{
	struct wlsc_compositor *compositor = tablet_client->shell->compositor;
	struct meego_tablet_shell *shell = tablet_client->shell;
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) compositor->input_device;

	fprintf(stderr, "poke client %s\n", tablet_client->name);
	shell->current_client = tablet_client;
	if (tablet_client->surface) {
		wlsc_surface_activate(tablet_client->surface, device,
				      wlsc_compositor_get_time());
		meego_tablet_shell_set_state(tablet_client->shell,
					     STATE_TASK);
	}
}

static const struct meego_tablet_client_interface tablet_client_interface = {
	tablet_client_poke
};

static void
tablet_shell_create_client(struct wl_client *client,
			   struct meego_tablet_shell *shell,
			   uint32_t id, const char *name, int fd)
{
	struct wlsc_compositor *compositor = shell->compositor;
	struct meego_tablet_client *tablet_client;

	tablet_client = malloc(sizeof *tablet_client);
	if (tablet_client == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	tablet_client->client = wl_client_create(compositor->wl_display, fd);
	tablet_client->shell = shell;
	tablet_client->name = strdup(name);

	tablet_client->resource.destroy = destroy_tablet_client;
	tablet_client->resource.object.id = id;
	tablet_client->resource.object.interface =
		&meego_tablet_client_interface;
	tablet_client->resource.object.implementation =
		(void (**)(void)) &tablet_client_interface;

	wl_client_add_resource(client, &tablet_client->resource);
	tablet_client->surface = NULL;
	shell->current_client = tablet_client;

	fprintf(stderr, "created client %p, id %d, name %s, fd %d\n",
		tablet_client->client, id, name, fd);
}

static const struct meego_tablet_shell_interface tablet_shell_interface = {
	tablet_shell_set_lockscreen,
	tablet_shell_set_switcher,
	tablet_shell_set_homescreen,
	tablet_shell_show_grid,
	tablet_shell_show_panels,
	tablet_shell_create_client
};

static void
launch_ux_daemon(struct meego_tablet_shell *shell)
{
	struct wlsc_compositor *compositor = shell->compositor;
	char s[32];
	int sv[2], flags;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		fprintf(stderr, "socketpair failed\n");
		return;
	}

	shell->pid = fork();
	switch (shell->pid) {
	case 0:
		/* SOCK_CLOEXEC closes both ends, so we need to unset
		 * the flag on the client fd. */
		flags = fcntl(sv[1], F_GETFD);
		if (flags != -1)
			fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);

		snprintf(s, sizeof s, "%d", sv[1]);
		setenv("WAYLAND_SOCKET", s, 1);
		setenv("EGL_PLATFORM", "wayland", 1);
		setenv("QT_QPA_PLATFORM", "waylandgl", 1);
		if (execl("/usr/libexec/meego-ux-daemon",
			  "/usr/libexec/meego-ux-daemon", NULL) < 0)
			fprintf(stderr, "exec failed: %m\n");
		exit(-1);

	default:
		close(sv[1]);
		shell->client =
			wl_client_create(compositor->wl_display, sv[0]);
		break;

	case -1:
		fprintf(stderr, "failed to fork\n");
		break;
	}
}

static void
toggle_switcher(struct meego_tablet_shell *shell)
{
	switch (shell->state) {
	case STATE_SWITCHER:
		wl_client_post_event(shell->client, &shell->object,
				     MEEGO_TABLET_SHELL_HIDE_SWITCHER);
		break;
	default:
		wl_client_post_event(shell->client, &shell->object,
				     MEEGO_TABLET_SHELL_SHOW_SWITCHER);
		meego_tablet_shell_set_state(shell, STATE_SWITCHER);
		break;
	}
}

static void
meego_tablet_shell_lock(struct wlsc_shell *base)
{
	struct meego_tablet_shell *shell =
		container_of(base, struct meego_tablet_shell, shell);

	if (shell->state == STATE_LOCKED)
		return;
	if (shell->state == STATE_SWITCHER)
		wl_client_post_event(shell->client, &shell->object,
				     MEEGO_TABLET_SHELL_HIDE_SWITCHER);

	wl_client_post_event(shell->client, &shell->object,
			     MEEGO_TABLET_SHELL_SHOW_LOCKSCREEN);
	
	meego_tablet_shell_set_state(shell, STATE_LOCKED);
}

static void
go_home(struct meego_tablet_shell *shell)
{
	struct wlsc_input_device *device =
		(struct wlsc_input_device *) shell->compositor->input_device;

	if (shell->state == STATE_SWITCHER)
		wl_client_post_event(shell->client, &shell->object,
				     MEEGO_TABLET_SHELL_HIDE_SWITCHER);

	wlsc_surface_activate(shell->home_surface, device,
			      wlsc_compositor_get_time());

	meego_tablet_shell_set_state(shell, STATE_HOME);
}

static int
long_press_handler(void *data)
{
	struct meego_tablet_shell *shell = data;

	shell->long_press_active = 0;
	toggle_switcher(shell);

	return 1;
}

static void
menu_key_binding(struct wl_input_device *device, uint32_t time,
		 uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct meego_tablet_shell *shell = data;

	if (shell->state == STATE_LOCKED)
		return;

	if (state)
		toggle_switcher(shell);
}

static void
home_key_binding(struct wl_input_device *device, uint32_t time,
		 uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct meego_tablet_shell *shell = data;

	if (shell->state == STATE_LOCKED)
		return;

	shell->device = (struct wlsc_input_device *) device;

	if (state) {
		wl_event_source_timer_update(shell->long_press_source, 500);
		shell->long_press_active = 1;
	} else if (shell->long_press_active) {
		wl_event_source_timer_update(shell->long_press_source, 0);
		shell->long_press_active = 0;

		switch (shell->state) {
		case STATE_HOME:
		case STATE_SWITCHER:
			toggle_switcher(shell);
			break;
		default:
			go_home(shell);
			break;
		}
	}
}

static void
tablet_shell_bind(struct wl_client *client,
		  struct wl_object *object, uint32_t version)
{
	struct meego_tablet_shell *shell =
		(struct meego_tablet_shell *) object;

	wl_client_post_event(shell->client, &shell->object,
			     MEEGO_TABLET_SHELL_SHOW_LOCKSCREEN);
}

static void
meego_tablet_shell_set_selection_focus(struct wlsc_shell *shell,
				       struct wl_selection *selection,
				       struct wl_surface *surface,
				       uint32_t time)
{
}

void
shell_init(struct wlsc_compositor *compositor);

WL_EXPORT void
shell_init(struct wlsc_compositor *compositor)
{
	struct meego_tablet_shell *shell;
	struct wl_event_loop *loop;

	shell = malloc(sizeof *shell);
	if (shell == NULL)
		return;

	memset(shell, 0, sizeof *shell);
	shell->compositor = compositor;

	shell->object.interface = &meego_tablet_shell_interface;
	shell->object.implementation =
		(void (**)(void)) &tablet_shell_interface;
	wl_display_add_object(compositor->wl_display, &shell->object);

	/* FIXME: This will make the object available to all clients. */
	wl_display_add_global(compositor->wl_display,
			      &shell->object, tablet_shell_bind);

	loop = wl_display_get_event_loop(compositor->wl_display);
	shell->sigchld_source =
		wl_event_loop_add_signal(loop, SIGCHLD,
					 sigchld_handler, shell);

	shell->long_press_source =
		wl_event_loop_add_timer(loop, long_press_handler, shell);

	wlsc_compositor_add_binding(compositor, KEY_LEFTMETA, 0, 0,
				    home_key_binding, shell);
	wlsc_compositor_add_binding(compositor, KEY_RIGHTMETA, 0, 0,
				    home_key_binding, shell);
	wlsc_compositor_add_binding(compositor, KEY_LEFTMETA, 0,
				    MODIFIER_SUPER, home_key_binding, shell);
	wlsc_compositor_add_binding(compositor, KEY_RIGHTMETA, 0,
				    MODIFIER_SUPER, home_key_binding, shell);
 	wlsc_compositor_add_binding(compositor, KEY_COMPOSE, 0, 0,
				    menu_key_binding, shell);

	compositor->shell = &shell->shell;

	shell->shell.lock = meego_tablet_shell_lock;
	shell->shell.attach = meego_tablet_shell_attach;
	shell->shell.set_selection_focus =
		meego_tablet_shell_set_selection_focus;
	launch_ux_daemon(shell);

	wlsc_spring_init(&compositor->fade.spring, 40.0, 1.0, 1.0);
	meego_tablet_shell_set_state(shell, STATE_STARTING);
}
