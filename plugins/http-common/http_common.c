/* SPDX-License-Identifier: Apache-2.0 */
#include "http_common.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(unsigned char *p, uint16_t n) { p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); }
static void put32(unsigned char *p, uint32_t n) { put16(p,(uint16_t)n); put16(p+2,(uint16_t)(n>>16)); }

void hp_buffer_free(hp_buffer *b) { free(b->data); memset(b,0,sizeof(*b)); }
int hp_buffer_append(hp_buffer *b, const void *data, size_t len)
{
    size_t cap; unsigned char *p;
    if (len > b->limit || b->len > b->limit-len) return 0;
    if (b->len+len > b->cap) {
        cap=b->cap ? b->cap : (b->limit<4096 ? b->limit : 4096);
        while (cap < b->len+len) { if (cap > b->limit/2) { cap=b->limit; break; } cap*=2; }
        p=(unsigned char *)realloc(b->data,cap);
        if (!p) return 0;
        b->data=p; b->cap=cap;
    }
    if (len) memcpy(b->data+b->len,data,len);
    b->len+=len; return 1;
}

int hp_wav_encode(const unsigned char *pcm, size_t len, hp_buffer *wav)
{
    unsigned char h[44]={0}; size_t i;
    if (!len || len%2 || len > HP_MAX_AUDIO-44) return 0;
    memcpy(h,"RIFF",4); put32(h+4,(uint32_t)len+36); memcpy(h+8,"WAVEfmt ",8);
    put32(h+16,16); put16(h+20,1); put16(h+22,1); put32(h+24,HP_RATE);
    put32(h+28,HP_RATE*2); put16(h+32,2); put16(h+34,16); memcpy(h+36,"data",4); put32(h+40,(uint32_t)len);
    wav->limit=HP_MAX_AUDIO;
    if (!hp_buffer_append(wav,h,44) || !hp_buffer_append(wav,pcm,len)) return 0;
    /* MPF LPCM is host endian; RIFF PCM is little endian. */
    { const uint16_t one=1; if (!*(const unsigned char *)&one) {
        for (i=44;i<wav->len;i+=2) { unsigned char x=wav->data[i]; wav->data[i]=wav->data[i+1]; wav->data[i+1]=x; }
    } }
    return 1;
}

int hp_wav_decode(const unsigned char *wav, size_t len, hp_buffer *pcm)
{
    size_t pos=12, n, data_pos=0, data_len=0, i; unsigned channels=0; int fmt=0, placeholder=0;
    if (len<44 || len>HP_MAX_AUDIO || memcmp(wav,"RIFF",4) || memcmp(wav+8,"WAVE",4)) return 0;
    /* Only the exact streaming placeholder pair observed in the existing TTS gateway. */
    placeholder=u32(wav+4)==0x7fffffbf && !memcmp(wav+12,"fmt ",4) && u32(wav+16)==16 &&
                !memcmp(wav+36,"data",4) && u32(wav+40)==0x7fffff9b;
    if (!placeholder && u32(wav+4)!=len-8) return 0;
    while (pos+8<=len) {
        n=u32(wav+pos+4);
        if (placeholder && pos==36) n=len-44;
        if (n>len-pos-8) return 0;
        if (!memcmp(wav+pos,"fmt ",4)) {
            if (fmt || n<16 || u16(wav+pos+8)!=1 || u16(wav+pos+22)!=16 || u32(wav+pos+12)!=HP_RATE) return 0;
            channels=u16(wav+pos+10);
            if ((channels!=1 && channels!=2) || u16(wav+pos+20)!=channels*2 || u32(wav+pos+16)!=HP_RATE*channels*2) return 0;
            fmt=1;
        } else if (!memcmp(wav+pos,"data",4)) {
            if (data_pos || !n) return 0;
            data_pos=pos+8; data_len=n;
        }
        pos+=8+n+(n&1);
    }
    if (!fmt || !data_pos || pos!=len || data_len%(channels*2)) return 0;
    pcm->limit=HP_MAX_AUDIO;
    /* Downmix 16-kHz stereo using a 32-bit sum. Do not silently resample other rates. */
    for (i=0;i<data_len;i+=channels*2) {
        int32_t s=(int16_t)u16(wav+data_pos+i); int16_t sample;
        if (channels==2) s=(s+(int16_t)u16(wav+data_pos+i+2))/2;
        sample=(int16_t)s;
        if (!hp_buffer_append(pcm,&sample,2)) return 0;
    }
    return 1;
}

char *hp_nlsml(const char *text, size_t len)
{
    hp_buffer b={0}; size_t i; const char *escape;
    const char *prefix="<?xml version=\"1.0\" encoding=\"UTF-8\"?><result><interpretation><input mode=\"speech\">";
    const char *suffix="</input></interpretation></result>";
    b.limit=HP_MAX_TEXT*6+512;
    if (len>HP_MAX_TEXT || !hp_buffer_append(&b,prefix,strlen(prefix))) goto fail;
    for (i=0;i<len;++i) {
        unsigned char c=(unsigned char)text[i];
        if (c<32 && c!=9 && c!=10 && c!=13) goto fail;
        escape=NULL;
        switch(c) { case '&':escape="&amp;";break; case '<':escape="&lt;";break; case '>':escape="&gt;";break; case '"':escape="&quot;";break; case '\'':escape="&apos;";break; }
        if (!hp_buffer_append(&b,escape?escape:text+i,escape?strlen(escape):1)) goto fail;
    }
    if (!hp_buffer_append(&b,suffix,strlen(suffix)+1)) goto fail;
    return (char *)b.data;
fail: hp_buffer_free(&b); return NULL;
}

typedef struct { hp_buffer body; hp_cancel_fn cancel; void *context; } hp_transfer;
static size_t receive(void *data, size_t size, size_t count, void *ctx)
{
    hp_transfer *t=(hp_transfer *)ctx;
    if ((size && count>SIZE_MAX/size) || (t->cancel && t->cancel(t->context))) return 0;
    return hp_buffer_append(&t->body,data,size*count) ? size*count : 0;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    hp_transfer *t=(hp_transfer *)ctx; (void)a; (void)b; (void)c; (void)d;
    return t->cancel ? t->cancel(t->context) : 0;
}
static CURL *prepare(const char *url, const char *id, long timeout_ms, hp_transfer *t, struct curl_slist **headers)
{
    CURL *curl; char header[160]; size_t i;
    if (!url || (strncmp(url,"http://",7) && strncmp(url,"https://",8)) || !id || strlen(id)>100) return NULL;
    for (i=0;id[i];++i) if ((unsigned char)id[i]<32 || (unsigned char)id[i]>126) return NULL;
    snprintf(header,sizeof(header),"X-Request-ID: %s",id);
    *headers=curl_slist_append(NULL,header);
    if (!*headers) return NULL;
    curl=curl_easy_init(); if (!curl) return NULL;
    curl_easy_setopt(curl,CURLOPT_URL,url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
#else
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_PROXY,"");
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,5000L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,timeout_ms);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,t);
    curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(curl,CURLOPT_XFERINFODATA,t);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    return curl;
}
static int perform(CURL *curl, struct curl_slist *headers)
{
    long status=0; CURLcode rc;
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    rc=curl_easy_perform(curl);
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    return rc==CURLE_OK && status>=200 && status<300;
}

int hp_tts(const char *url, const char *text, size_t len, const char *id, long timeout,
           hp_cancel_fn cancel, void *context, hp_buffer *pcm)
{
    hp_transfer t={{0},cancel,context}; struct curl_slist *headers=NULL,*next; CURL *curl=NULL;
    struct json_object *json=NULL; int ok=0;
    if (!len || len>HP_MAX_TEXT || memchr(text,0,len)) return 0;
    t.body.limit=HP_MAX_AUDIO;
    curl=prepare(url,id,timeout,&t,&headers); if (!curl) goto done;
    next=curl_slist_append(headers,"Content-Type: application/json"); if (!next) goto done; headers=next;
    json=json_object_new_object(); if (!json) goto done;
    json_object_object_add(json,"text",json_object_new_string_len(text,(int)len));
    json_object_object_add(json,"format",json_object_new_string("wav"));
    json_object_object_add(json,"sample_rate",json_object_new_int(HP_RATE));
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json_object_to_json_string_ext(json,JSON_C_TO_STRING_PLAIN));
    if (perform(curl,headers) && !(cancel && cancel(context))) ok=hp_wav_decode(t.body.data,t.body.len,pcm);
done:
    if (curl) curl_easy_cleanup(curl);
    curl_slist_free_all(headers); if (json) json_object_put(json); hp_buffer_free(&t.body);
    if (!ok) hp_buffer_free(pcm);
    return ok;
}

int hp_asr(const char *url, const unsigned char *pcm, size_t len, const char *id, long timeout,
           hp_cancel_fn cancel, void *context, char **text, size_t *text_len)
{
    hp_transfer t={{0},cancel,context}; hp_buffer wav={0}; struct curl_slist *headers=NULL; CURL *curl=NULL;
    curl_mime *mime=NULL; curl_mimepart *part; struct json_tokener *tok=NULL;
    struct json_object *root=NULL,*value=NULL; int ok=0; size_t used;
    *text=NULL; *text_len=0; t.body.limit=1024*1024;
    if (!hp_wav_encode(pcm,len,&wav)) goto done;
    curl=prepare(url,id,timeout,&t,&headers); if (!curl) goto done;
    mime=curl_mime_init(curl); if (!mime) goto done;
    part=curl_mime_addpart(mime); if (!part) goto done;
    curl_mime_name(part,"file"); curl_mime_filename(part,"utterance.wav"); curl_mime_type(part,"audio/wav");
    if (curl_mime_data(part,(const char *)wav.data,wav.len)!=CURLE_OK) goto done;
    part=curl_mime_addpart(mime); if (!part) goto done; curl_mime_name(part,"format"); curl_mime_data(part,"wav",CURL_ZERO_TERMINATED);
    part=curl_mime_addpart(mime); if (!part) goto done; curl_mime_name(part,"sample_rate"); curl_mime_data(part,"16000",CURL_ZERO_TERMINATED);
    part=curl_mime_addpart(mime); if (!part) goto done; curl_mime_name(part,"language_hints"); curl_mime_data(part,"zh",CURL_ZERO_TERMINATED);
    curl_easy_setopt(curl,CURLOPT_MIMEPOST,mime);
    if (!perform(curl,headers) || (cancel && cancel(context)) || !t.body.len) goto done;
    tok=json_tokener_new(); if (!tok) goto done;
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    root=json_tokener_parse_ex(tok,(const char *)t.body.data,(int)t.body.len);
    if (!root || json_tokener_get_error(tok)!=json_tokener_success) goto done;
    used=json_tokener_get_parse_end(tok);
    for (;used<t.body.len;++used) if (!strchr(" \t\r\n",t.body.data[used]) || !t.body.data[used]) goto done;
    if (!json_object_is_type(root,json_type_object) || !json_object_object_get_ex(root,"text",&value) || !json_object_is_type(value,json_type_string)) goto done;
    *text_len=(size_t)json_object_get_string_len(value);
    if (*text_len>HP_MAX_TEXT || memchr(json_object_get_string(value),0,*text_len)) goto done;
    *text=(char *)malloc(*text_len+1); if (!*text) goto done;
    memcpy(*text,json_object_get_string(value),*text_len+1); ok=1;
done:
    if (root) json_object_put(root);
    if (tok) json_tokener_free(tok);
    if (mime) curl_mime_free(mime);
    if (curl) curl_easy_cleanup(curl);
    curl_slist_free_all(headers); hp_buffer_free(&wav); hp_buffer_free(&t.body);
    if (!ok) { free(*text); *text=NULL; *text_len=0; }
    return ok;
}
