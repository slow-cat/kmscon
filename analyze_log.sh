#!/bin/bash

if [ -z "$1" ]; then
    # 最新のログファイルを使用
    LOGFILE=$(ls -t /tmp/kmscon_logs/kmscon_*.log 2>/dev/null | head -1)
    if [ -z "$LOGFILE" ]; then
        echo "エラー: ログファイルが見つかりません"
        exit 1
    fi
else
    LOGFILE="$1"
fi

echo "=== kmscon ログ解析 ==="
echo "ファイル: $LOGFILE"
echo ""

echo "--- FBO関連 ---"
grep -E "(FBO|offscreen)" "$LOGFILE" | head -20

echo ""
echo "--- スクロール検出 ---"
grep "SCROLL" "$LOGFILE" | head -20

echo ""
echo "--- クリア操作 ---"
grep -E "(CLEARING|SKIP clear)" "$LOGFILE" | head -20

echo ""
echo "--- エラー/警告 ---"
grep -E "(error|warning|Error|Warning)" "$LOGFILE" | head -20

echo ""
echo "=== 統計 ==="
echo "CLEARING回数: $(grep -c 'CLEARING FBO' "$LOGFILE")"
echo "SKIP clear回数: $(grep -c 'SKIP clear' "$LOGFILE")"
echo "スクロール検出: $(grep -c 'SCROLL:' "$LOGFILE")"
echo "FBO作成: $(grep -c 'FBO.*enabled' "$LOGFILE")"
