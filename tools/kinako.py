#!/usr/bin/env python3
"""KinakoCore2 きなこ側CLI。

使い方:
  kinako.py validate scenes/hello.json        DSL検証
  kinako.py stage scenes/hello.json            seq採番して feed/scene.json に配置 (後はgit pushで配信)
  kinako.py notify "タイトル" "本文"            通知シーンを生成して stage まで一気に
  kinako.py say "セリフ" [--wav path.wav]       おしゃべりシーンを生成 (wavは feed/audio/ へ)
  kinako.py face happy                          表情だけ変えるシーンを生成
  kinako.py manifest --version 1.1.0 --bin-url https://.../kinako_core2.bin
                                               OTAマニフェストを feed/manifest.json に生成
配信 = feed/ をGitHubリポジトリにcommit&pushすること。端末は10秒以内に拾う。
"""
import argparse, hashlib, json, os, shutil, sys, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FEED = ROOT / "feed"
ACTIONS = {"face", "say", "notify", "beep", "tone_seq", "wait", "led_text", "brightness", "clear_text"}
EMOTIONS = {"normal", "happy", "sleep", "surprise", "sad", "love", "think", "dizzy"}
EVENTS = {"on_touch", "on_button_a", "on_button_b", "on_button_c", "on_shake", "on_tilt"}


def check_steps(steps, where):
    if not isinstance(steps, list):
        return [f"{where}: stepsは配列で"]
    errs = []
    for i, s in enumerate(steps):
        if not isinstance(s, dict) or s.get("do") not in ACTIONS:
            errs.append(f"{where}[{i}]: 未知のdo: {s.get('do') if isinstance(s, dict) else s}")
            continue
        if s["do"] == "face" and s.get("emotion", "normal") not in EMOTIONS:
            errs.append(f"{where}[{i}]: 未知のemotion: {s.get('emotion')}")
        if s["do"] == "say" and not s.get("text") and not s.get("audio_url"):
            errs.append(f"{where}[{i}]: sayにはtextかaudio_urlが要る")
    return errs


def validate(scene):
    errs = []
    if not isinstance(scene.get("seq"), int):
        errs.append("seq (整数) が要る")
    errs += check_steps(scene.get("steps", []), "steps")
    for k, v in scene.items():
        if k in EVENTS:
            errs += check_steps(v, k)
    return errs


def next_seq():
    cur = FEED / "scene.json"
    if cur.exists():
        try:
            return int(json.loads(cur.read_text()).get("seq", 0)) + 1
        except Exception:
            return 1
    return 1


def stage(scene):
    FEED.mkdir(exist_ok=True)
    scene["seq"] = next_seq()
    errs = validate(scene)
    if errs:
        print("検証エラー:", *errs, sep="\n  "); sys.exit(1)
    (FEED / "scene.json").write_text(json.dumps(scene, ensure_ascii=False, indent=2))
    print(f"staged: feed/scene.json seq={scene['seq']} name={scene.get('name','')}")
    print("→ feed/ をGitHubへpushすると端末が約10秒で実行します")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("validate"); p.add_argument("file")
    p = sub.add_parser("stage"); p.add_argument("file")
    p = sub.add_parser("notify"); p.add_argument("title"); p.add_argument("body")
    p = sub.add_parser("say"); p.add_argument("text"); p.add_argument("--wav")
    p = sub.add_parser("face"); p.add_argument("emotion", choices=sorted(EMOTIONS))
    p = sub.add_parser("manifest"); p.add_argument("--version", required=True); p.add_argument("--bin-url", required=True); p.add_argument("--bin-file")
    p = sub.add_parser("cf-scene"); p.add_argument("file")
    p = sub.add_parser("cf-live"); p.add_argument("file")
    p = sub.add_parser("cf-status")
    p = sub.add_parser("cf-reflex"); p.add_argument("file")
    p = sub.add_parser("cf-firmware"); p.add_argument("bin"); p.add_argument("--version", required=True)
    p = sub.add_parser("cf-telemetry")
    a = ap.parse_args()

    if a.cmd == "validate":
        scene = json.loads(Path(a.file).read_text())
        errs = validate(scene)
        print("OK" if not errs else "\n".join(errs)); sys.exit(0 if not errs else 1)
    if a.cmd == "stage":
        stage(json.loads(Path(a.file).read_text()))
    if a.cmd == "notify":
        stage({"name": "notify", "steps": [
            {"do": "notify", "title": a.title, "body": a.body, "beep": 2, "duration_ms": 6000},
            {"do": "face", "emotion": "normal"}]})
    if a.cmd == "say":
        steps = [{"do": "face", "emotion": "happy"}]
        step = {"do": "say", "text": a.text}
        if a.wav:
            dst = FEED / "audio" / Path(a.wav).name
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(a.wav, dst)
            step["audio_url"] = f"__FILL_RAW_BASE__/feed/audio/{dst.name}"
            print(f"wav配置: {dst} (audio_urlの __FILL_RAW_BASE__ をリポジトリのraw URLに置換してください)")
        steps.append(step); steps.append({"do": "wait", "ms": 1500}); steps.append({"do": "clear_text"})
        stage({"name": "say", "steps": steps})
    if a.cmd == "face":
        stage({"name": "face", "steps": [{"do": "face", "emotion": a.emotion}]})
    if a.cmd == "cf-scene":
        scene = json.loads(Path(a.file).read_text())
        scene["seq"] = int(scene.get("seq", 0)) + 1
        errs = validate(scene)
        if errs: print("検証エラー:", *errs, sep="\n  "); sys.exit(1)
        print(cf_call("/admin/scene", json.dumps(scene, ensure_ascii=False).encode()))
        print(f"配信完了: seq={scene['seq']} (端末が約10秒で実行)")
    if a.cmd == "cf-live":
        scene = json.loads(Path(a.file).read_text())
        errs = validate(scene)
        if errs: print("検証エラー:", *errs, sep="\n  "); sys.exit(1)
        print(cf_call("/admin/send-live", json.dumps(scene, ensure_ascii=False).encode()))
    if a.cmd == "cf-status":
        print(cf_call("/admin/status"))
    if a.cmd == "cf-reflex":
        print(cf_call("/admin/reflex", Path(a.file).read_bytes()))
        print("リフレックス設定を反映 (Workerが即時応答するルール)")
    if a.cmd == "cf-firmware":
        print(cf_call(f"/admin/firmware?version={a.version}", Path(a.bin).read_bytes(), "application/octet-stream"))
        # v1.2.5+ は WS の {"type":"ota"} で即時チェックする (旧版は無視するので無害)
        try:
            print("即時チェック送信:", cf_call("/admin/send-live", b'{"type":"ota"}'))
        except Exception as e:
            print("即時チェック送信に失敗 (ポーリングで拾います):", e)
        print("OTAマニフェスト更新済み (v1.2.5+ は即時、旧版はポーリングで自動更新)")
    if a.cmd == "cf-telemetry":
        print(cf_call("/admin/telemetry"))
    if a.cmd == "manifest":
        FEED.mkdir(exist_ok=True)
        m = {"version": a.version, "bin_url": a.bin_url}
        if a.bin_file:
            m["sha256"] = hashlib.sha256(Path(a.bin_file).read_bytes()).hexdigest()
            m["size"] = Path(a.bin_file).stat().st_size
        (FEED / "manifest.json").write_text(json.dumps(m, indent=2))
        print(f"manifest生成: version={a.version} -> feed/manifest.json (pushでOTA配信)")


def cf_base_key():
    base = os.environ.get("KINAKO_CF_BASE", "")
    key = os.environ.get("KINAKO_CF_ADMIN_KEY", "")
    if not base or not key:
        envf = ROOT / "cloudflare" / "keys.env"
        kv = dict(l.split("=", 1) for l in envf.read_text().splitlines() if "=" in l) if envf.exists() else {}
        key = key or kv.get("ADMIN_KEY", "")
        base = base or kv.get("CF_BASE", "")
    if not base:
        print("KINAKO_CF_BASE (Worker URL) が未設定。cloudflare/keys.env に CF_BASE=... を追加して"); sys.exit(1)
    return base.rstrip("/"), key


def cf_call(path, data=None, ctype="application/json"):
    base, key = cf_base_key()
    req = urllib.request.Request(base + path, data=data, method="POST" if data is not None else "GET")
    req.add_header("X-Admin-Key", key)
    # 既定の "Python-urllib/x.y" は Cloudflare のブラウザ整合性チェックで 403 (error 1010) になる
    req.add_header("User-Agent", "kinako-tools/1.0")
    if data is not None: req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode()


if __name__ == "__main__":
    main()
