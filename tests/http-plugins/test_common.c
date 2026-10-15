/* SPDX-License-Identifier: Apache-2.0 */
#include "http_common.h"
#include "http_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    int16_t samples[]={-32768,-123,0,123,32767}; hp_buffer wav={0},pcm={0}; char *xml;
    int rates[]={8000,16000}; size_t rate_index;
    for (rate_index=0;rate_index<2;++rate_index) {
    int rate=rates[rate_index];
    assert(hp_wav_encode((unsigned char *)samples,sizeof(samples),rate,&wav));
    assert(hp_wav_decode(wav.data,wav.len,rate,&pcm));
    assert(pcm.len==sizeof(samples) && !memcmp(pcm.data,samples,sizeof(samples)));
    hp_buffer_free(&pcm);
    assert(!hp_wav_decode(wav.data,wav.len-1,rate,&pcm));
    assert(!hp_wav_decode(wav.data,wav.len,rate==8000 ? 16000 : 8000,&pcm));
    assert(!hp_wav_decode(wav.data,wav.len,24000,&pcm));
    wav.data[4]=0xbf; wav.data[5]=0xff; wav.data[6]=0xff; wav.data[7]=0x7f;
    wav.data[40]=0x9b; wav.data[41]=0xff; wav.data[42]=0xff; wav.data[43]=0x7f;
    assert(hp_wav_decode(wav.data,wav.len,rate,&pcm)); hp_buffer_free(&pcm);
    wav.data[40]=0x9a;
    assert(!hp_wav_decode(wav.data,wav.len,rate,&pcm)); hp_buffer_free(&wav);
    assert(!hp_wav_encode((unsigned char *)samples,3,rate,&wav));
    }
    assert(!hp_wav_encode((unsigned char *)samples,sizeof(samples),24000,&wav));
    { unsigned char header[44]; assert(hp_wav_header(header,0,8000)); assert(!hp_wav_header(header,1,8000)); }
    xml=hp_nlsml("\xe4\xbd\xa0\xe5\xa5\xbd<&\"'",10);
    assert(xml && strstr(xml,"&lt;&amp;&quot;&apos;")); free(xml);
    assert(!hp_nlsml("bad\x01",4));
    wav.limit=4; assert(hp_buffer_append(&wav,"1234",4)); assert(!hp_buffer_append(&wav,"5",1)); hp_buffer_free(&wav);
    {
        unsigned char data[8],out[8]; hp_ring ring={data,0,0,sizeof(data)};
        assert(hp_ring_write(&ring,"123456",6));
        assert(!hp_ring_write(&ring,"789",3));
        assert(hp_ring_read(&ring,out,4)==4 && !memcmp(out,"1234",4));
        assert(hp_ring_write(&ring,"789012",6));
        assert(hp_ring_read(&ring,out,8)==8 && !memcmp(out,"56789012",8));
        assert(hp_ring_read(&ring,out,8)==0);
    }
    puts("PASS: PCM/WAV roundtrip, truncation, rate validation, exact placeholder, XML escaping, size limits");
    return 0;
}
