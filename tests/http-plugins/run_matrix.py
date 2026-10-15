"""Run real SIP/MRCP/RTP and archive regressions at both supported rates."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
for rate in (8000, 16000):
    print(f'Checking HTTP and streaming plugins at {rate} Hz', flush=True)
    subprocess.run([sys.executable, str(root/'test_integration.py')],
                   env=dict(os.environ, UNIMRCP_TEST_RATE=str(rate)), check=True)
subprocess.run([sys.executable,str(root/'test_config.py')],check=True)
