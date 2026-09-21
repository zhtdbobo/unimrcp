/* SPDX-License-Identifier: Apache-2.0 */
#include "http_common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    int16_t samples[]={-32768,-123,0,123,32767}; hp_buffer wav={0},pcm={0}; char *xml;
    assert(hp_wav_encode((unsigned char *)samples,sizeof(samples),&wav));
    assert(hp_wav_decode(wav.data,wav.len,&pcm));
    assert(pcm.len==sizeof(samples) && !memcmp(pcm.data,samples,sizeof(samples)));
    hp_buffer_free(&pcm);
    assert(!hp_wav_decode(wav.data,wav.len-1,&pcm));
    wav.data[24]=0x40; wav.data[25]=0x1f; /* 8000 Hz must not play at the wrong speed. */
    assert(!hp_wav_decode(wav.data,wav.len,&pcm));
    wav.data[24]=0x80; wav.data[25]=0x3e;
    wav.data[4]=0xbf; wav.data[5]=0xff; wav.data[6]=0xff; wav.data[7]=0x7f;
    wav.data[40]=0x9b; wav.data[41]=0xff; wav.data[42]=0xff; wav.data[43]=0x7f;
    assert(hp_wav_decode(wav.data,wav.len,&pcm)); hp_buffer_free(&pcm);
    wav.data[40]=0x9a;
    assert(!hp_wav_decode(wav.data,wav.len,&pcm)); hp_buffer_free(&wav);
    assert(!hp_wav_encode((unsigned char *)samples,3,&wav));
    xml=hp_nlsml("\xe4\xbd\xa0\xe5\xa5\xbd<&\"'",10);
    assert(xml && strstr(xml,"&lt;&amp;&quot;&apos;")); free(xml);
    assert(!hp_nlsml("bad\x01",4));
    wav.limit=4; assert(hp_buffer_append(&wav,"1234",4)); assert(!hp_buffer_append(&wav,"5",1)); hp_buffer_free(&wav);
    puts("PASS: PCM/WAV roundtrip, truncation, rate validation, exact placeholder, XML escaping, size limits");
    return 0;
}
