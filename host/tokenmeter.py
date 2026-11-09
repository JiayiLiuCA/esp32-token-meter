# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak>=0.22"]
# ///
"""Token Meter host: computes the token and cost totals T3 Code's usage page
shows (Past 24h / 7 / 30 / 90 days) and pushes them to the
ESP32-S3-RLCD-4.2 over BLE every 5 minutes.

Parity with T3 Code (desktop app bundle, apps/server):
  * records (src/provider/Drivers/claudeUsage.ts): transcript lines with
    type == "assistant" and message.usage; model "<synthetic>" skipped;
    advisor iterations become extra records.
  * dedupe key "<message.id>:<requestId>" (advisor: "...:advisor:<i>"),
    first occurrence wins across the whole scan.
  * tokens = input + cache_read + cache_creation + output.
  * cost (src/usage/usagePricing.ts): costUSD when present, else list price
    from ~/.t3/userdata/usage-model-rates.json; "fast" uses
    provider_specific_entry.fast as a multiplier, else *_priority rates.
  * windows (web session-logic `ct`): Past 24h = [now floored to the minute
    - 24h, that minute); N days = local calendar days today-(N-1) .. today.
  * Claude Code deletes old transcripts (cleanupPeriodDays, 30 by default);
    T3 Code keeps their records in usage-scan-cache-v11.json. Records of
    cached files no longer on disk are merged in from there, so 30/90 days
    match T3 Code.

Usage:
  uv run host/tokenmeter.py            # scan + push to the device forever
  uv run host/tokenmeter.py --print    # print the current numbers and exit
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import logging
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

SERVICE_UUID = "7e1a0001-6b2c-4d3e-9f10-746f6b6d7472"
RX_UUID = "7e1a0002-6b2c-4d3e-9f10-746f6b6d7472"
DEVICE_NAME = "TokenMeter"

PUSH_INTERVAL_S = 300
PERIOD_DAYS = (1, 7, 30, 90)  # 1 = Past 24h (rolling), others calendar days
HORIZON_DAYS = max(PERIOD_DAYS)

HOME = Path.home()
CLAUDE_DIR = Path(os.environ.get("CLAUDE_CONFIG_DIR") or HOME / ".claude")
T3_HOME = Path(os.environ.get("T3CODE_HOME") or HOME / ".t3")
RATES_FILE = T3_HOME / "userdata" / "usage-model-rates.json"
T3_CACHE_FILE = T3_HOME / "userdata" / "usage-scan-cache-v11.json"
T3_SPEEDS = ("standard", "fast", "ultrafast")

UNPRICEABLE = {"<synthetic>", "synthetic", "opus", "sonnet", "haiku", "fable"}
FIRST_PARTY = {"anthropic", "deepseek", "gemini", "meta", "mistral", "openai", "xai"}

log = logging.getLogger("tokenmeter")


# --------------------------------------------------------------------------
# Pricing (mirror of T3 Code usagePricing.ts)

@dataclass(frozen=True)
class Rates:
    inp: float
    out: float
    cache_read: float
    cache_write: float


def _num(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def _read_rates(entry: dict, suffix: str, std: Rates | None) -> Rates | None:
    inp = _num(entry.get(f"input_cost_per_token{suffix}"))
    out = _num(entry.get(f"output_cost_per_token{suffix}"))
    if inp is None or out is None:
        return None

    def cache(name: str, attr: str) -> float:
        v = _num(entry.get(f"{name}{suffix}"))
        if v is not None:
            return v
        if std is not None and std.inp > 0:
            return getattr(std, attr) / std.inp * inp
        return inp

    return Rates(inp, out, cache("cache_read_input_token_cost", "cache_read"),
                 cache("cache_creation_input_token_cost", "cache_write"))


def _scale(r: Rates, k: float) -> Rates:
    return Rates(r.inp * k, r.out * k, r.cache_read * k, r.cache_write * k)


class RateTable:
    """model key -> {"standard": Rates, "fast": Rates|None, "ultrafast": Rates|None}"""

    def __init__(self, path: Path):
        self.path = path
        self.mtime = -1.0
        self.table: dict[str, dict[str, Rates | None]] = {}

    def refresh(self) -> None:
        try:
            m = self.path.stat().st_mtime
        except FileNotFoundError:
            if not self.table:
                log.warning("no T3 Code rate table at %s; costs will be $0", self.path)
            return
        if m == self.mtime:
            return
        doc = json.loads(self.path.read_text()).get("document", {})
        table: dict[str, dict[str, Rates | None]] = {}
        for name, entry in doc.items():
            if not isinstance(entry, dict):
                continue
            std = _read_rates(entry, "", None)
            if std is None:
                continue
            key = name.strip().lower()
            if not key:
                continue
            spec = entry.get("provider_specific_entry")
            mult = _num(spec.get("fast")) if isinstance(spec, dict) else None
            table[key] = {
                "standard": std,
                "fast": _scale(std, mult) if mult and mult > 0 else _read_rates(entry, "_priority", std),
                "ultrafast": _read_rates(entry, "_ultrafast", std),
            }
        # Bare-name aliases ("vendor/claude-x" -> "claude-x"), first party wins.
        aliases: dict[str, tuple[dict, bool]] = {}
        for key, rate in table.items():
            bare = key.rsplit("/", 1)[-1]
            if bare == key or bare in table:
                continue
            fp = key.split("/", 1)[0] in FIRST_PARTY
            held = aliases.get(bare)
            if held is None or (fp and not held[1]):
                aliases[bare] = (rate, fp)
        for bare, (rate, _) in aliases.items():
            table[bare] = rate
        self.table = table
        self.mtime = m
        log.info("loaded %d model rates", len(table))

    def price(self, model: str, tot: tuple[int, int, int, int], speed: str,
              reported: float | None) -> tuple[float, float]:
        """(cost, cache savings) in USD; savings = cache_read * (input - cache_read rate)."""
        key = model.strip().lower().split("[", 1)[0]
        bare = key.rsplit("/", 1)[-1]
        rate = None if (not bare or bare in UNPRICEABLE) else self.table.get(key)
        if rate is None:
            return (reported or 0.0), 0.0
        r = (rate.get(speed) if speed != "standard" else None) or rate["standard"]
        unc, cr, cw, out = tot
        listed = unc * r.inp + cr * r.cache_read + cw * r.cache_write + out * r.out
        return (reported if reported is not None else listed), cr * (r.inp - r.cache_read)


# --------------------------------------------------------------------------
# Records

@dataclass(frozen=True)
class Rec:
    ts_ms: float
    model: str
    tot: tuple[int, int, int, int]  # uncached input, cache read, cache write, output
    speed: str
    reported: float | None
    key: str | None
    session: str = ""


def _count(v) -> int:
    return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0 else 0


def parse_line(line: str):
    """Claude Code transcript line -> Rec(s) (mirror of claudeUsage.ts)."""
    try:
        rec = json.loads(line)
    except ValueError:
        return
    if not isinstance(rec, dict) or rec.get("type") != "assistant":
        return
    msg = rec.get("message")
    if not isinstance(msg, dict):
        return
    usage = msg.get("usage")
    if not isinstance(usage, dict):
        return
    ts = rec.get("timestamp")
    if not isinstance(ts, str):
        return
    try:
        ts_ms = dt.datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp() * 1000
    except ValueError:
        return
    model = msg.get("model") if isinstance(msg.get("model"), str) else ""
    if not model or model == "<synthetic>":
        return
    mid = msg.get("id") if isinstance(msg.get("id"), str) else None
    rid = rec.get("requestId") if isinstance(rec.get("requestId"), str) else None
    key = None if mid is None and rid is None else f"{mid or ''}:{rid or ''}"
    speed = "fast" if usage.get("speed") == "fast" else "standard"
    tot = (_count(usage.get("input_tokens")), _count(usage.get("cache_read_input_tokens")),
           _count(usage.get("cache_creation_input_tokens")), _count(usage.get("output_tokens")))
    session = rec.get("sessionId") if isinstance(rec.get("sessionId"), str) else ""
    yield Rec(ts_ms, model, tot, speed, _num(rec.get("costUSD")), key, session)
    iters = usage.get("iterations")
    if isinstance(iters, list):
        for i, it in enumerate(iters):
            if not isinstance(it, dict) or it.get("type") != "advisor_message":
                continue
            am = it.get("model") if isinstance(it.get("model"), str) else ""
            if not am or am == "<synthetic>":
                continue
            atot = (_count(it.get("input_tokens")), _count(it.get("cache_read_input_tokens")),
                    _count(it.get("cache_creation_input_tokens")), _count(it.get("output_tokens")))
            yield Rec(ts_ms, am, atot, speed, None, None if key is None else f"{key}:advisor:{i}", session)


def t3_cache_records(path: Path, since_ms: float):
    """Records T3 Code kept for transcripts that are gone from disk.

    Cache row layout (usageScanCache.ts serializeRecord): [timestampMs,
    modelIdx, sessionIdx, uncached, cached, cacheCreation, output, reasoning,
    dedupeKey, reportedCostUsd, speedIdx, rateModelIdx|null, ...].
    """
    try:
        doc = json.loads(path.read_text())
    except (FileNotFoundError, ValueError):
        return
    models = doc.get("models") or []
    sessions = doc.get("sessions") or []
    for fpath, entry in (doc.get("files") or {}).items():
        if os.path.exists(fpath):
            continue  # live file: scanned directly
        for r in (entry.get("r") or []) + (entry.get("t") or []):
            try:
                if r[0] < since_ms:
                    continue
                rate_model = models[r[11]] if len(r) > 11 and r[11] is not None else models[r[1]]
                speed = T3_SPEEDS[r[10]] if 0 <= r[10] < len(T3_SPEEDS) else "standard"
                session = sessions[r[2]] if isinstance(r[2], int) and 0 <= r[2] < len(sessions) else ""
                yield Rec(r[0], rate_model, (r[3], r[4], r[5], r[6]), speed, _num(r[9]), r[8], session)
            except (IndexError, TypeError):
                continue


# --------------------------------------------------------------------------
# Scanner

@dataclass
class FileState:
    offset: int = 0


@dataclass
class Scanner:
    rates: RateTable
    root: Path = CLAUDE_DIR / "projects"
    t3_cache: Path = T3_CACHE_FILE
    files: dict[str, FileState] = field(default_factory=dict)
    seen: set[str] = field(default_factory=set)
    # (ts_ms, tokens, cost, session id) for every record inside the 90-day horizon
    entries: list[tuple[float, int, float, str]] = field(default_factory=list)
    _t3_mtime: float = -1.0

    @staticmethod
    def horizon_ms() -> float:
        start = dt.date.today() - dt.timedelta(days=HORIZON_DAYS - 1)
        return dt.datetime.combine(start, dt.time()).timestamp() * 1000

    def _add(self, rec: Rec, since_ms: float, out: list) -> None:
        if rec.key is not None:
            if rec.key in self.seen:
                return
            self.seen.add(rec.key)
        if rec.ts_ms < since_ms:
            return
        cost, _savings = self.rates.price(rec.model, rec.tot, rec.speed, rec.reported)
        out.append((rec.ts_ms, sum(rec.tot), cost, rec.session))

    def scan(self) -> None:
        self.rates.refresh()
        since_ms = self.horizon_ms()
        for dirpath, _dirs, names in os.walk(self.root):
            for n in names:
                if not n.endswith(".jsonl"):
                    continue
                p = os.path.join(dirpath, n)
                try:
                    st = os.stat(p)
                except FileNotFoundError:
                    continue
                fs = self.files.get(p)
                if fs is None:
                    if st.st_mtime * 1000 < since_ms:  # nothing inside the horizon
                        self.files[p] = FileState(st.st_size)
                        continue
                    fs = self.files[p] = FileState()
                if st.st_size < fs.offset:  # rewritten; dedupe keeps re-reads safe
                    fs.offset = 0
                if st.st_size > fs.offset:
                    self._read(p, fs, since_ms)
        self._merge_t3_cache(since_ms)
        self.entries = [e for e in self.entries if e[0] >= since_ms]

    def _read(self, path: str, fs: FileState, since_ms: float) -> None:
        with open(path, "rb") as f:
            f.seek(fs.offset)
            data = f.read()
        end = data.rfind(b"\n")
        if end < 0:
            return
        fs.offset += end + 1
        for raw in data[: end + 1].split(b"\n"):
            if b'"usage"' not in raw or b'"assistant"' not in raw:
                continue
            for rec in parse_line(raw.decode("utf-8", "replace")):
                self._add(rec, since_ms, self.entries)

    def _merge_t3_cache(self, since_ms: float) -> None:
        try:
            m = self.t3_cache.stat().st_mtime
        except FileNotFoundError:
            return
        if m == self._t3_mtime:
            return
        added: list[tuple[float, int, float, str]] = []
        for rec in t3_cache_records(self.t3_cache, since_ms):
            self._add(rec, since_ms, added)
        self.entries.extend(added)
        self._t3_mtime = m
        log.info("merged %d records from T3 Code's cache of deleted transcripts", len(added))

    def periods(self, now: dt.datetime | None = None) -> list[tuple[int, float, int]]:
        """(tokens, cost, sessions) per PERIOD_DAYS window.

        sessions = distinct non-empty sessionIds among the window's records
        (T3 Code: UsageService `distinctSessions`)."""
        now = now or dt.datetime.now()
        until = now.replace(second=0, microsecond=0).timestamp() * 1000
        bounds = []
        for days in PERIOD_DAYS:
            if days == 1:
                bounds.append((until - 24 * 3600 * 1000, until))
            else:
                start = dt.datetime.combine(now.date() - dt.timedelta(days=days - 1), dt.time())
                bounds.append((start.timestamp() * 1000, float("inf")))
        sums = [[0, 0.0, set()] for _ in bounds]
        for ts, tok, cost, session in self.entries:
            for i, (a, b) in enumerate(bounds):
                if a <= ts < b:
                    sums[i][0] += tok
                    sums[i][1] += cost
                    if session:
                        sums[i][2].add(session)
        return [(t, c, len(s)) for t, c, s in sums]


# --------------------------------------------------------------------------
# Snapshot

def snapshot(sc: Scanner) -> dict:
    return {"v": 4, "p": [[tok, int(round(cost * 100)), n] for tok, cost, n in sc.periods()]}


def encode(snap: dict) -> bytes:
    return json.dumps(snap, separators=(",", ":")).encode()


def human(n: float) -> str:
    for unit, k in (("B", 1e9), ("M", 1e6), ("K", 1e3)):
        if n >= k:
            return f"{n / k:.1f}{unit}"
    return str(int(n))


def print_report(sc: Scanner) -> None:
    labels = ["Past 24h", "7 days", "30 days", "90 days"]
    for label, (tok, cost, n) in zip(labels, sc.periods()):
        print(f"{label:<9} {human(tok):>7} tokens  ({tok:,})   cost ${cost:,.2f}   {n:,} sessions")
    print(f"payload {len(encode(snapshot(sc)))} bytes")


# --------------------------------------------------------------------------
# BLE

async def find_device(timeout: float = 10.0):
    from bleak import BleakScanner

    def match(d, adv):
        return SERVICE_UUID in (adv.service_uuids or []) or (adv.local_name or d.name) == DEVICE_NAME

    # The firmware keeps advertising while a connection slot is free, so a
    # board the OS is already connected to is still found here.
    return await BleakScanner.find_device_by_filter(match, timeout=timeout)


async def run(sc: Scanner) -> None:
    from bleak import BleakClient

    loop = asyncio.get_running_loop()
    while True:
        dev = await find_device()
        if dev is None:
            log.info("device not found; retrying in 30s")
            await asyncio.sleep(30)
            continue
        log.info("connecting to %s", dev.address)
        disconnected = asyncio.Event()
        try:
            async with BleakClient(dev, disconnected_callback=lambda _c: disconnected.set()) as client:
                log.info("connected")
                while not disconnected.is_set():
                    await loop.run_in_executor(None, sc.scan)
                    snap = snapshot(sc)
                    await client.write_gatt_char(RX_UUID, encode(snap), response=True)
                    log.info("sent 24h=%s $%.2f", human(snap["p"][0][0]), snap["p"][0][1] / 100)
                    try:
                        await asyncio.wait_for(disconnected.wait(), PUSH_INTERVAL_S)
                    except asyncio.TimeoutError:
                        pass
        except Exception as e:  # noqa: BLE001 - keep the daemon alive
            log.warning("link error: %s", e)
        log.info("disconnected")
        await asyncio.sleep(5)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--print", action="store_true", help="print current numbers and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s", stream=sys.stderr)
    sc = Scanner(RateTable(RATES_FILE))
    t0 = time.monotonic()
    sc.scan()
    log.info("initial scan %.1fs, %d files, %d records in window",
             time.monotonic() - t0, len(sc.files), len(sc.entries))
    if args.print:
        print_report(sc)
        return
    asyncio.run(run(sc))


if __name__ == "__main__":
    main()
