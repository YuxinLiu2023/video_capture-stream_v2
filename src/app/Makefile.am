AM_CPPFLAGS = $(CXX17_FLAGS) -I$(srcdir)/../util -I$(srcdir)/../video
FFMPEG_CFLAGS = $(shell pkg-config --cflags libavutil libswscale)
FFMPEG_LIBS = $(shell pkg-config --libs libavutil libswscale)
AM_CXXFLAGS = $(PICKY_CXXFLAGS) $(FFMPEG_CFLAGS)
BASE_LDADD = ../video/libvideo.a ../util/libutil.a \
	$(FFMPEG_LIBS) $(VPX_LIBS) $(SDL_LIBS) -lpthread


bin_PROGRAMS = video_sender video_receiver

video_sender_SOURCES = video_sender.cc \
	protocol.hh protocol.cc encoder.hh encoder.cc capture.hh capture.cc
video_sender_LDADD = $(BASE_LDADD)

video_receiver_SOURCES = video_receiver.cc \
	protocol.hh protocol.cc decoder.hh decoder.cc capture.hh capture.cc
video_receiver_LDADD = $(BASE_LDADD)
