"""Exercise fixed-rate configuration and optional recording with real MRCP."""
import http.server
import threading

import test_integration as base


def main():
    upstream = http.server.ThreadingHTTPServer(('127.0.0.1',0),base.Upstream)
    threading.Thread(target=upstream.serve_forever,daemon=True).start()
    tts = f'http://127.0.0.1:{upstream.server_port}/tts'
    asr = f'http://127.0.0.1:{upstream.server_port}/asr'
    try:
        for rate in (8000,16000):
            def exercise(sip):
                for resource in ('speechsynth','speechrecog'):
                    with base.Session(sip,resource,offered_rates=(16000,8000)) as s:
                        assert s.rate==rate, 'Fixed engine rate was not negotiated'
                        if resource=='speechsynth':
                            before=base.record_files(); s.speak('fixed-rate'); s.event('SPEAK-COMPLETE',0)
                            base.assert_new_recording(before,base.pcm_bytes(rate),rate)
                        else:
                            s.recognize(); s.audio(); s.event('RECOGNITION-COMPLETE',0)
            base.run_server(tts,asr,exercise,sample_rate=rate)
        def disabled(sip):
            with base.Session(sip,'speechsynth') as s:
                s.speak('recording-disabled'); s.event('SPEAK-COMPLETE',0)
                assert not base.AUDIO_DIR.exists()
        base.run_server(tts,asr,disabled,save_audio=False)
        assert not base.HTTP_ERRORS, base.HTTP_ERRORS
        print('PASS: fixed 8000/16000 configuration and recording disabled',flush=True)
    finally:
        upstream.shutdown(); upstream.server_close()


if __name__=='__main__':
    main()
