"""Run the native sessionizer and encode its two tables as typed Parquet."""

import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

os.environ.setdefault("POLARS_MAX_THREADS", os.environ.get("NCPU", "8"))
import polars as pl


USER = {
    "user_id": pl.String, "day": pl.Int32, "sessions": pl.Int64, "events": pl.Int64,
    "revenue_cents": pl.Int64, "top_category": pl.String,
}
CAMPAIGN = {
    "campaign_id": pl.String, "channel": pl.String, "day": pl.Int32, "sessions": pl.Int64,
    "purchasing_sessions": pl.Int64, "revenue_cents": pl.Int64, "assisted_sessions": pl.Int64,
}


def epoch_millis(value):
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    return str((datetime.fromisoformat(value.replace("Z", "+00:00")) - epoch) // timedelta(milliseconds=1))


def run(data, output):
    manifest = json.loads((data / "manifest.json").read_text())
    scratch = Path(os.environ.get("SCRATCH_DIR", tempfile.gettempdir()))
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch) as directory:
        stage = Path(directory)
        subprocess.run(
            [Path(__file__).with_name("sessions"), data, stage, epoch_millis(manifest["window_start"]),
             epoch_millis(manifest["window_end"]), os.environ.get("NCPU", "8")], check=True,
        )
        for name, file, schema in [
            ("user_daily", "user-*.csv", USER),
            ("campaign_daily", "campaign.csv", CAMPAIGN),
        ]:
            folder = output / name
            folder.mkdir(parents=True, exist_ok=True)
            table = pl.scan_csv(stage / file, schema=schema).with_columns(pl.col("day").cast(pl.Date))
            table.sink_parquet(folder / "part-0.parquet", compression="snappy")


if __name__ == "__main__":
    data = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATA_DIR", "/data"))
    output = Path(sys.argv[2] if len(sys.argv) > 2 else os.environ.get("OUTPUT_DIR", "/output"))
    run(data, output)
