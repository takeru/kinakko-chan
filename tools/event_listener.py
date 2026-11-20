#!/usr/bin/env python3
"""KinakoCore2 イベント常駐リスナー

Worker の /admin/ws に直接 WebSocket で接続し、端末イベントを
受信した瞬間にスプールファイル (JSONL) へ追記する。
フック (kinako-core2-events) がスプールを読んで きなこを起こす。
接続が切れたら指数バックオフで再接続し続ける (systemd が監督する)。

設定は cloudflare/keys.env (CF_BASE / ADMIN_KEY) から読む。値は表示しない。
環境変数で上書き可: KINAKO_KEYS / KINAKO_SPOOL / KINAKO_HEARTBEAT
"""
import asyncio
import json
import os
import time
from pathlib import Path

import websockets

BASE = Path(__file__).resolve().parent.parent
KEYS = Path(os.environ.get("KINAKO_KEYS", BASE / "cloudflare" / "keys.env"))
SPOOL = Path(os.environ.get(
    "KINAKO_SPOOL", Path.home() / "hooks" / "state" / "kinako-ws-spool.jsonl"))
HEARTBEAT = Path(os.environ.get(
    "KINAKO_HEARTBEAT", Path.home() / "hooks" / "state" / "kinako-ws-listener.heartbeat"))


def load_keys():
    cfg = {}
    for line in KEYS.read_text().splitlines():
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            cfg[k.strip()] = v.strip()
    return cfg["CF_BASE"].rstrip("/"), cfg["ADMIN_KEY"]


def beat():
    HEARTBEAT.parent.mkdir(parents=True, exist_ok=True)
    HEARTBEAT.write_text(str(int(time.time())))


def append_spool(obj):
    SPOOL.parent.mkdir(parents=True, exist_ok=True)
    with SPOOL.open("a") as f:
        f.write(json.dumps(obj) + "\n")
    # 肥大化防止: 200行を超えたら直近100行だけ残す (正本は DO のイベントログ)
    try:
        lines = SPOOL.read_text().splitlines()
        if len(lines) > 200:
            SPOOL.write_text("\n".join(lines[-100:]) + "\n")
    except OSError:
        pass


async def run_once(url, key):
    # proxy=None: プロキシを介さず直接つなぐ (このVMでは直接TCP/TLSが許可済み)
    async with websockets.connect(url, additional_headers={"X-Admin-Key": key},
                                  proxy=None, ping_interval=20,
                                  ping_timeout=20) as ws:
        print(f"[listener] connected {url}", flush=True)
        beat()
        while True:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=30)
            except asyncio.TimeoutError:
                beat()  # 無音でも接続は生きている (ping/pong はライブラリ処理)
                continue
            beat()
            try:
                obj = json.loads(msg)
            except ValueError:
                continue
            if obj.get("type") == "event":
                append_spool(obj)
                print(f"[listener] event {obj.get('event')} ts={obj.get('ts')}",
                      flush=True)


async def main():
    base, key = load_keys()
    url = base.replace("https://", "wss://").replace("http://", "ws://") + "/admin/ws"
    backoff = 1
    while True:
        try:
            await run_once(url, key)
            backoff = 1
        except Exception as e:  # 接続失敗/切断 → バックオフして再接続
            print(f"[listener] disconnected: {type(e).__name__} "
                  f"(retry in {backoff}s)", flush=True)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 60)


if __name__ == "__main__":
    asyncio.run(main())
