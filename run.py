"""Launch the native sessionizer with the dataset manifest window."""

import json
import os
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

def epoch_millis(value):
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    return str((datetime.fromisoformat(value.replace("Z", "+00:00")) - epoch) // timedelta(milliseconds=1))


def run(data, output):
    manifest = json.loads((data / "manifest.json").read_text())
    binary = Path(__file__).resolve().with_name("sessions")
    os.execv(binary, [binary, data, output, epoch_millis(manifest["window_start"]),
                      epoch_millis(manifest["window_end"]), os.environ.get("NCPU", "8")])


if __name__ == "__main__":
    data = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATA_DIR", "/data"))
    output = Path(sys.argv[2] if len(sys.argv) > 2 else os.environ.get("OUTPUT_DIR", "/output"))
    run(data, output)
