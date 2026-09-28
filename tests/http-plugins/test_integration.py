"""Real SIP/MRCP/RTP test against the compiled server; upstream HTTP is mocked.

Runs only inside the disposable Linux test container. No real voice service,
credentials, external ports, or production FreeSWITCH instance are used.
"""
import email.parser
import http.server
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import uuid
import wave
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
RATE = 16000
PCM = struct.pack('<'+'h'*3200, *(int(6000*math.sin(i*2*math.pi*440/RATE)) for i in range(3200)))
ASR_MODE = 'ok'
ASR_REQUESTS = []
TTS_REQUESTS = []
HTTP_ERRORS = []

def wav_bytes():
    f = io.BytesIO()
    with wave.open(f, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(PCM)
    return f.getvalue()

class Upstream(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            body = self.rfile.read(int(self.headers['Content-Length']))
            assert self.headers.get('X-Request-ID')
            status, content_type = 200, 'audio/wav'
            if self.path == '/tts':
                request = json.loads(body)
                assert set(request) == {'text', 'format', 'sample_rate'}
                assert request['sample_rate'] == RATE and request['format'] == 'wav'
                TTS_REQUESTS.append(request['text'])
                if request['text'] == 'slow':
                    time.sleep(3)
                payload = wav_bytes()
                if request['text'] == 'bad-wav':
                    payload = payload[:-2]
                if request['text'] == 'http-error':
                    status, payload = 503, b'upstream unavailable'
                if request['text'] == 'placeholder':
                    payload = bytearray(payload)
                    struct.pack_into('<I', payload, 4, 0x7FFFFFBF)
                    struct.pack_into('<I', payload, 40, 0x7FFFFF9B)
            else:
                message = email.parser.BytesParser().parsebytes(
                    ('Content-Type: '+self.headers['Content-Type']+'\r\nMIME-Version: 1.0\r\n\r\n').encode()+body)
                fields = {part.get_param('name', header='Content-Disposition'): part.get_payload(decode=True)
                          for part in message.walk() if part.get_content_disposition() == 'form-data'}
                assert fields['format'] == b'wav' and fields['sample_rate'] == b'16000' and fields['language_hints'] == b'zh'
                with wave.open(io.BytesIO(fields['file']), 'rb') as w:
                    assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (1, 2, RATE)
                    assert w.getnframes() > 2000
                mode = ASR_MODE
                ASR_REQUESTS.append(mode)
                if mode == 'slow':
                    time.sleep(3)
                content_type = 'application/json'
                payload = json.dumps({'text': '你好<&"测试'}).encode()
                if mode == 'empty':
                    payload = b'{"text":""}'
                if mode == 'bad-json':
                    payload = b'{"text":12}'
                if mode == 'oversize':
                    payload = json.dumps({'text': 'x'*90000}).encode()
            self.send_response(status); self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(payload))); self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when STOP / channel close cancels the HTTP request.
        except Exception as exc:
            HTTP_ERRORS.append(repr(exc))
            self.send_error(500)

def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); return s.getsockname()[1]

def check_sip_tcp(sip_port):
    # The ASR/TTS sessions below use UDP. Check a real SIP response over TCP
    # too, so omitting the explicit transport list cannot silently disable it.
    with socket.create_connection(('127.0.0.1', sip_port), timeout=5) as sip:
        call_id = uuid.uuid4().hex
        message = (f'OPTIONS sip:server@127.0.0.1:{sip_port} SIP/2.0\r\n'
                   f'Via: SIP/2.0/TCP 127.0.0.1:{sip.getsockname()[1]};branch=z9hG4bK{uuid.uuid4().hex};rport\r\n'
                   f'From: <sip:test@127.0.0.1>;tag={uuid.uuid4().hex}\r\n'
                   f'To: <sip:server@127.0.0.1:{sip_port}>\r\n'
                   f'Call-ID: {call_id}\r\nCSeq: 1 OPTIONS\r\n'
                   'Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
        sip.sendall(message.encode())
        response = b''
        while b'\r\n\r\n' not in response:
            part = sip.recv(65536)
            assert part, 'SIP TCP connection closed before response headers'
            response += part
        assert response.startswith(b'SIP/2.0 200 '), response
        assert call_id.encode() in response, response
    print('PASS: SIP OPTIONS over TCP with default transports', flush=True)

class Session:
    def __init__(self, sip_port, resource):
        self.sip_port, self.resource = sip_port, resource
        self.rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rtp.bind(('127.0.0.1', 0)); self.rtp.settimeout(2)
        self.sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sip.bind(('127.0.0.1', 0)); self.sip.settimeout(5)
        self.call_id, self.tag = uuid.uuid4().hex, uuid.uuid4().hex
        self.to = f'<sip:server@127.0.0.1:{sip_port}>'
        self.buffer = b''; self.counter = 0; self.closed = False
        self.rtp_sequence = 0; self.rtp_origin = time.monotonic()
        direction = 'recvonly' if resource == 'speechsynth' else 'sendonly'
        sdp = ('v=0\r\no=test 1 1 IN IP4 127.0.0.1\r\ns=test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n'
               'm=application 9 TCP/MRCPv2 1\r\na=setup:active\r\na=connection:new\r\n'
               f'a=resource:{resource}\r\na=cmid:1\r\nm=audio {self.rtp.getsockname()[1]} RTP/AVP 99\r\n'
               f'a=rtpmap:99 L16/16000/1\r\na={direction}\r\na=mid:1\r\n')
        invite = self.send_sip('INVITE', 1, sdp)
        # SIP is UDP here: retransmit the same transaction if the first packet
        # arrives before Sofia's receive task is ready (or is otherwise lost).
        self.sip.settimeout(.5)
        deadline = time.monotonic()+10
        while True:
            try:
                response = self.sip.recv(65536).decode()
            except socket.timeout:
                if time.monotonic() >= deadline:
                    raise
                self.sip.sendto(invite, ('127.0.0.1', self.sip_port))
                continue
            if response.startswith('SIP/2.0 100'):
                continue
            assert response.startswith('SIP/2.0 200'), response
            break
        self.to = re.search(r'^To:\s*(.+)\r?$', response, re.M|re.I)[1].strip()
        mrcp_port = int(re.search(r'm=application (\d+)', response)[1])
        self.server_rtp = int(re.search(r'm=audio (\d+)', response)[1])
        self.channel = re.search(r'a=channel:([^\r\n]+)', response)[1]
        self.send_sip('ACK', 1)
        self.control = socket.create_connection(('127.0.0.1', mrcp_port), timeout=5)
        self.control.settimeout(5)

    def send_sip(self, method, sequence, body=''):
        branch = 'z9hG4bK'+uuid.uuid4().hex
        message = (f'{method} sip:server@127.0.0.1:{self.sip_port} SIP/2.0\r\n'
                   f'Via: SIP/2.0/UDP 127.0.0.1:{self.sip.getsockname()[1]};branch={branch};rport\r\n'
                   f'From: <sip:test@127.0.0.1>;tag={self.tag}\r\nTo: {self.to}\r\nCall-ID: {self.call_id}\r\n'
                   f'CSeq: {sequence} {method}\r\nMax-Forwards: 70\r\n'
                   f'Contact: <sip:test@127.0.0.1:{self.sip.getsockname()[1]}>\r\n'
                   + ('Content-Type: application/sdp\r\n' if body else '')
                   + f'Content-Length: {len(body.encode())}\r\n\r\n'+body)
        packet = message.encode()
        self.sip.sendto(packet, ('127.0.0.1', self.sip_port))
        return packet

    def send(self, method, text='', headers=None):
        self.counter += 1
        body = text.encode()
        rest = f'{method} {self.counter}\r\nChannel-Identifier: {self.channel}\r\n'
        for key, value in (headers or {}).items():
            rest += f'{key}: {value}\r\n'
        rest += f'Content-Length: {len(body)}\r\n\r\n'
        length = 0
        while True:
            packet = f'MRCP/2.0 {length} '.encode()+rest.encode()+body
            if len(packet) == length:
                break
            length = len(packet)
        self.control.sendall(packet)
        return self.counter

    def receive(self, timeout=5):
        self.control.settimeout(timeout)
        while b'\r\n' not in self.buffer:
            part = self.control.recv(65536); assert part, 'MRCP connection closed'; self.buffer += part
        length = int(self.buffer.split(b' ', 2)[1])
        while len(self.buffer) < length:
            part = self.control.recv(65536); assert part; self.buffer += part
        data, self.buffer = self.buffer[:length], self.buffer[length:]
        return data.decode()

    def response(self, state='COMPLETE'):
        data = self.receive()
        assert f' {self.counter} 200 {state}\r\n' in data, data
        return data

    def recognize(self, timeout=2000):
        self.send('RECOGNIZE', 'builtin:speech/transcribe', {'Content-Type':'text/uri-list', 'No-Input-Timeout':timeout, 'Speech-Complete-Timeout':200})
        self.response('IN-PROGRESS')

    def speak(self, text):
        self.send('SPEAK', text, {'Content-Type':'text/plain'}); self.response('IN-PROGRESS')

    def audio(self, speech=True):
        # 0.4s voiced + 0.4s silence. Actual RTP timing, not a fast packet dump.
        start = time.monotonic()
        timestamp = int((start-self.rtp_origin)*RATE)
        for seq in range(40):
            pcm = [int(6000*math.sin((seq*320+i)*2*math.pi*440/RATE)) if speech and seq < 20 else 0 for i in range(320)]
            packet = struct.pack('!BBHII', 0x80, 99, self.rtp_sequence & 0xffff, (timestamp+seq*320) & 0xffffffff, 12345)+struct.pack('!320h', *pcm)
            self.rtp_sequence += 1
            self.rtp.sendto(packet, ('127.0.0.1', self.server_rtp))
            time.sleep(max(0, start+(seq+1)*.02-time.monotonic()))

    def event(self, name, cause):
        data = self.receive()
        if ' START-OF-INPUT ' in data:
            data = self.receive()
        assert f' {name} ' in data and f'Completion-Cause: {cause:03}' in data, data
        return data

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.send_sip('BYE', 2)
        self.sip.settimeout(3)
        try:
            while not self.sip.recv(65536).startswith(b'SIP/2.0 200'):
                pass
        finally:
            self.control.close(); self.sip.close(); self.rtp.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

def exercise(sip):
    global ASR_MODE
    with Session(sip, 'speechsynth') as s:
        for phrase in ('你好', 'placeholder'):
            s.speak(phrase)
            heard = bytearray()
            deadline = time.monotonic()+3
            while time.monotonic() < deadline:
                packet = s.rtp.recv(4096)
                if any(packet[12:]):
                    heard += packet[12:]
                if len(heard) >= len(PCM):
                    break
            assert any(heard), 'TTS emitted only silence'
            s.event('SPEAK-COMPLETE', 0)
        for phrase in ('bad-wav', 'http-error'):
            s.speak(phrase); s.event('SPEAK-COMPLETE', 4)
        s.speak('slow'); time.sleep(.1)
        start = time.monotonic(); s.send('STOP'); s.response()
        assert time.monotonic()-start < 2, 'STOP blocked behind upstream HTTP'
        s.speak('after-stop'); s.event('SPEAK-COMPLETE', 0)
    print('PASS: TTS RTP, placeholder WAV, malformed WAV/HTTP failure, STOP and reuse', flush=True)

    with Session(sip, 'speechrecog') as s:
        for mode, cause in [('ok',0), ('empty',1), ('bad-json',6), ('oversize',6)]:
            ASR_MODE=mode; s.recognize(); s.audio(); event=s.event('RECOGNITION-COMPLETE',cause)
            if mode=='ok':
                tree=ET.fromstring(event.split('\r\n\r\n',1)[1]); assert tree.find('.//input').text=='你好<&"测试'
        before=len(ASR_REQUESTS); s.recognize(timeout=300); s.event('RECOGNITION-COMPLETE',2)
        assert len(ASR_REQUESTS)==before, 'No-input should not call ASR'
        ASR_MODE='slow'; s.recognize(); s.audio()
        event=s.receive(); assert 'START-OF-INPUT' in event, event
        s.send('STOP'); s.response()
        ASR_MODE='ok'; s.recognize(); s.audio(); s.event('RECOGNITION-COMPLETE',0)
    print('PASS: ASR RTP→multipart WAV, NLSML escaping, empty/error results, no-input, STOP and reuse', flush=True)

    # Separate channel workers: one slow upstream must not block another channel.
    with Session(sip, 'speechsynth') as slow, Session(sip, 'speechsynth') as fast:
        slow.speak('slow'); start=time.monotonic(); fast.speak('concurrent'); fast.event('SPEAK-COMPLETE',0)
        assert time.monotonic()-start < 2
        slow.close()  # Close with active HTTP; late response must not touch freed channel memory.
    time.sleep(3.2)
    with Session(sip,'speechsynth') as s:
        s.speak('still-alive'); s.event('SPEAK-COMPLETE',0)
    assert not HTTP_ERRORS, HTTP_ERRORS
    print('PASS: concurrent sessions, close during HTTP, fresh session after delayed response', flush=True)

def main():
    cflags=subprocess.check_output(['pkg-config','--cflags','libcurl','json-c'],text=True).split()
    libs=subprocess.check_output(['pkg-config','--libs','libcurl','json-c'],text=True).split()
    # Keep ASan's executable mapping stable under WSL address randomization.
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-g','-no-pie','-fsanitize=address,undefined','-Iplugins/http-common',*cflags,
                    'tests/http-plugins/test_common.c','plugins/http-common/http_common.c',*libs,'-o','/tmp/test-http-common'],check=True,cwd=ROOT)
    subprocess.run(['/tmp/test-http-common'],check=True)
    upstream=http.server.ThreadingHTTPServer(('127.0.0.1',0),Upstream)
    threading.Thread(target=upstream.serve_forever,daemon=True).start()
    tmp=Path(tempfile.mkdtemp(prefix='unimrcp-http-test-'))
    shutil.copytree('/opt/unimrcp/conf',tmp/'conf')
    (tmp/'plugin').symlink_to('/opt/unimrcp/plugin')
    for name in ('data','log','var'):
        (tmp/name).mkdir()
    sip, mrcp = free_port(), free_port()
    tree=ET.parse(ROOT/'conf/unimrcpserver-http.xml')
    tree.find('.//sip-port').text=str(sip); tree.find('.//mrcp-port').text=str(mrcp)
    tree.write(tmp/'conf/unimrcpserver.xml',encoding='utf-8',xml_declaration=True)
    env=dict(os.environ,UNIMRCP_HTTP_TTS_URL=f'http://127.0.0.1:{upstream.server_port}/tts',UNIMRCP_HTTP_ASR_URL=f'http://127.0.0.1:{upstream.server_port}/asr')
    logfile=tmp/'server-console.log'
    with logfile.open('w') as log:
        process=subprocess.Popen(['stdbuf','-oL','-eL','/opt/unimrcp/bin/unimrcpserver','-r',str(tmp)],stdin=subprocess.PIPE,stdout=log,stderr=log,env=env)
        try:
            for _ in range(100):
                assert process.poll() is None, f'Server exited {process.returncode}'
                try:
                    for port in (mrcp,sip):
                        with socket.create_connection(('127.0.0.1',port),timeout=.1):
                            pass
                    break
                except OSError:
                    time.sleep(.1)
            else:
                raise AssertionError('MRCP listener did not start')
            check_sip_tcp(sip)
            exercise(sip)
            assert process.poll() is None
        except Exception:
            log.flush(); print(logfile.read_text()[-16000:],flush=True); raise
        finally:
            if process.poll() is None:
                process.stdin.write(b'quit\n'); process.stdin.flush()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
            upstream.shutdown()
    assert process.returncode==0, f'Non-clean shutdown: {process.returncode}'
    print('PASS: clean server shutdown',flush=True)

if __name__=='__main__':
    main()
