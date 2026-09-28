"""Check an already running loopback UniMRCP server through SIP/MRCP/RTP.

Run inside the existing project image. The server chooses its HTTP backends;
this client neither starts another server nor calls a model's HTTP API.
"""
import argparse
import re
import select
import struct
import sys
import time
import wave
import xml.etree.ElementTree as ET

# Available in the original 1.8.0 image; importing does not run its test main.
sys.path.insert(0, '/src/tests/http-plugins')
from test_integration import RATE, Session


def completed(message, name):
    first_line = message.split('\r\n', 1)[0]
    if f' {name} ' not in first_line:
        raise RuntimeError(f'Unexpected MRCP message: {first_line}')
    cause = re.search(r'^Completion-Cause:\s*(\d{3})[^\r\n]*', message, re.M | re.I)
    if not cause or cause[1] != '000':
        raise RuntimeError(f'{name}: {cause[0] if cause else "missing Completion-Cause"}')


def rtp_audio(packet):
    if len(packet) < 12 or packet[0] >> 6 != 2 or packet[1] & 0x7f != 99:
        raise RuntimeError('Expected RTP v2 with negotiated L16 payload type 99')
    offset = 12 + 4 * (packet[0] & 15)
    if packet[0] & 0x10:
        if len(packet) < offset + 4:
            raise RuntimeError('Truncated RTP extension')
        offset += 4 + 4 * struct.unpack_from('!H', packet, offset + 2)[0]
    end = len(packet)
    if packet[0] & 0x20:
        padding = packet[-1]
        if not padding or padding > end - offset:
            raise RuntimeError('Invalid RTP padding')
        end -= padding
    if end <= offset or (end - offset) % 2:
        raise RuntimeError('Invalid L16 RTP payload')
    samples = struct.unpack(f'!{(end-offset)//2}h', packet[offset:end])
    return struct.pack(f'<{len(samples)}h', *samples)


def synthesize(port, text, timeout, output):
    with Session(port, 'speechsynth') as session:
        session.speak(text)
        audio = bytearray()
        deadline = time.monotonic() + timeout
        drain_until = None
        while True:
            now = time.monotonic()
            if drain_until is not None and now >= drain_until:
                break
            if now >= deadline:
                raise TimeoutError('Waiting for TTS RTP / SPEAK-COMPLETE timed out')
            if session.buffer:
                ready = [session.control]
            else:
                ready, _, _ = select.select([session.rtp, session.control], [], [], .1)
            if session.rtp in ready:
                pcm = rtp_audio(session.rtp.recv(65536))
                # The media engine may send silence while HTTP synthesis runs.
                if audio or any(pcm):
                    audio.extend(pcm)
                if len(audio) > RATE * 2 * 30:
                    raise RuntimeError('Use a short test phrase (under 30 seconds)')
            if session.control in ready:
                event = session.receive(timeout=max(.1, deadline-time.monotonic()))
                completed(event, 'SPEAK-COMPLETE')
                # MRCP completion and the final UDP packets can arrive in either order.
                drain_until = time.monotonic() + .3
        while audio[-2:] == b'\x00\x00':
            del audio[-2:]
        if not audio:
            raise RuntimeError('TTS completed but no non-silent RTP audio arrived')
        with wave.open(output, 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(RATE)
            wav.writeframes(audio)
        print(f'PASS: TTS SPEAK-COMPLETE 000; received {len(audio)//2} samples via RTP; {output}', flush=True)
        return bytes(audio)


def recognize(port, audio, timeout):
    with Session(port, 'speechrecog') as session:
        session.send('RECOGNIZE', 'builtin:speech/transcribe', {
            'Content-Type': 'text/uri-list',
            'No-Input-Timeout': 10000,
            'Speech-Complete-Timeout': 1000,
        })
        session.response('IN-PROGRESS')
        # Replay the captured TTS audio, followed by 1.5 seconds of silence.
        # WAV uses little-endian PCM; RTP L16 uses network byte order.
        audio += b'\x00' * (RATE * 2 * 3 // 2)
        frame_bytes = RATE // 50 * 2
        started = time.monotonic()
        for sequence, offset in enumerate(range(0, len(audio), frame_bytes)):
            pcm = audio[offset:offset+frame_bytes].ljust(frame_bytes, b'\x00')
            samples = struct.unpack(f'<{frame_bytes//2}h', pcm)
            packet = struct.pack('!BBHII', 0x80, 99, sequence & 0xffff,
                                 offset // 2, 12345)
            packet += struct.pack(f'!{len(samples)}h', *samples)
            session.rtp.sendto(packet, ('127.0.0.1', session.server_rtp))
            time.sleep(max(0, started+(sequence+1)*.02-time.monotonic()))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Waiting for RECOGNITION-COMPLETE timed out')
            event = session.receive(timeout=remaining)
            if ' START-OF-INPUT ' in event.split('\r\n', 1)[0]:
                continue
            completed(event, 'RECOGNITION-COMPLETE')
            result = ET.fromstring(event.split('\r\n\r\n', 1)[1])
            text = next((''.join(node.itertext()).strip() for node in result.iter()
                         if node.tag.rsplit('}', 1)[-1] == 'input'), '')
            if not text:
                raise RuntimeError('ASR succeeded but returned no recognized text')
            print(f'PASS: ASR RECOGNITION-COMPLETE 000; NLSML text: {text}', flush=True)
            return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sip-port', type=int, default=8060)
    parser.add_argument('--text', default='你好，这是一段语音测试。')
    parser.add_argument('--timeout', type=float, default=90,
                        help='Seconds to wait for each model response (default: 90)')
    parser.add_argument('--output', default='/tmp/unimrcp-check.wav')
    args = parser.parse_args()
    if not 1 <= args.sip_port <= 65535 or not 1 <= args.timeout <= 300 or not args.text.strip():
        parser.error('Use a valid SIP port, a timeout between 1 and 300, and nonempty text')
    print(f'Checking existing UniMRCP at 127.0.0.1:{args.sip_port}; text: {args.text}', flush=True)
    audio = synthesize(args.sip_port, args.text, args.timeout, args.output)
    recognize(args.sip_port, audio, args.timeout)
    print('PASS: SIP -> MRCP -> server HTTP plugins -> RTP roundtrip. Compare recognized text with the input phrase.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, RuntimeError, ValueError, ET.ParseError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr, flush=True)
        sys.exit(1)
