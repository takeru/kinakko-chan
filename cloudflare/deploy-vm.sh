#!/bin/sh
# KinakoCore2 バスを きなこの VM からデプロイする (APIトークン方式・非対話)
# 事前: keys.env に CLOUDFLARE_API_TOKEN と CLOUDFLARE_ACCOUNT_ID があり、
#       wrangler.toml に KV の id が入っていること (初回だけ Mac の deploy.sh
#       相当の準備が済んでいればよい)。Mac での手動デプロイは deploy.sh。
set -e
cd "$(dirname "$0")"
if [ ! -f keys.env ]; then echo "keys.env がない"; exit 1; fi
if grep -q REPLACE_WITH_KV_ID wrangler.toml; then echo "wrangler.toml の KV id が未設定"; exit 1; fi
set -a; . ./keys.env; set +a
export CLOUDFLARE_API_TOKEN CLOUDFLARE_ACCOUNT_ID

echo "== デプロイ (VM) =="
# DO のコード更新は即時モード: 既定の deferred (最大300秒) だと稼働中の
# ハブが旧コードを回し続け、新エンドポイントが 404 になる時間帯ができる
# (2026-11-21 の /admin/ws で実測)。即時リセットしても冬眠APIの接続は
# 維持され、状態は DO ストレージにあるので実害はない。
# 初回は確認APIが 10007 を返すことがある (作成の反映待ち)。冪等なので1回再試行
npx -y wrangler deploy --durable-objects-code-update-mode immediate || { echo "デプロイ失敗。数秒待って再試行"; sleep 5; npx -y wrangler deploy --durable-objects-code-update-mode immediate; }

echo "== シークレット登録 (冪等: keys.env と同期) =="
printf %s "$DEVICE_KEY" | npx -y wrangler secret put DEVICE_KEY
printf %s "$ADMIN_KEY" | npx -y wrangler secret put ADMIN_KEY

echo "完了 (VM からデプロイ)。"
