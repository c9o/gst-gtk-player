# name
bin_PROGRAMS = gst-gtk-player

# list of source files
# the prefix is the name of the binary
gst_gtk_player_SOURCES = main.c fs-element-added-notifier.c

# list of headers we're not going to install
noinst_HEADERS = ../include/fs-element-added-notifier.h ../include/wayland.h

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
gst_gtk_player_CFLAGS = -I $(top_srcdir)/include -Wall `pkg-config --cflags gtk+-3.0 cairo gstreamer-video-1.0`
gst_gtk_player_LDFLAGS = `pkg-config --libs gtk+-3.0 cairo gstreamer-video-1.0`
