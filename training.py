"""Generate a small synthetic workload for GCC's build-time profiling pass."""

import datetime as dt
import hashlib
import json
import random
from pathlib import Path


def opaque_id(prefix, number, short_width=8):
    mixed = (number * 2654435761 + 2246822519) & 0xFFFFFFFF
    if mixed % 8:
        return prefix + f"{mixed:08x}"[-short_width:]
    value = (prefix + str(number)).encode()
    return prefix + hashlib.blake2s(value, digest_size=16).hexdigest()


def write_training_data(root: Path):
    rng = random.Random(926147)
    start = int(dt.datetime(2023, 4, 1, tzinfo=dt.timezone.utc).timestamp())

    def row(values):
        # The generated values contain no embedded commas, line breaks or quotes.
        return ",".join(
            f'"{value}"' if rng.randrange(5) == 0 else str(value) for value in values
        ) + "\n"

    def timestamp(seconds, form):
        if form < 6:
            return dt.datetime.fromtimestamp(seconds, dt.timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"
            )
        if form == 6:
            offset = dt.timedelta(minutes=rng.choice((-330, 180, 540)))
            return dt.datetime.fromtimestamp(seconds, dt.timezone(offset)).isoformat(
                timespec="seconds"
            )
        return str(seconds * 1000 + rng.randrange(1000))

    for name in ("events", "clicks", "links"):
        (root / name).mkdir(parents=True)
    window = {"window_start": "2023-04-01T00:00:00Z", "window_end": "2023-04-22T00:00:00Z"}
    (root / "manifest.json").write_text(json.dumps(window))
    users = [opaque_id("u", i) for i in range(3200)]
    products = [f"p{i:05x}" for i in range(1200)]
    campaigns = [f"c{i:04x}" for i in range(96)]
    with (root / "products.csv").open("w") as output:
        output.write("product_id,category\n")
        output.writelines(row((product, f"category{i % 21}")) for i, product in enumerate(products))
    with (root / "campaigns.csv").open("w") as output:
        output.write("campaign_id,channel\n")
        output.writelines(row((campaign, f"channel{i % 5}")) for i, campaign in enumerate(campaigns))
    with (root / "links" / "part.csv").open("w") as output:
        output.write("user_id_a,user_id_b\n")
        output.writelines(row((user, users[i - i % 3])) for i, user in enumerate(users) if i % 3)

    for part in range(6):
        with (root / "events" / f"{part}.csv").open("w") as output:
            output.write("event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents\n")
            for number in range(part * 6500, (part + 1) * 6500):
                event = opaque_id("e", number)
                user = rng.choice(users)
                seconds = start + rng.randrange(21 * 86400)
                stamp = timestamp(seconds, rng.randrange(10))
                kind = rng.choices(("view", "cart", "purchase"), weights=(84, 12, 4))[0]
                product = rng.choice(products)
                price = rng.randrange(10, 50000) if kind == "purchase" else ""
                if number % 277 == 0:
                    stamp = "2023-04-31T03:12:24Z"
                if number % 571 == 0:
                    user = ""
                if number % 41 == 0:
                    user = users[number % 7]
                ingested = seconds + rng.randrange(86401)
                output.write(row((event, user, stamp, timestamp(ingested, 0), kind, product, price)))
                if number % 17 == 0:
                    output.write(row((event, user, stamp, timestamp(ingested + 60, 0), kind, product, price)))
                if number % 773 == 0:
                    output.write('"invalid,short,row\n')

    for part in range(3):
        with (root / "clicks" / f"{part}.csv").open("w") as output:
            output.write("click_id,user_id,campaign_id,ts\n")
            for number in range(part * 3400, (part + 1) * 3400):
                seconds = start + rng.randrange(-7 * 86400, 21 * 86400)
                values = (opaque_id("k", number, 7), rng.choice(users), rng.choice(campaigns),
                          timestamp(seconds, rng.randrange(10)))
                output.write(row(values))
                if number % 59 == 0:
                    output.write(row(values))
