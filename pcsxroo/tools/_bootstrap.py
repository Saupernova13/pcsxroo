"""Put pcsxroo/ on sys.path so the scripts in pcsxroo/tools can import ps2ee."""

import sys
from pathlib import Path

PCSXROO = Path(__file__).resolve().parent.parent
if str(PCSXROO) not in sys.path:
    sys.path.insert(0, str(PCSXROO))
