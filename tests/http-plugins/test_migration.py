"""Verify migration preserves deployed IP/ports/parameters and backend overrides."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('migration', ROOT/'deploy/single-directory/prepare_migration.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class MigrationTests(unittest.TestCase):
    def test_preserve_configuration(self):
        tree = ET.parse(ROOT/'conf/unimrcpserver-http.xml')
        tree.find('./properties/ip').text = '10.20.30.40'
        tree.find('.//sip-port').text = '8062'
        tree.find('.//mrcp-port').text = '1655'
        tree.find('.//rtsp-port').text = '1666'
        tree.find('.//rtp-port-min').text = '28000'
        tree.find('.//rtp-port-max').text = '28100'
        migration.parameter(migration.engine(tree, 'HTTP-ASR'), 'vad-threshold', '750')
        migration.parameter(migration.engine(tree, 'HTTP-ASR'), 'http-timeout-ms', '123456')
        before = ET.tostring(tree.getroot())
        after = migration.prepare_xml(before)
        for path in ('./properties/ip', './/sip-port', './/mrcp-port', './/rtsp-port',
                     './/rtp-port-min', './/rtp-port-max'):
            self.assertEqual(after.findtext(path), tree.findtext(path))
        for param in ('vad-threshold', 'http-timeout-ms'):
            self.assertEqual(migration.engine(after, 'HTTP-ASR').find('./param[@name="%s"]' % param).get('value'),
                             migration.engine(tree, 'HTTP-ASR').find('./param[@name="%s"]' % param).get('value'))
        codecs = after.find('.//rtp-settings/codecs')
        self.assertEqual(codecs.text, 'L16/96/8000 L16/99/16000')
        self.assertEqual(codecs.get('own-preference'), 'true')

    def test_running_environment_and_fresh_directory(self):
        containers = []
        for role, digit in (('http', '1'), ('stream', '2')):
            env = ['UNIMRCP_HTTP_ASR_URL=http://old-host/asr?token=$literal',
                   'UNIMRCP_HTTP_TTS_URL=http://old-host/tts']
            if role == 'stream':
                env.extend(['UNIMRCP_STREAM_ASR_URL=ws://old-host/asr/stream',
                            'UNIMRCP_STREAM_TTS_URL=http://old-host/tts/stream'])
            containers.append({'Id': digit*64, 'Name': '/old-'+role,
                               'Config': {'Env': env, 'Image': 'old-image', 'User': ''},
                               'HostConfig': {'NetworkMode': 'host', 'ExtraHosts': ['old-host:10.0.0.7'],
                                              'RestartPolicy': {'Name': 'unless-stopped'}}})
        calls = []
        def docker(*args):
            calls.append(args)
            if args[0] == 'inspect':
                return json.dumps(containers)
            if args[0] == 'image':
                return '[]'
            if args[0] == 'cp':
                source = ROOT/('conf/unimrcpserver-http.xml' if args[1].startswith('1') else
                               'deploy/stream/conf/unimrcpserver-http.xml')
                tree = ET.parse(source)
                if args[1].startswith('2'):
                    tree.find('.//sip-port').text = '8062'
                Path(args[2]).write_bytes(ET.tostring(tree.getroot()))
                return ''
            raise AssertionError('Unexpected Docker mutation: %r' % (args,))
        with tempfile.TemporaryDirectory() as tmp:
            argv = ['prepare_migration.py', '--http-container', 'old-http', '--stream-container', 'old-stream',
                    '--output-dir', tmp]
            with patch.object(migration, 'docker', docker), patch('sys.argv', argv):
                migration.main()
                with self.assertRaises(ValueError):
                    migration.main()
            compose = json.loads((Path(tmp)/'docker-compose.yml').read_text())
            ordinary = compose['services']['unimrcp-http']
            self.assertEqual(ordinary['environment']['UNIMRCP_HTTP_ASR_URL'], 'http://old-host/asr?token=$$literal')
            self.assertEqual(ordinary['extra_hosts'], ['old-host:10.0.0.7'])
            self.assertEqual(ordinary['image'], migration.IMAGE)
            self.assertEqual(ordinary['environment']['UNIMRCP_TTS_AUDIO_DIR'], migration.AUDIO_DIR)
            self.assertTrue((Path(tmp)/'old-config/http.xml').exists())
            self.assertTrue((Path(tmp)/'tts-audio/stream').is_dir())
            metadata = json.loads((Path(tmp)/'migration.json').read_text())
            self.assertEqual(metadata[1]['sip_port'], '8062')
            self.assertEqual(ET.parse(Path(tmp)/'conf/unimrcpserver-stream.xml').findtext('.//sip-port'), '8062')
        self.assertTrue(all(call[0] in ('inspect', 'image', 'cp') for call in calls))


if __name__ == '__main__':
    unittest.main()
