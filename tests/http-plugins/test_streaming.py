"""Real SIP/MRCP/RTP with the streaming contracts in sibling Asr and Tts projects."""
import asyncio
import http.server
from http import HTTPStatus
import json
import socket
import threading
import time

import websockets

from test_integration import PCM, RATE, Session, run_server

ERRORS = []
RELEASE = threading.Event()
FIRST_AUDIO = threading.Event()
ASR_CONNECTED = threading.Event()
ASR_MODE = 'ok'
ASR_FINISHED = []


class StreamingTTS(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass  # STOP may reset an HTTP keep-alive connection between requests.

    def chunk(self, data):
        self.wfile.write(f'{len(data):x}\r\n'.encode()+data+b'\r\n')
        self.wfile.flush()

    def do_POST(self):
        try:
            assert self.path == '/api/v1/tts/stream'
            assert self.headers.get('X-Request-ID')
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert request['format'] == 'pcm' and request['sample_rate'] == RATE
            phrase = request['text']
            if phrase == 'slow':
                time.sleep(3)
            self.send_response(503 if phrase == 'http-error' else 302 if phrase == 'redirect' else 200)
            if phrase == 'redirect':
                self.send_header('Location', '/unexpected-redirect')
            self.send_header('Content-Type', 'application/json' if phrase == 'wrong-type' else 'audio/pcm')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            if phrase == 'gated':
                # Split a sample across HTTP chunks. The client must play before
                # releasing this gate, and must survive the resulting underrun.
                self.chunk(PCM[:1]); self.chunk(PCM[1:2001]); self.chunk(PCM[2001:])
                assert RELEASE.wait(3), 'TTS buffered the whole response'
                self.chunk(PCM)
            elif phrase == 'burst':
                for _ in range(400):
                    self.chunk(PCM)
            elif phrase == 'odd':
                self.chunk(PCM+b'\x01')
            elif phrase == 'empty':
                pass
            elif phrase == 'truncated':
                self.chunk(PCM)
                self.close_connection = True  # Missing terminal HTTP chunk is an error.
                return
            else:
                self.chunk(PCM)
            self.wfile.write(b'0\r\n\r\n'); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:
            ERRORS.append(repr(exc))
            self.close_connection = True


async def asr_handler(ws, path):
    mode = ASR_MODE
    ASR_CONNECTED.set()
    try:
        assert path == '/v1/asr/stream'
        assert ws.request_headers.get('X-Request-ID')
        start = json.loads(await ws.recv())
        assert start == {'type': 'start', 'format': 'pcm', 'sample_rate': RATE,
                         'language_hints': ['zh'], 'max_sentence_silence': 200}
        if mode == 'no-start':
            await ws.wait_closed(); return
        await ws.send(json.dumps({'event': 'started'}))
        audio = bytearray()
        while True:
            packet = await ws.recv()
            if isinstance(packet, str):
                assert json.loads(packet) == {'type': 'finish'}
                assert len(audio) > 2000
                ASR_FINISHED.append(mode)
                if mode == 'no-completion':
                    await ws.wait_closed(); return
                text = '' if mode == 'empty' else '你好<&"，这是修正后的完整结果。'
                await ws.send(json.dumps({'event': 'completed', 'text': text}))
                return
            assert 0 < len(packet) <= 3200 and len(packet) % 2 == 0
            audio.extend(packet)
            FIRST_AUDIO.set()
            if len(audio) == len(packet):
                if mode == 'close':
                    await ws.close(); return
                if mode == 'error':
                    await ws.send('{"event":"error","code":"upstream_error"}'); return
                if mode == 'bad-json':
                    await ws.send('{"event":'); return
                if mode == 'oversize':
                    await ws.send(json.dumps({'event': 'result', 'text': 'x'*90000, 'is_final': False})); return
                if mode == 'big-message':
                    await ws.send('x'*(1024*1024+1)); return
                # Fragmented text + ping, then revisions and duplicate finals.
                partial = json.dumps({'event': 'result', 'text': '临时', 'is_final': False})
                await ws.send([partial[:13], partial[13:]])
                await ws.ping(b'alive')
                for text in ('修正', '修正'):
                    await ws.send(json.dumps({'event': 'result', 'text': text, 'is_final': True}))
    except websockets.ConnectionClosed:
        pass
    except Exception as exc:
        ERRORS.append(repr(exc))


def no_event(session, duration=.2):
    try:
        event = session.receive(timeout=duration)
    except socket.timeout:
        return
    raise AssertionError(f'Unexpected event: {event}')


def wait_audio(session):
    deadline = time.monotonic()+2
    while time.monotonic() < deadline:
        if any(session.rtp.recv(4096)[12:]):
            return
    raise AssertionError('No non-silent RTP')


def exercise(sip):
    global ASR_MODE
    with Session(sip, 'speechsynth') as s:
        RELEASE.clear()
        s.speak('gated'); wait_audio(s)
        no_event(s, .3)  # First block drains; the HTTP stream remains open.
        s.send('PAUSE'); s.response()
        s.send('RESUME'); s.response()
        RELEASE.set(); s.event('SPEAK-COMPLETE', 0)
        for phrase in ('odd', 'empty', 'wrong-type', 'http-error', 'redirect', 'truncated'):
            s.speak(phrase); s.event('SPEAK-COMPLETE', 4)
        for phrase in ('slow', 'burst'):
            s.speak(phrase)
            s.send('PAUSE'); s.response()
            time.sleep(.2)
            s.rtp.setblocking(False)
            try:
                while s.rtp.recv(4096):
                    pass
            except BlockingIOError:
                pass
            s.rtp.settimeout(2)
            for _ in range(3):
                assert not any(s.rtp.recv(4096)[12:]), 'PAUSE still emitted audio'
            start = time.monotonic(); s.send('STOP'); s.response()
            assert time.monotonic()-start < .5, 'Streaming STOP was delayed'
            s.speak('after-stop'); s.event('SPEAK-COMPLETE', 0)
        s.speak('slow'); s.send('BARGE-IN-OCCURRED'); s.response()
        s.speak('after-barge-in'); s.event('SPEAK-COMPLETE', 0)
    print('PASS: TTS plays before HTTP EOF, odd chunk boundaries, underrun, PAUSE/RESUME, bad/truncated responses, backpressure, STOP/reuse', flush=True)

    with Session(sip, 'speechrecog') as s:
        FIRST_AUDIO.clear(); s.recognize()
        audio = threading.Thread(target=s.audio)
        audio.start()
        try:
            assert FIRST_AUDIO.wait(.6), 'ASR waited for the complete utterance'
            assert audio.is_alive(), 'ASR audio was not sent while capturing'
            event = s.receive(); assert 'START-OF-INPUT' in event, event
            no_event(s, .1)  # Partial/final-sentence events must not complete the MRCP request.
        finally:
            audio.join()
        event = s.event('RECOGNITION-COMPLETE', 0)
        assert '这是修正后的完整结果' in event and '&lt;&amp;&quot;' in event, event
        for mode, cause in [('empty', 1), ('error', 6), ('bad-json', 6), ('oversize', 6),
                            ('big-message', 6), ('close', 6), ('redirect', 6), ('no-start', 6), ('no-completion', 6)]:
            ASR_MODE = mode; s.recognize(); s.audio(); s.event('RECOGNITION-COMPLETE', cause)
        ASR_MODE = 'ok'; s.recognize(timeout=300); s.event('RECOGNITION-COMPLETE', 2)
        ASR_MODE = 'no-start'; s.recognize()
        start = time.monotonic(); s.send('STOP'); s.response()
        assert time.monotonic()-start < .5
        ASR_MODE = 'ok'; s.recognize(); s.audio(); s.event('RECOGNITION-COMPLETE', 0)
        # Input timers can be started while the WebSocket is active.
        s.send('RECOGNIZE', 'builtin:speech/transcribe', {
            'Content-Type': 'text/uri-list', 'Start-Input-Timers': 'false',
            'No-Input-Timeout': 300, 'Speech-Complete-Timeout': 200})
        s.response('IN-PROGRESS'); no_event(s, .4)
        s.send('START-INPUT-TIMERS'); s.response(); s.event('RECOGNITION-COMPLETE', 2)
        s.send('RECOGNIZE', 'builtin:speech/transcribe', {
            'Content-Type': 'text/uri-list', 'Recognition-Timeout': 100,
            'Speech-Complete-Timeout': 200})
        s.response('IN-PROGRESS'); s.audio(); s.event('RECOGNITION-COMPLETE', 3)
    print('PASS: ASR duplex PCM before end of input, fragmented results/ping, final text, error/timeout/no-input, STOP/reuse and input timers', flush=True)

    with Session(sip, 'speechsynth') as slow, Session(sip, 'speechsynth') as fast:
        slow.speak('slow'); fast.speak('fast'); fast.event('SPEAK-COMPLETE', 0)
        slow.close()
    with Session(sip, 'speechrecog') as slow, Session(sip, 'speechrecog') as fast:
        ASR_MODE = 'no-start'; ASR_CONNECTED.clear(); slow.recognize()
        assert ASR_CONNECTED.wait(1)
        ASR_MODE = 'ok'; fast.recognize(); fast.audio(); fast.event('RECOGNITION-COMPLETE', 0)
        slow.close()
    ASR_MODE = 'no-start'
    with Session(sip, 'speechrecog') as s:
        s.recognize(); s.close()
    ASR_MODE = 'ok'
    with Session(sip, 'speechrecog') as s:
        s.recognize(); s.audio(); s.event('RECOGNITION-COMPLETE', 0)
    assert not ERRORS, ERRORS
    print('PASS: parallel streaming sessions and close during HTTP / WebSocket startup', flush=True)


def main():
    upstream = http.server.ThreadingHTTPServer(('127.0.0.1', 0), StreamingTTS)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    loop = asyncio.new_event_loop()

    async def handshake(path, headers):
        if path != '/v1/asr/stream':
            ERRORS.append('ASR followed a redirect')
            return HTTPStatus.NOT_FOUND, [], b'wrong endpoint'
        if ASR_MODE == 'redirect':
            return HTTPStatus.FOUND, [('Location', '/unexpected-redirect')], b'redirect'

    async def start_ws():
        return await websockets.serve(asr_handler, '127.0.0.1', 0, process_request=handshake)

    ws = loop.run_until_complete(start_ws())
    thread = threading.Thread(target=loop.run_forever, daemon=True); thread.start()
    port = ws.sockets[0].getsockname()[1]
    try:
        run_server(f'http://127.0.0.1:{upstream.server_port}/api/v1/tts/stream',
                   f'ws://127.0.0.1:{port}/v1/asr/stream', exercise, streaming=True)
    finally:
        RELEASE.set(); upstream.shutdown(); upstream.server_close()

        async def stop_ws():
            ws.close(); await ws.wait_closed()

        asyncio.run_coroutine_threadsafe(stop_ws(), loop).result(5)
        loop.call_soon_threadsafe(loop.stop); thread.join(5); loop.close()


if __name__ == '__main__':
    main()
