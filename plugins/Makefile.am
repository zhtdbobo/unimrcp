MAINTAINERCLEANFILES   = Makefile.in

EXTRA_DIST             = http-common/http_common.h http-common/http_common.c http-common/http_engine.c

SUBDIRS                =
if HTTP_TTS_PLUGIN
SUBDIRS               += http-tts
endif
if HTTP_ASR_PLUGIN
SUBDIRS               += http-asr
endif


if DEMOSYNTH_PLUGIN
SUBDIRS               += demo-synth
endif

if DEMORECOG_PLUGIN
SUBDIRS               += demo-recog
endif

if DEMOVERIFIER_PLUGIN
SUBDIRS               += demo-verifier
endif

if RECORDER_PLUGIN
SUBDIRS               += mrcp-recorder
endif
