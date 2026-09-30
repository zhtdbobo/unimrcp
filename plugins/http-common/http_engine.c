/* SPDX-License-Identifier: Apache-2.0
 * Shared implementation compiled into two independent UniMRCP resource plugins.
 * All HTTP and MRCP responses run on a per-channel worker. Media callbacks only
 * copy bounded PCM frames and update state; they never wait for HTTP or allocate.
 */
#include "mrcp_synth_engine.h"
#include "mrcp_recog_engine.h"
#include "http_common.h"
#include "http_stream.h"
#include <apr_atomic.h>
#include <apr_queue.h>
#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <apr_uuid.h>
#include <apr_strings.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HP_SYNTH
#define HP_RESOURCE MRCP_SYNTHESIZER_RESOURCE
#define HP_START SYNTHESIZER_SPEAK
#define HP_STOP SYNTHESIZER_STOP
#define HP_ENV "UNIMRCP_HTTP_TTS_URL"
#define HP_STREAM_ENV "UNIMRCP_STREAM_TTS_URL"
#else
#define HP_RESOURCE MRCP_RECOGNIZER_RESOURCE
#define HP_START RECOGNIZER_RECOGNIZE
#define HP_STOP RECOGNIZER_STOP
#define HP_ENV "UNIMRCP_HTTP_ASR_URL"
#define HP_STREAM_ENV "UNIMRCP_STREAM_ASR_URL"
#endif

typedef struct {
    const char *url;
    long timeout_ms, max_ms, noinput_ms, silence_ms, activity_ms, threshold;
    int curl_ready, streaming;
} http_engine;
enum { HP_IDLE, HP_CAPTURING, HP_RECOG_READY, HP_PLAYING, HP_PLAY_DONE, HP_LIMIT };
typedef struct { mrcp_message_t *request; apr_uint32_t epoch; } http_command;
typedef struct {
    mrcp_engine_channel_t *base;
    http_engine *engine;
    apr_queue_t *commands;
    apr_thread_t *worker;
    apr_thread_mutex_t *media_mutex;
    apr_thread_mutex_t *command_mutex;
    volatile apr_uint32_t closing, epoch, media_state, speech, media_fault;
    volatile apr_uint32_t active_epoch;
    mrcp_message_t *active; /* Worker-owned; never touched by the media thread. */
    hp_buffer audio; size_t position;
    hp_ring ring;
    hp_stream *transfer; /* Worker-owned; closed before freeing the ring. */
    int stream_done;
    int paused, input_timers, speech_reported;
    long noinput_ms, silence_ms, max_ms, recognition_ms;
    size_t run_samples, quiet_samples, total_samples;
    apr_time_t started, timers_started, speech_started;
    char request_id[APR_UUID_FORMATTED_LENGTH+1];
} http_channel;

static apt_bool_t engine_destroy(mrcp_engine_t *engine);
static apt_bool_t engine_open(mrcp_engine_t *engine);
static apt_bool_t engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t *channel_create(mrcp_engine_t *engine, apr_pool_t *pool);
static apt_bool_t channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t channel_request(mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t stream_destroy(mpf_audio_stream_t *stream) { (void)stream; return TRUE; }
static apt_bool_t stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec) { (void)stream; (void)codec; return TRUE; }
static apt_bool_t stream_close(mpf_audio_stream_t *stream) { (void)stream; return TRUE; }
#ifdef HP_SYNTH
static apt_bool_t stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);
static const mpf_audio_stream_vtable_t stream_vtable={stream_destroy,stream_open,stream_close,stream_read,NULL,NULL,NULL,NULL};
#else
static apt_bool_t stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);
static const mpf_audio_stream_vtable_t stream_vtable={stream_destroy,NULL,NULL,NULL,stream_open,stream_close,stream_write,NULL};
#endif
static const mrcp_engine_method_vtable_t engine_vtable={engine_destroy,engine_open,engine_close,channel_create};
static const mrcp_engine_channel_method_vtable_t channel_vtable={channel_destroy,channel_open,channel_close,channel_request};

MRCP_PLUGIN_VERSION_DECLARE
MRCP_PLUGIN_LOGGER_IMPLEMENT

static long config_number(mrcp_engine_t *engine, const char *name, long fallback, long min, long max)
{
    const char *s=mrcp_engine_param_get(engine,name); char *end; long n;
    if (!s) return fallback;
    errno=0; n=strtol(s,&end,10);
    return errno || end==s || *end || n<min || n>max ? -1 : n;
}
MRCP_PLUGIN_DECLARE(mrcp_engine_t *) mrcp_plugin_create(apr_pool_t *pool)
{
    http_engine *e=apr_pcalloc(pool,sizeof(*e));
    return mrcp_engine_create(HP_RESOURCE,e,&engine_vtable,pool);
}
static apt_bool_t engine_open(mrcp_engine_t *engine)
{
    http_engine *e=engine->obj; const char *url=getenv(HP_ENV), *stream_url=getenv(HP_STREAM_ENV); int valid, valid_url;
    if (!url || !*url) url=mrcp_engine_param_get(engine,"url");
    if (!stream_url || !*stream_url) stream_url=mrcp_engine_param_get(engine,"stream-url");
    e->streaming=stream_url && *stream_url;
    if (e->streaming) url=stream_url;
    e->url=url ? apr_pstrdup(engine->pool,url) : NULL;
    e->timeout_ms=config_number(engine,"http-timeout-ms",60000,1000,180000);
    e->max_ms=config_number(engine,"max-utterance-ms",60000,1000,120000);
    e->noinput_ms=config_number(engine,"no-input-timeout-ms",15000,100,120000);
    e->silence_ms=config_number(engine,"speech-complete-timeout-ms",1000,100,10000);
    e->activity_ms=config_number(engine,"min-speech-ms",120,10,1000);
    e->threshold=config_number(engine,"vad-threshold",500,1,32767);
    valid_url=e->url && (!strncmp(e->url,"http://",7) || !strncmp(e->url,"https://",8));
#ifndef HP_SYNTH
    if (e->streaming) valid_url=e->url && (!strncmp(e->url,"ws://",5) || !strncmp(e->url,"wss://",6));
#endif
    valid=valid_url &&
          e->timeout_ms>0 && e->max_ms>0 && e->noinput_ms>0 && e->silence_ms>0 && e->activity_ms>0 && e->threshold>0;
    if (valid) e->curl_ready=curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;
    if (!valid || !e->curl_ready) apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"HTTP speech plugin: invalid configuration or CURL initialization failed");
    return mrcp_engine_open_respond(engine,valid && e->curl_ready);
}
static apt_bool_t engine_close(mrcp_engine_t *engine) { return mrcp_engine_close_respond(engine); }
static apt_bool_t engine_destroy(mrcp_engine_t *engine)
{
    http_engine *e=engine->obj;
    if (e->curl_ready) { curl_global_cleanup(); e->curl_ready=0; }
    return TRUE;
}
static int cancelled(void *context)
{
    http_channel *c=context;
    return apr_atomic_read32(&c->closing) || apr_atomic_read32(&c->epoch)!=apr_atomic_read32(&c->active_epoch);
}
static void clear_audio(http_channel *c)
{
    hp_stream_close(c->transfer); c->transfer=NULL;
    apr_thread_mutex_lock(c->media_mutex);
    apr_atomic_set32(&c->media_state,HP_IDLE);
    hp_buffer_free(&c->audio); c->position=0; c->paused=0;
    free(c->ring.data); memset(&c->ring,0,sizeof(c->ring)); c->stream_done=0;
    apr_thread_mutex_unlock(c->media_mutex);
}
#ifdef HP_SYNTH
static int streaming_audio(void *context, const unsigned char *pcm, size_t len)
{
    http_channel *c=context; int ok;
    if (cancelled(c)) return -1;
    apr_thread_mutex_lock(c->media_mutex);
    ok=hp_ring_write(&c->ring,pcm,len);
    apr_thread_mutex_unlock(c->media_mutex);
    return ok;
}
#else
static size_t streaming_audio(void *context, unsigned char *pcm, size_t cap, int *eof)
{
    http_channel *c=context; size_t n;
    if (cancelled(c) || apr_atomic_read32(&c->media_fault)) return SIZE_MAX;
    apr_thread_mutex_lock(c->media_mutex);
    n=hp_ring_read(&c->ring,pcm,cap);
    *eof=!c->ring.len && apr_atomic_read32(&c->media_state)==HP_RECOG_READY;
    apr_thread_mutex_unlock(c->media_mutex);
    return n;
}
#endif
static void reply(http_channel *c, mrcp_message_t *request, mrcp_status_code_e status, mrcp_request_state_e state)
{
    mrcp_message_t *response=mrcp_response_create(request,request->pool);
    response->start_line.status_code=status; response->start_line.request_state=state;
    mrcp_engine_channel_message_send(c->base,response);
}
static void complete(http_channel *c, int cause, const char *text, size_t len)
{
    mrcp_message_t *event; mrcp_message_t *request=c->active;
    if (!request || cancelled(c)) return;
#ifdef HP_SYNTH
    (void)text; (void)len;
    event=mrcp_event_create(request,SYNTHESIZER_SPEAK_COMPLETE,request->pool);
    ((mrcp_synth_header_t *)mrcp_resource_header_prepare(event))->completion_cause=(mrcp_synth_completion_cause_e)cause;
    mrcp_resource_header_property_add(event,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
#else
    event=mrcp_event_create(request,RECOGNIZER_RECOGNITION_COMPLETE,request->pool);
    if (text && len) {
        char *xml=hp_nlsml(text,len);
        if (xml) {
            mrcp_generic_header_t *header=mrcp_generic_header_prepare(event);
            apt_string_assign(&header->content_type,"application/x-nlsml",event->pool);
            mrcp_generic_header_property_add(event,GENERIC_HEADER_CONTENT_TYPE);
            apt_string_assign(&event->body,xml,event->pool); free(xml);
        } else cause=RECOGNIZER_COMPLETION_CAUSE_ERROR;
    }
    ((mrcp_recog_header_t *)mrcp_resource_header_prepare(event))->completion_cause=(mrcp_recog_completion_cause_e)cause;
    mrcp_resource_header_property_add(event,RECOGNIZER_HEADER_COMPLETION_CAUSE);
#endif
    event->start_line.request_state=MRCP_REQUEST_STATE_COMPLETE;
    clear_audio(c); c->active=NULL;
    mrcp_engine_channel_message_send(c->base,event);
}
static int plain_content(mrcp_message_t *request)
{
    mrcp_generic_header_t *h=mrcp_generic_header_get(request);
#ifdef HP_SYNTH
    return !h || !mrcp_generic_header_property_check(request,GENERIC_HEADER_CONTENT_TYPE) ||
        !strcmp(h->content_type.buf,"text/plain") || !strcmp(h->content_type.buf,"text/plain; charset=utf-8");
#else
    static const char builtin[]="builtin:speech/transcribe";
    size_t n=request->body.length;
    if (!h || !mrcp_generic_header_property_check(request,GENERIC_HEADER_CONTENT_TYPE) || strcmp(h->content_type.buf,"text/uri-list")) return 0;
    while (n && (request->body.buf[n-1]=='\r' || request->body.buf[n-1]=='\n')) --n;
    return n==sizeof(builtin)-1 && !memcmp(request->body.buf,builtin,n);
#endif
}
static void start_request(http_channel *c, http_command *command)
{
    mrcp_message_t *r=command->request; const mpf_codec_descriptor_t *codec; apr_uuid_t uuid;
    unsigned char *ring=NULL;
#ifdef HP_SYNTH
    hp_buffer pcm={0};
    codec=mrcp_engine_source_stream_codec_get(c->base);
#else
    mrcp_recog_header_t *h; unsigned char *pcm;
    codec=mrcp_engine_sink_stream_codec_get(c->base);
#endif
    if (c->active) { reply(c,r,MRCP_STATUS_CODE_METHOD_NOT_VALID,MRCP_REQUEST_STATE_COMPLETE); return; }
    if (!codec || codec->sampling_rate!=HP_RATE || codec->channel_count!=1 || !plain_content(r)) {
        reply(c,r,MRCP_STATUS_CODE_UNSUPPORTED_PARAM_VALUE,MRCP_REQUEST_STATE_COMPLETE); return;
    }
#ifdef HP_SYNTH
    if (!r->body.length || r->body.length>HP_MAX_TEXT || memchr(r->body.buf,0,r->body.length)) {
        reply(c,r,MRCP_STATUS_CODE_ILLEGAL_PARAM_VALUE,MRCP_REQUEST_STATE_COMPLETE); return;
    }
#else
    c->noinput_ms=c->engine->noinput_ms; c->silence_ms=c->engine->silence_ms; c->max_ms=c->engine->max_ms; c->recognition_ms=0; c->input_timers=1;
    h=mrcp_resource_header_get(r);
    if (h) {
        if (mrcp_resource_header_property_check(r,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT)) c->noinput_ms=(long)h->no_input_timeout;
        if (mrcp_resource_header_property_check(r,RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT)) c->silence_ms=(long)h->speech_complete_timeout;
        if (mrcp_resource_header_property_check(r,RECOGNIZER_HEADER_RECOGNITION_TIMEOUT)) c->recognition_ms=(long)h->recognition_timeout;
        if (mrcp_resource_header_property_check(r,RECOGNIZER_HEADER_START_INPUT_TIMERS)) c->input_timers=h->start_input_timers;
        if (mrcp_resource_header_property_check(r,RECOGNIZER_HEADER_SAVE_WAVEFORM) && h->save_waveform) {
            reply(c,r,MRCP_STATUS_CODE_UNSUPPORTED_PARAM_VALUE,MRCP_REQUEST_STATE_COMPLETE); return;
        }
    }
    if (c->noinput_ms<100 || c->noinput_ms>120000 || c->silence_ms<100 || c->silence_ms>10000 || c->recognition_ms<0 || c->recognition_ms>120000) {
        reply(c,r,MRCP_STATUS_CODE_ILLEGAL_PARAM_VALUE,MRCP_REQUEST_STATE_COMPLETE); return;
    }
    pcm=c->engine->streaming ? NULL : malloc((size_t)c->max_ms*HP_RATE*2/1000);
    if (!pcm && !c->engine->streaming) { reply(c,r,MRCP_STATUS_CODE_METHOD_FAILED,MRCP_REQUEST_STATE_COMPLETE); return; }
#endif
    if (c->engine->streaming) {
        ring=malloc(HP_RING_CAP);
        if (!ring) { reply(c,r,MRCP_STATUS_CODE_METHOD_FAILED,MRCP_REQUEST_STATE_COMPLETE); return; }
    }
    clear_audio(c); apr_atomic_set32(&c->active_epoch,command->epoch); c->active=r;
    c->ring.data=ring; c->ring.cap=ring ? HP_RING_CAP : 0;
    c->started=c->timers_started=apr_time_now(); c->speech_reported=0;
    apr_atomic_set32(&c->speech,0); apr_atomic_set32(&c->media_fault,0);
    apr_uuid_get(&uuid); apr_uuid_format(c->request_id,&uuid);
    reply(c,r,MRCP_STATUS_CODE_SUCCESS,MRCP_REQUEST_STATE_INPROGRESS);
#ifdef HP_SYNTH
    if (cancelled(c)) return;
    if (c->engine->streaming) {
        apr_atomic_set32(&c->media_state,HP_PLAYING);
        c->transfer=hp_stream_tts(c->engine->url,r->body.buf,r->body.length,c->request_id,c->engine->timeout_ms,streaming_audio,c);
        if (!c->transfer) complete(c,SYNTHESIZER_COMPLETION_CAUSE_ERROR,NULL,0);
        return;
    }
    if (!hp_tts(c->engine->url,r->body.buf,r->body.length,c->request_id,c->engine->timeout_ms,cancelled,c,&pcm)) {
        if (!cancelled(c)) {
            apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"HTTP TTS failed request=%s",c->request_id);
            complete(c,SYNTHESIZER_COMPLETION_CAUSE_ERROR,NULL,0);
        }
        return;
    }
    apr_thread_mutex_lock(c->media_mutex);
    if (!cancelled(c)) { c->audio=pcm; memset(&pcm,0,sizeof(pcm)); apr_atomic_set32(&c->media_state,HP_PLAYING); }
    apr_thread_mutex_unlock(c->media_mutex); hp_buffer_free(&pcm);
#else
    apr_thread_mutex_lock(c->media_mutex);
    c->audio.data=pcm; c->audio.cap=c->audio.limit=(size_t)c->max_ms*HP_RATE*2/1000;
    c->run_samples=c->quiet_samples=c->total_samples=0;
    if (!cancelled(c)) apr_atomic_set32(&c->media_state,HP_CAPTURING);
    apr_thread_mutex_unlock(c->media_mutex);
    if (c->engine->streaming && !cancelled(c)) {
        c->transfer=hp_stream_asr(c->engine->url,c->request_id,c->engine->timeout_ms,c->silence_ms,streaming_audio,c);
        if (!c->transfer) complete(c,RECOGNIZER_COMPLETION_CAUSE_ERROR,NULL,0);
    }
#endif
}
static void stop_request(http_channel *c, mrcp_message_t *r)
{
    mrcp_message_t *response=mrcp_response_create(r,r->pool);
    if (c->active) {
        mrcp_generic_header_t *h=mrcp_generic_header_prepare(response);
        active_request_id_list_append(h,c->active->start_line.request_id);
        mrcp_generic_header_property_add(response,GENERIC_HEADER_ACTIVE_REQUEST_ID_LIST);
    }
    clear_audio(c); c->active=NULL;
    mrcp_engine_channel_message_send(c->base,response);
}
static void dispatch(http_channel *c, http_command *command)
{
    mrcp_message_t *r=command->request;
    if (r->start_line.method_id==HP_START) { start_request(c,command); return; }
    if (r->start_line.method_id==HP_STOP
#ifdef HP_SYNTH
        || r->start_line.method_id==SYNTHESIZER_BARGE_IN_OCCURRED
#endif
    ) { stop_request(c,r); return; }
#ifdef HP_SYNTH
    if (r->start_line.method_id==SYNTHESIZER_PAUSE || r->start_line.method_id==SYNTHESIZER_RESUME) {
        apr_thread_mutex_lock(c->media_mutex); c->paused=r->start_line.method_id==SYNTHESIZER_PAUSE; apr_thread_mutex_unlock(c->media_mutex);
        reply(c,r,MRCP_STATUS_CODE_SUCCESS,MRCP_REQUEST_STATE_COMPLETE); return;
    }
#else
    if (r->start_line.method_id==RECOGNIZER_START_INPUT_TIMERS) {
        c->input_timers=1; c->timers_started=apr_time_now();
        reply(c,r,MRCP_STATUS_CODE_SUCCESS,MRCP_REQUEST_STATE_COMPLETE); return;
    }
#endif
    reply(c,r,MRCP_STATUS_CODE_METHOD_NOT_ALLOWED,MRCP_REQUEST_STATE_COMPLETE);
}
static void poll_media(http_channel *c)
{
    apr_uint32_t state;
    if (!c->active || cancelled(c)) return;
    state=apr_atomic_read32(&c->media_state);
#ifdef HP_SYNTH
    if (c->transfer) {
        int status=hp_stream_poll(c->transfer);
        if (status<0) {
            apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Streaming TTS failed request=%s",c->request_id);
            complete(c,SYNTHESIZER_COMPLETION_CAUSE_ERROR,NULL,0); return;
        }
        if (status>0) {
            hp_stream_close(c->transfer); c->transfer=NULL;
            apr_thread_mutex_lock(c->media_mutex); c->stream_done=1; apr_thread_mutex_unlock(c->media_mutex);
        }
    }
    if (state==HP_PLAY_DONE) complete(c,SYNTHESIZER_COMPLETION_CAUSE_NORMAL,NULL,0);
#else
    if (apr_atomic_read32(&c->speech) && !c->speech_reported) {
        mrcp_message_t *e=mrcp_event_create(c->active,RECOGNIZER_START_OF_INPUT,c->active->pool);
        e->start_line.request_state=MRCP_REQUEST_STATE_INPROGRESS;
        c->speech_reported=1; c->speech_started=apr_time_now(); mrcp_engine_channel_message_send(c->base,e);
    }
    if (apr_atomic_read32(&c->media_fault)) { complete(c,RECOGNIZER_COMPLETION_CAUSE_ERROR,NULL,0); return; }
    if (state==HP_LIMIT) { complete(c,RECOGNIZER_COMPLETION_CAUSE_RECOGNITION_TIMEOUT,NULL,0); return; }
    if (state==HP_CAPTURING) {
        apr_time_t now=apr_time_now();
        int cause=-1;
        if (!apr_atomic_read32(&c->speech) && c->input_timers && now-c->timers_started>=apr_time_from_msec(c->noinput_ms)) cause=RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT;
        else if (c->speech_reported && c->recognition_ms && now-c->speech_started>=apr_time_from_msec(c->recognition_ms)) cause=RECOGNIZER_COMPLETION_CAUSE_RECOGNITION_TIMEOUT;
        else if (now-c->started>=apr_time_from_msec(c->max_ms)) cause=RECOGNIZER_COMPLETION_CAUSE_RECOGNITION_TIMEOUT;
        if (cause>=0) { complete(c,cause,NULL,0); return; }
    }
    if (c->transfer) {
        int status=hp_stream_poll(c->transfer);
        if (status) {
            size_t len=0,i; int nonspace=0; const char *text=hp_stream_text(c->transfer,&len);
            if (status<0) apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Streaming ASR failed request=%s",c->request_id);
            for (i=0;i<len;++i) if (!strchr(" \t\r\n",text[i])) { nonspace=1; break; }
            complete(c,status<0 ? RECOGNIZER_COMPLETION_CAUSE_ERROR : nonspace ? RECOGNIZER_COMPLETION_CAUSE_SUCCESS : RECOGNIZER_COMPLETION_CAUSE_NO_MATCH,
                     status>0 && nonspace ? text : NULL,status>0 && nonspace ? len : 0);
            return;
        }
    }
    if (state==HP_RECOG_READY && !c->engine->streaming) {
        hp_buffer pcm; char *text=NULL; size_t len=0; int ok;
        apr_thread_mutex_lock(c->media_mutex); pcm=c->audio; memset(&c->audio,0,sizeof(c->audio)); apr_atomic_set32(&c->media_state,HP_IDLE); apr_thread_mutex_unlock(c->media_mutex);
        ok=hp_asr(c->engine->url,pcm.data,pcm.len,c->request_id,c->engine->timeout_ms,cancelled,c,&text,&len);
        hp_buffer_free(&pcm);
        if (!cancelled(c)) {
            size_t i; int nonspace=0;
            for (i=0;i<len;++i) if (!strchr(" \t\r\n",text[i])) { nonspace=1; break; }
            if (!ok) apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"HTTP ASR failed request=%s",c->request_id);
            complete(c,!ok ? RECOGNIZER_COMPLETION_CAUSE_ERROR : nonspace ? RECOGNIZER_COMPLETION_CAUSE_SUCCESS : RECOGNIZER_COMPLETION_CAUSE_NO_MATCH,nonspace?text:NULL,nonspace?len:0);
        }
        free(text);
    }
#endif
}
static void *APR_THREAD_FUNC channel_worker(apr_thread_t *thread, void *context)
{
    http_channel *c=context; void *item; (void)thread;
    while (!apr_atomic_read32(&c->closing)) {
        apr_status_t status;
        apr_thread_mutex_lock(c->command_mutex);
        status=apr_queue_trypop(c->commands,&item);
        apr_thread_mutex_unlock(c->command_mutex);
        if (status==APR_SUCCESS) { dispatch(c,item); free(item); }
        poll_media(c);
        if (status!=APR_SUCCESS) apr_sleep(apr_time_from_msec(5));
    }
    clear_audio(c); c->active=NULL;
    while (apr_queue_trypop(c->commands,&item)==APR_SUCCESS) {
        http_command *command=item;
        reply(c,command->request,MRCP_STATUS_CODE_METHOD_FAILED,MRCP_REQUEST_STATE_COMPLETE); free(command);
    }
    /* destroy() joins this worker before the channel pool can be released. */
    mrcp_engine_channel_close_respond(c->base);
    return NULL;
}
static mrcp_engine_channel_t *channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
    http_channel *c=apr_pcalloc(pool,sizeof(*c)); mpf_stream_capabilities_t *caps; mpf_termination_t *termination;
    c->engine=engine->obj;
    if (apr_thread_mutex_create(&c->media_mutex,APR_THREAD_MUTEX_DEFAULT,pool)!=APR_SUCCESS || apr_thread_mutex_create(&c->command_mutex,APR_THREAD_MUTEX_DEFAULT,pool)!=APR_SUCCESS || apr_queue_create(&c->commands,32,pool)!=APR_SUCCESS) return NULL;
#ifdef HP_SYNTH
    caps=mpf_source_stream_capabilities_create(pool);
#else
    caps=mpf_sink_stream_capabilities_create(pool);
#endif
    mpf_codec_capabilities_add(&caps->codecs,MPF_SAMPLE_RATE_16000,"LPCM");
    termination=mrcp_engine_audio_termination_create(c,&stream_vtable,caps,pool);
    c->base=mrcp_engine_channel_create(engine,&channel_vtable,c,termination,pool);
    return c->base;
}
static apt_bool_t channel_open(mrcp_engine_channel_t *channel)
{
    http_channel *c=channel->method_obj;
    apr_status_t status=apr_thread_create(&c->worker,NULL,channel_worker,c,channel->pool);
    return mrcp_engine_channel_open_respond(channel,status==APR_SUCCESS);
}
static apt_bool_t channel_close(mrcp_engine_channel_t *channel)
{
    http_channel *c=channel->method_obj;
    apr_atomic_set32(&c->closing,1);
    return c->worker ? TRUE : mrcp_engine_channel_close_respond(channel);
}
static apt_bool_t channel_destroy(mrcp_engine_channel_t *channel)
{
    http_channel *c=channel->method_obj; apr_status_t status;
    apr_atomic_set32(&c->closing,1);
    if (c->worker) { apr_thread_join(&status,c->worker); c->worker=NULL; }
    return TRUE;
}
static apt_bool_t channel_request(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    http_channel *c=channel->method_obj; http_command *command;
    int stop=request->start_line.method_id==HP_STOP;
#ifdef HP_SYNTH
    stop=stop || request->start_line.method_id==SYNTHESIZER_BARGE_IN_OCCURRED;
#endif
    /* No queued synthesis or selective STOP in this first HTTP adapter. */
    if (stop && mrcp_generic_header_property_check(request,GENERIC_HEADER_ACTIVE_REQUEST_ID_LIST)) {
        reply(c,request,MRCP_STATUS_CODE_UNSUPPORTED_PARAM,MRCP_REQUEST_STATE_COMPLETE); return TRUE;
    }
    command=malloc(sizeof(*command));
    if (!command || apr_atomic_read32(&c->closing)) { free(command); reply(c,request,MRCP_STATUS_CODE_METHOD_FAILED,MRCP_REQUEST_STATE_COMPLETE); return TRUE; }
    if (apr_thread_mutex_trylock(c->command_mutex)!=APR_SUCCESS) {
        free(command); reply(c,request,MRCP_STATUS_CODE_METHOD_FAILED,MRCP_REQUEST_STATE_COMPLETE); return TRUE;
    }
    command->epoch=apr_atomic_read32(&c->epoch); command->request=request;
    if (apr_queue_trypush(c->commands,command)!=APR_SUCCESS) {
        apr_thread_mutex_unlock(c->command_mutex);
        free(command); reply(c,request,MRCP_STATUS_CODE_METHOD_FAILED,MRCP_REQUEST_STATE_COMPLETE); return TRUE;
    }
    if (stop) apr_atomic_inc32(&c->epoch);
    apr_thread_mutex_unlock(c->command_mutex);
    return TRUE;
}
#ifdef HP_SYNTH
static apt_bool_t stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
    http_channel *c=stream->obj; size_t n=frame->codec_frame.size;
    memset(frame->codec_frame.buffer,0,n); frame->type|=MEDIA_FRAME_TYPE_AUDIO;
    if (apr_atomic_read32(&c->media_state)!=HP_PLAYING || cancelled(c)) return TRUE;
    if (apr_thread_mutex_trylock(c->media_mutex)!=APR_SUCCESS) return TRUE;
    if (apr_atomic_read32(&c->media_state)==HP_PLAYING && !cancelled(c) && !c->paused) {
        if (c->engine->streaming) {
            if (!c->ring.len && c->stream_done) apr_atomic_set32(&c->media_state,HP_PLAY_DONE);
            else hp_ring_read(&c->ring,frame->codec_frame.buffer,n);
        }
        else if (c->position==c->audio.len) apr_atomic_set32(&c->media_state,HP_PLAY_DONE);
        else {
            if (n>c->audio.len-c->position) n=c->audio.len-c->position;
            memcpy(frame->codec_frame.buffer,c->audio.data+c->position,n); c->position+=n;
        }
    }
    apr_thread_mutex_unlock(c->media_mutex); return TRUE;
}
#else
static apt_bool_t stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
    http_channel *c=stream->obj; size_t i,n=frame->codec_frame.size,samples=n/2; uint64_t energy=0; const unsigned char *bytes=frame->codec_frame.buffer;
    if (!(frame->type&MEDIA_FRAME_TYPE_AUDIO) || !n || apr_atomic_read32(&c->media_state)!=HP_CAPTURING || cancelled(c)) return TRUE;
    if (apr_thread_mutex_trylock(c->media_mutex)!=APR_SUCCESS) { apr_atomic_set32(&c->media_fault,1); return TRUE; }
    if (apr_atomic_read32(&c->media_state)!=HP_CAPTURING || cancelled(c)) { apr_thread_mutex_unlock(c->media_mutex); return TRUE; }
    if (n%2) { apr_atomic_set32(&c->media_fault,1); apr_thread_mutex_unlock(c->media_mutex); return TRUE; }
    if (c->total_samples+samples>(size_t)c->max_ms*HP_RATE/1000) { apr_atomic_set32(&c->media_state,HP_LIMIT); apr_thread_mutex_unlock(c->media_mutex); return TRUE; }
    if (c->engine->streaming) {
        if (!hp_ring_write(&c->ring,bytes,n)) { apr_atomic_set32(&c->media_fault,1); apr_thread_mutex_unlock(c->media_mutex); return TRUE; }
    } else { memcpy(c->audio.data+c->audio.len,bytes,n); c->audio.len+=n; }
    c->total_samples+=samples;
    for (i=0;i<n;i+=2) { int16_t s; int32_t v; memcpy(&s,bytes+i,2); v=s; energy+=(uint64_t)((int64_t)v*v); }
    if (energy>=(uint64_t)c->engine->threshold*c->engine->threshold*samples) {
        c->run_samples+=samples;
        if (c->run_samples>=(size_t)c->engine->activity_ms*HP_RATE/1000) { apr_atomic_set32(&c->speech,1); c->quiet_samples=0; }
    } else { c->run_samples=0; if (apr_atomic_read32(&c->speech)) c->quiet_samples+=samples; }
    if (apr_atomic_read32(&c->speech) && c->quiet_samples>=(size_t)c->silence_ms*HP_RATE/1000) apr_atomic_set32(&c->media_state,HP_RECOG_READY);
    apr_thread_mutex_unlock(c->media_mutex); return TRUE;
}
#endif
