#!/bin/bash

# ログディレクトリ作成
mkdir -p /tmp/kmscon_logs

# タイムスタンプ付きログファイル
LOGFILE="/tmp/kmscon_logs/kmscon_$(date +%Y%m%d_%H%M%S).log"

echo "=== kmscon Debug Log ===" > "$LOGFILE"
echo "Date: $(date)" >> "$LOGFILE"
echo "===================" >> "$LOGFILE"
echo "" >> "$LOGFILE"

# kmsconを実行してログをファイルに保存
# Note: gltexは自動で選択される（drm3dバックエンド使用時）
cd /tmp/kmscon
sudo builddir/kmscon --debug 2>&1 | tee -a "$LOGFILE"

echo ""
echo "ログファイル: $LOGFILE"
