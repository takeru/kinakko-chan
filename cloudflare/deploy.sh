#!/bin/sh
# KinakoCore2 バスを Cloudflare にデプロイする (ユーザーの Mac で1回だけ実行)
# 事前: Cloudflare アカウントがあること。wrangler login はブラウザで本人が承認する。
set -e
cd "$(dirname "$0")"
if [ ! -f keys.env ]; then echo "keys.env がない (keys.example.env をコピーして値を入れる)"; exit 1; fi
if [ ! -f wrangler.toml ]; then cp wrangler.toml.example wrangler.toml; echo "wrangler.toml を example から作成"; fi
set -a; . ./keys.env; set +a

echo "== wrangler ログイン確認 =="
npx -y wrangler whoami || npx -y wrangler login

echo "== KV 作成 (既にあればそのまま) =="
if grep -q REPLACE_WITH_KV_ID wrangler.toml; then
  OUT=$(npx -y wrangler kv namespace create KINAKO 2>&1 || true)
  ID=$(echo "$OUT" | grep -o 'id = "[a-f0-9]*"' | head -1 | sed 's/.*"\(.*\)"/\1/')
  # -i.bak は BSD(mac)/GNU sed 両対応のため。バックアップは残さない
  if [ -n "$ID" ]; then sed -i.bak "s/REPLACE_WITH_KV_ID/$ID/" wrangler.toml && rm -f wrangler.toml.bak; fi
  grep -q REPLACE_WITH_KV_ID wrangler.toml && { echo "KV id を取得できなかった。wrangler kv namespace list で確認して wrangler.toml に手で入れて"; exit 1; }
fi

echo "== デプロイ =="
# 初回は本体アップロード成功直後の確認APIが 10007 (Worker が存在しない) を返すことがある
# (作成の反映待ち)。デプロイは冪等なので1回だけやり直す
npx -y wrangler deploy || { echo "デプロイ失敗。数秒待って再試行"; sleep 5; npx -y wrangler deploy; }

echo "== シークレット登録 =="
printf %s "$DEVICE_KEY" | npx -y wrangler secret put DEVICE_KEY
printf %s "$ADMIN_KEY" | npx -y wrangler secret put ADMIN_KEY

echo ""
echo "完了。Worker の URL (https://kinako-core2-bus.<あなた>.workers.dev) を"
echo "Core2 の設定ポータルに、こう入れる:"
echo "  Feed URL     : https://kinako-core2-bus.<あなた>.workers.dev/scene.json"
echo "  Telemetry URL: https://kinako-core2-bus.<あなた>.workers.dev/telemetry"
echo "  Device Key   : keys.env の DEVICE_KEY の値"
echo "URL をきなこに教えてくれれば、初回のシーンとファーム(v1.1.0)を流し込む。"
