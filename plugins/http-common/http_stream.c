/* SPDX-License-Identifier: Apache-2.0
 * HTTP PCM synthesis and the Asr/realtime.py WebSocket protocol.
 * Each instance belongs to one channel worker, including library callbacks.
 */
#include "http_stream.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct hp_stream {
    CURL *easy;
    CURLM *multi;
    struct curl_slist *headers;
    struct json_object *request;
    hp_pcm_sink sink;
    hp_pcm_source source;
    void *context;
    int status, paused, tail_valid;
    unsigned char tail;
    size_t audio_bytes;
    struct lws_context *ws_context;
    struct lws *ws;
    lws_sorted_usec_list_t timer;
    lws_usec_t deadline;
    int connected, start_sent, ready, finish_sent;
    hp_buffer message;
    char *url, *path, *host, *text;
    size_t text_len;
    char id[101], start[256];
};

int hp_ring_write(hp_ring *r, const void *data, size_t len)
{
    size_t pos, n;
    if (len>r->cap-r->len) return 0;
    if (!len) return 1;
    pos=(r->head+r->len)%r->cap; n=r->cap-pos;
    if (n>len) n=len;
    memcpy(r->data+pos,data,n);
    memcpy(r->data,(const unsigned char *)data+n,len-n);
    r->len+=len;
    return 1;
}

size_t hp_ring_read(hp_ring *r, void *data, size_t len)
{
    size_t n;
    if (len>r->len) len=r->len;
    if (!len) return 0;
    n=r->cap-r->head; if (n>len) n=len;
    memcpy(data,r->data+r->head,n);
    memcpy((unsigned char *)data+n,r->data,len-n);
    r->head=(r->head+len)%r->cap; r->len-=len;
    return len;
}

static void swap_pcm(unsigned char *data, size_t len)
{
    const uint16_t one=1; size_t i;
    if (*(const unsigned char *)&one) return;
    for (i=0;i<len;i+=2) { unsigned char tmp=data[i]; data[i]=data[i+1]; data[i+1]=tmp; }
}

static int valid_id(const char *id)
{
    size_t i;
    if (!id || strlen(id)>100) return 0;
    for (i=0;id[i];++i) if ((unsigned char)id[i]<32 || (unsigned char)id[i]>126) return 0;
    return 1;
}

static size_t tts_receive(char *data, size_t size, size_t count, void *context)
{
    hp_stream *s=context;
    unsigned char pcm[CURL_MAX_WRITE_SIZE+2]; size_t len, total, aligned;
    long code=0; char *type=NULL; int accepted;
    if (size && count>SIZE_MAX/size) return 0;
    len=size*count; total=len+(size_t)s->tail_valid; aligned=total&~(size_t)1;
    curl_easy_getinfo(s->easy,CURLINFO_RESPONSE_CODE,&code);
    curl_easy_getinfo(s->easy,CURLINFO_CONTENT_TYPE,&type);
    if (code!=200 || !type || (strcmp(type,"audio/pcm") && strncmp(type,"audio/pcm;",10)) ||
        len>CURL_MAX_WRITE_SIZE || len>HP_MAX_AUDIO-s->audio_bytes) return 0;
    if (s->tail_valid) pcm[0]=s->tail;
    memcpy(pcm+s->tail_valid,data,len); swap_pcm(pcm,aligned);
    accepted=aligned ? s->sink(s->context,pcm,aligned) : 1;
    if (accepted<0) return 0;
    if (!accepted) { s->paused=1; return CURL_WRITEFUNC_PAUSE; }
    s->audio_bytes+=len; s->tail_valid=(int)(total&1);
    if (s->tail_valid) s->tail=pcm[aligned];
    return len;
}

hp_stream *hp_stream_tts(const char *url, const char *text, size_t len,
                         const char *id, long timeout, hp_pcm_sink sink, void *context)
{
    hp_stream *s; char header[128]; struct curl_slist *next;
    if (!url || (strncmp(url,"http://",7) && strncmp(url,"https://",8)) ||
        !valid_id(id) || !sink || !len || len>HP_MAX_TEXT || memchr(text,0,len)) return NULL;
    s=calloc(1,sizeof(*s)); if (!s) return NULL;
    s->sink=sink; s->context=context;
    s->easy=curl_easy_init(); s->multi=curl_multi_init(); s->request=json_object_new_object();
    if (!s->easy || !s->multi || !s->request) goto fail;
    json_object_object_add(s->request,"text",json_object_new_string_len(text,(int)len));
    json_object_object_add(s->request,"format",json_object_new_string("pcm"));
    json_object_object_add(s->request,"sample_rate",json_object_new_int(HP_RATE));
    snprintf(header,sizeof(header),"X-Request-ID: %s",id);
    s->headers=curl_slist_append(NULL,header); if (!s->headers) goto fail;
    next=curl_slist_append(s->headers,"Content-Type: application/json"); if (!next) goto fail;
    s->headers=next;
    curl_easy_setopt(s->easy,CURLOPT_URL,url);
    curl_easy_setopt(s->easy,CURLOPT_PROXY,"");
    curl_easy_setopt(s->easy,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(s->easy,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(s->easy,CURLOPT_CONNECTTIMEOUT_MS,5000L);
    curl_easy_setopt(s->easy,CURLOPT_TIMEOUT_MS,timeout);
    curl_easy_setopt(s->easy,CURLOPT_HTTPHEADER,s->headers);
    curl_easy_setopt(s->easy,CURLOPT_POSTFIELDS,json_object_to_json_string_ext(s->request,JSON_C_TO_STRING_PLAIN));
    curl_easy_setopt(s->easy,CURLOPT_WRITEFUNCTION,tts_receive);
    curl_easy_setopt(s->easy,CURLOPT_WRITEDATA,s);
    if (curl_multi_add_handle(s->multi,s->easy)!=CURLM_OK) goto fail;
    return s;
fail:
    hp_stream_close(s); return NULL;
}

static int asr_message(hp_stream *s)
{
    struct json_tokener *tok=json_tokener_new();
    struct json_object *root=NULL,*event=NULL,*text=NULL,*final=NULL;
    const char *name; size_t used; int ok=0;
    if (!tok) return 0;
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT|JSON_TOKENER_VALIDATE_UTF8);
    root=json_tokener_parse_ex(tok,(const char *)s->message.data,(int)s->message.len);
    if (!root || json_tokener_get_error(tok)!=json_tokener_success) goto done;
    used=json_tokener_get_parse_end(tok);
    for (;used<s->message.len;++used) if (!s->message.data[used] || !strchr(" \t\r\n",s->message.data[used])) goto done;
    if (!json_object_is_type(root,json_type_object) || !json_object_object_get_ex(root,"event",&event) ||
        !json_object_is_type(event,json_type_string)) goto done;
    name=json_object_get_string(event);
    if (strlen(name)!=(size_t)json_object_get_string_len(event)) goto done;
    if (!strcmp(name,"started") && s->start_sent && !s->ready) { s->ready=1; ok=1; goto done; }
    if (!s->ready || (strcmp(name,"result") && strcmp(name,"completed"))) goto done;
    if (!json_object_object_get_ex(root,"text",&text) || !json_object_is_type(text,json_type_string) ||
        (size_t)json_object_get_string_len(text)>HP_MAX_TEXT ||
        memchr(json_object_get_string(text),0,(size_t)json_object_get_string_len(text))) goto done;
    if (!strcmp(name,"result")) {
        /* Consume partial revisions, but only completed.text is authoritative. */
        ok=json_object_object_get_ex(root,"is_final",&final) && json_object_is_type(final,json_type_boolean);
    } else if (s->finish_sent) {
        s->text_len=(size_t)json_object_get_string_len(text);
        s->text=malloc(s->text_len+1);
        if (s->text) { memcpy(s->text,json_object_get_string(text),s->text_len+1); s->status=1; ok=1; }
    }
done:
    if (root) json_object_put(root);
    json_tokener_free(tok); s->message.len=0;
    return ok;
}

static int asr_callback(struct lws *ws, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len)
{
    hp_stream *s=lws_context_user(lws_get_context(ws));
    unsigned char buffer[LWS_PRE+3200]; size_t n; int eof=0, written;
    enum lws_write_protocol mode=LWS_WRITE_TEXT;
    (void)user;
    if (!s) return 0;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        unsigned char **p=in;
        return lws_add_http_header_by_name(ws,(const unsigned char *)"x-request-id:",
                   (const unsigned char *)s->id,(int)strlen(s->id),p,*p+len) ? -1 : 0;
    }
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        s->connected=1; lws_callback_on_writable(ws); break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (s->status || s->finish_sent) break;
        if (!s->start_sent) { n=strlen(s->start); memcpy(buffer+LWS_PRE,s->start,n); s->start_sent=1; }
        else {
            if (!s->ready) break;
            n=s->source(s->context,buffer+LWS_PRE,3200,&eof);
            if (n==SIZE_MAX || n>3200 || n%2) goto fail;
            if (n) { swap_pcm(buffer+LWS_PRE,n); mode=LWS_WRITE_BINARY; }
            else if (eof) {
                static const char finish[]="{\"type\":\"finish\"}";
                n=sizeof(finish)-1; memcpy(buffer+LWS_PRE,finish,n); s->finish_sent=1;
            } else break;
        }
        /* lws buffers a partial socket write internally; never replay it. */
        written=lws_write(ws,buffer+LWS_PRE,n,mode);
        if (written<(int)n) goto fail;
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (s->status) break;
        if (lws_frame_is_binary(ws) || !hp_buffer_append(&s->message,in,len)) goto fail;
        if (lws_is_final_fragment(ws) && !lws_remaining_packet_payload(ws) && !asr_message(s)) goto fail;
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    case LWS_CALLBACK_CLIENT_CLOSED:
        s->ws=NULL; s->connected=0; if (!s->status) s->status=-1; break;
    default: break;
    }
    return 0;
fail:
    s->status=-1; return -1;
}

static const struct lws_protocols asr_protocols[]={
    {"asr-stream",asr_callback,0,16384,0,NULL,0},
    {NULL,NULL,0,0,0,NULL,0}
};

static void service_tick(lws_sorted_usec_list_t *timer) { (void)timer; }

hp_stream *hp_stream_asr(const char *url, const char *id, long timeout,
                         long silence_ms, hp_pcm_source source, void *context)
{
    hp_stream *s; struct lws_context_creation_info info;
    struct lws_client_connect_info conn;
    const char *protocol,*address,*path; int port; size_t n;
    if (!url || (strncmp(url,"ws://",5) && strncmp(url,"wss://",6)) || !valid_id(id) || !source) return NULL;
    s=calloc(1,sizeof(*s)); if (!s) return NULL;
    n=strlen(url); s->url=malloc(n+1); s->path=malloc(n+2); s->host=malloc(n+8);
    if (!s->url || !s->path || !s->host) goto fail;
    memcpy(s->url,url,n+1); memcpy(s->id,id,strlen(id)+1);
    if (lws_parse_uri(s->url,&protocol,&address,&port,&path) || port<1 || port>65535) goto fail;
    snprintf(s->path,n+2,"/%s",path);
    snprintf(s->host,n+8,strchr(address,':') ? "[%s]:%d" : "%s:%d",address,port);
    s->source=source; s->context=context; s->message.limit=1024*1024;
    if (silence_ms<200) silence_ms=200;
    if (silence_ms>6000) silence_ms=6000;
    snprintf(s->start,sizeof(s->start),"{\"type\":\"start\",\"format\":\"pcm\",\"sample_rate\":16000,\"language_hints\":[\"zh\"],\"max_sentence_silence\":%ld}",silence_ms);
    memset(&info,0,sizeof(info));
    info.port=CONTEXT_PORT_NO_LISTEN; info.protocols=asr_protocols; info.user=s;
    info.uid=-1; info.gid=-1;
    info.options=LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.http_proxy_address="";
    info.fd_limit_per_thread=16; info.timeout_secs=5;
    s->ws_context=lws_create_context(&info); if (!s->ws_context) goto fail;
    memset(&conn,0,sizeof(conn)); conn.context=s->ws_context;
    conn.address=address; conn.port=port; conn.path=s->path; conn.host=s->host;
    conn.local_protocol_name="asr-stream"; conn.pwsi=&s->ws;
    conn.ssl_connection=LCCSCF_HTTP_NO_FOLLOW_REDIRECT | (!strcmp(protocol,"wss") ? LCCSCF_USE_SSL : 0);
    s->deadline=lws_now_usecs()+(lws_usec_t)timeout*1000;
    if (!lws_client_connect_via_info(&conn)) goto fail;
    return s;
fail:
    hp_stream_close(s); return NULL;
}

int hp_stream_poll(hp_stream *s)
{
    if (s->status) return s->status;
    if (s->multi) {
        CURLMsg *message; int running, pending;
        if (s->paused) {
            s->paused=0;
            if (curl_easy_pause(s->easy,CURLPAUSE_CONT)!=CURLE_OK) return s->status=-1;
        }
        if (curl_multi_perform(s->multi,&running)!=CURLM_OK) return s->status=-1;
        while ((message=curl_multi_info_read(s->multi,&pending))) {
            if (message->msg==CURLMSG_DONE) {
                long code=0; curl_easy_getinfo(s->easy,CURLINFO_RESPONSE_CODE,&code);
                s->status=message->data.result==CURLE_OK && code==200 && s->audio_bytes && !s->tail_valid ? 1 : -1;
            }
        }
    } else {
        if (lws_now_usecs()>=s->deadline) return s->status=-1;
        if (s->ws && s->connected && !s->finish_sent) lws_callback_on_writable(s->ws);
        /* lws 3.2+ ignores the service timeout argument; bound its wait via a timer. */
        lws_sul_schedule(s->ws_context,0,&s->timer,service_tick,1000);
        if (lws_service(s->ws_context,0)<0) s->status=-1;
    }
    return s->status;
}

const char *hp_stream_text(hp_stream *s, size_t *len) { *len=s->text_len; return s->text; }

void hp_stream_close(hp_stream *s)
{
    if (!s) return;
    if (s->multi && s->easy) curl_multi_remove_handle(s->multi,s->easy);
    if (s->easy) curl_easy_cleanup(s->easy);
    if (s->multi) curl_multi_cleanup(s->multi);
    curl_slist_free_all(s->headers);
    if (s->request) json_object_put(s->request);
    if (s->ws_context) { lws_sul_cancel(&s->timer); lws_context_destroy(s->ws_context); }
    hp_buffer_free(&s->message);
    free(s->url); free(s->path); free(s->host); free(s->text); free(s);
}
