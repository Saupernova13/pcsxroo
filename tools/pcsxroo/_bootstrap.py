"""Put tools/pcsxroo on sys.path so the scripts here can import ps2ee."""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
