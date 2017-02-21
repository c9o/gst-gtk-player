/*
 * Copyright (C) 2014 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#define GST_USE_UNSTABLE_API

#include <gst/video/videooverlay.h>
#include <gtk/gtk.h>
#include <cairo/cairo.h>
#include "fs-element-added-notifier.h"

#ifdef WAYLAND_DEMO
#include "wayland.h"
#include <gdk/gdkwayland.h>
#else
# include <gdk/gdkx.h>
#endif

struct AppData {
  GtkWidget *video_window;
  GtkWidget *app_window;
  GstElement *pipeline;

  GstVideoOverlay *overlay;
  guintptr window_handle;

#ifdef WAYLAND_DEMO
  GstWaylandVideo *wlvideo;
  struct wl_display *display_handle;
  GtkAllocation video_widget_allocation;
  gboolean geometry_changing;
#endif

  char **argv;
  int current_uri; /* index for argv */
};

static gchar *
find_file (const gchar * name)
{
  const gchar * const * system_dirs = g_get_system_data_dirs ();
  gchar * ret;
  while (*system_dirs) {
    ret = g_build_filename (*system_dirs, "gst-gtk-player", name, NULL);
    if (g_file_test (ret, G_FILE_TEST_EXISTS)) {
      g_print ("Found '%s' at '%s'\n", name, ret);
      return ret;
    }
    g_free (ret);
    system_dirs++;
  }
  return g_strdup (name);
}

static void
on_about_to_finish (GstElement * playbin, struct AppData * d)
{
  if (d->argv[++d->current_uri] == NULL)
    d->current_uri = 1;

  g_print ("Now playing %s\n", d->argv[d->current_uri]);
  g_object_set (playbin, "uri", d->argv[d->current_uri], NULL);
}

static GstBusSyncReply
bus_sync_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
  struct AppData *d = user_data;

#ifdef WAYLAND_DEMO
  if (gst_is_wayland_display_handle_need_context_message (message)) {
    if (d->display_handle != 0) {
      GstContext *context;

      context = gst_wayland_display_handle_context_new (d->display_handle);
      gst_element_set_context(GST_ELEMENT (GST_MESSAGE_SRC (message)), context);

      /* HACK save the pointer to the sink (which implements GstWaylandVideo)
       * from this point. Unfortunately, d->overlay can also be the playbin
       * instead of waylandsink */
      d->wlvideo = GST_WAYLAND_VIDEO (GST_MESSAGE_SRC (message));
    } else {
      g_warning ("Should have obtained display_handle by now!\n");
    }

    gst_message_unref (message);
    return GST_BUS_DROP;
  } else
#endif
  if (gst_is_video_overlay_prepare_window_handle_message (message)) {
    if (d->window_handle != 0) {
      /* GST_MESSAGE_SRC (message) will be the overlay object that we have to
       * use. This may be waylandsink, but it may also be playbin. In the latter
       * case, we must make sure to use playbin instead of waylandsink, because
       * playbin resets the window handle and render_rectangle after restarting
       * playback and the actual window size is lost */
      d->overlay = GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));

#ifdef WAYLAND_DEMO
      g_print ("setting window handle and size (%d x %d)\n",
          d->video_widget_allocation.width, d->video_widget_allocation.height);
#endif

      gst_video_overlay_set_window_handle (d->overlay, d->window_handle);
#ifdef WAYLAND_DEMO
      gst_video_overlay_set_render_rectangle (d->overlay,
          d->video_widget_allocation.x, d->video_widget_allocation.y,
          d->video_widget_allocation.width, d->video_widget_allocation.height);
#endif
    } else {
      g_warning ("Should have obtained window_handle by now!\n");
    }

    gst_message_unref (message);
    return GST_BUS_DROP;
  }

  return GST_BUS_PASS;
}

#ifdef WAYLAND_DEMO
static void
on_frame_clock_after_paint (GdkFrameClock * clock, gpointer data)
{
  struct AppData *d = data;

  if (d->geometry_changing) {
    g_print ("end geometry change\n");
    gst_wayland_video_end_geometry_change (d->wlvideo);
    d->geometry_changing = FALSE;
  }
}
#endif

static void
video_widget_realize_cb (GtkWidget * widget, gpointer data)
{
  struct AppData *d = data;
  GdkWindow *window;

  window = gtk_widget_get_window (widget);

#ifdef WAYLAND_DEMO
  {
    GdkFrameClock *frame_clock;
    GdkDisplay *display;

    display = gtk_widget_get_display (widget);

    /* Note that the surface passed to waylandsink here is the top-level
    * surface of the window, since gtk does not implement subsurfaces */
    d->display_handle = gdk_wayland_display_get_wl_display (display);
    d->window_handle = (guintptr) gdk_wayland_window_get_wl_surface (window);
    gtk_widget_get_allocation (widget, &d->video_widget_allocation);

    frame_clock = gtk_widget_get_frame_clock (widget);
    g_signal_connect_data (frame_clock, "after-paint",
        G_CALLBACK (on_frame_clock_after_paint), data, NULL, G_CONNECT_AFTER);
  }
#else
  d->window_handle = GDK_WINDOW_XID (window);
#endif
}

static gboolean
video_widget_draw_cb (GtkWidget * widget, cairo_t *cr, gpointer data)
{
#ifdef WAYLAND_DEMO
  struct AppData *d = data;

  if (!d->window_handle)
    return FALSE;

  gtk_widget_get_allocation (widget, &d->video_widget_allocation);

  g_print ("draw_cb x %d, y %d, w %d, h %d\n",
      d->video_widget_allocation.x, d->video_widget_allocation.y,
      d->video_widget_allocation.width, d->video_widget_allocation.height);

  if (d->wlvideo && d->overlay && !d->geometry_changing) {
    gst_wayland_video_begin_geometry_change (d->wlvideo);
    d->geometry_changing = TRUE;

    gst_video_overlay_set_render_rectangle (d->overlay,
        d->video_widget_allocation.x, d->video_widget_allocation.y,
        d->video_widget_allocation.width, d->video_widget_allocation.height);
  }
#endif

  return FALSE;
}

static void
playing_clicked_cb (GtkButton *button, struct AppData * d)
{
  gst_element_set_state (d->pipeline, GST_STATE_PLAYING);
}

static void
paused_clicked_cb (GtkButton *button, struct AppData * d)
{
  gst_element_set_state (d->pipeline, GST_STATE_PAUSED);
}

static void
null_clicked_cb (GtkButton *button, struct AppData * d)
{
  gst_element_set_state (d->pipeline, GST_STATE_NULL);
}

static void
build_window (struct AppData * d)
{
  GtkBuilder *builder;
  GtkWidget *button;
  gchar *window_ui;
  GError *error = NULL;

  builder = gtk_builder_new ();
  window_ui = find_file ("window.ui");
  if (!gtk_builder_add_from_file (builder, window_ui, &error)) {
    g_error ("Failed to load window.ui: %s", error->message);
    g_error_free (error);
    goto exit;
  }

  d->app_window = GTK_WIDGET (gtk_builder_get_object (builder, "window"));
  g_object_ref (d->app_window);
  g_signal_connect (d->app_window, "destroy",
      G_CALLBACK (gtk_main_quit), NULL);

  g_object_set (d->app_window, "title",
#ifdef WAYLAND_DEMO
      "GStreamer Wayland GTK Demo",
#else
      "GStreamer X11 GTK Demo",
#endif
      NULL);

  d->video_window = GTK_WIDGET (gtk_builder_get_object (builder, "videoarea"));
  g_signal_connect (d->video_window, "draw",
      G_CALLBACK (video_widget_draw_cb), d);
  g_signal_connect (d->video_window, "realize",
      G_CALLBACK (video_widget_realize_cb), d);

  button = GTK_WIDGET (gtk_builder_get_object (builder, "button_playing"));
  g_signal_connect (button, "clicked", G_CALLBACK (playing_clicked_cb), d);

  button = GTK_WIDGET (gtk_builder_get_object (builder, "button_paused"));
  g_signal_connect (button, "clicked", G_CALLBACK (paused_clicked_cb), d);

  button = GTK_WIDGET (gtk_builder_get_object (builder, "button_null"));
  g_signal_connect (button, "clicked", G_CALLBACK (null_clicked_cb), d);

exit:
  g_free (window_ui);
  g_object_unref (builder);
}

static GstElement *
get_sink (struct AppData *d)
{
  GstElement *sink;
#ifdef WAYLAND_DEMO
  sink = gst_element_factory_make ("waylandsink", NULL);
#else
  sink = gst_element_factory_make ("glimagesink", NULL);
#endif
  return gst_object_ref_sink (sink);
}

int
main (int argc, char **argv)
{
  struct AppData data = {0};
  GstBus *bus;
  FsElementAddedNotifier *notifier;
  gchar *codec_preferences_file;
  GstElement *sink;

#ifdef WAYLAND_DEMO
  gdk_set_allowed_backends ("wayland");
#else
  gdk_set_allowed_backends ("x11");
#endif

  gtk_init (&argc, &argv);
  gst_init (&argc, &argv);

  // create the window
  build_window (&data);

  // show the GUI
  gtk_widget_show_all (data.app_window);

  // realize window now so that the video window gets created and we can
  // obtain its window handle before the pipeline is started up and the video
  // sink asks for the handle of the window to render onto
  gtk_widget_realize (data.app_window);

  if (argc > 1) {
    data.argv = argv;
    data.current_uri = 1;

    data.pipeline = gst_element_factory_make ("playbin", NULL);
    g_object_set (data.pipeline, "uri", argv[data.current_uri], NULL);

    sink = get_sink (&data);
    g_object_set (data.pipeline, "video-sink", sink, NULL);
    gst_object_unref (sink);

    // enable looping
    g_signal_connect (data.pipeline, "about-to-finish",
        G_CALLBACK (on_about_to_finish), &data);
  } else {
    GstElement *src;

    data.pipeline = gst_pipeline_new (NULL);
    src = gst_element_factory_make ("videotestsrc", NULL);
    g_object_set (src, "pattern", 18, "background-color", 0x0000F000, NULL);

    sink = get_sink (&data);
    gst_bin_add_many (GST_BIN (data.pipeline), src, sink, NULL);
    gst_element_link (src, sink);
    gst_object_unref (sink);
  }

  // set up sync handler for setting the xid once the pipeline is started
  bus = gst_pipeline_get_bus (GST_PIPELINE (data.pipeline));
  gst_bus_set_sync_handler (bus, (GstBusSyncHandler) bus_sync_handler, &data,
      NULL);
  gst_object_unref (bus);

  notifier = fs_element_added_notifier_new ();
  fs_element_added_notifier_add (notifier, GST_BIN (data.pipeline));

  codec_preferences_file = find_file ("codec-properties.ini");
  fs_element_added_notifier_set_properties_from_file (notifier,
      codec_preferences_file, NULL);
  g_free (codec_preferences_file);

#ifndef WAYLAND_DEMO
  /* force GstSystemClock for measurements */
  {
    GstClock *c = gst_system_clock_obtain ();
    gst_pipeline_use_clock (GST_PIPELINE (data.pipeline), c);
    gst_object_unref (c);
  }
#endif

  // play
  gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  gtk_main ();
  gst_element_set_state (data.pipeline, GST_STATE_NULL);

  g_object_unref (notifier);
  gst_object_unref (data.pipeline);

  return 0;
}
