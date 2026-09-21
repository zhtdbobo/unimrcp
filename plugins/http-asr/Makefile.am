AM_CPPFLAGS = $(UNIMRCP_PLUGIN_INCLUDES) $(HTTP_SPEECH_CFLAGS) -I$(srcdir)/../http-common
plugin_LTLIBRARIES = httpasr.la
httpasr_la_SOURCES = src/http_asr_engine.c ../http-common/http_common.c
httpasr_la_LDFLAGS = $(UNIMRCP_PLUGIN_OPTS)
httpasr_la_LIBADD = $(HTTP_SPEECH_LIBS)
EXTRA_DIST = CMakeLists.txt
include $(top_srcdir)/build/rules/uniplugin.am
