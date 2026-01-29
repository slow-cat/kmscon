# kmscon デバッグログ記録方法

## 基本的な使い方

### 1. ログを記録しながら実行
```bash
cd /tmp/kmscon
./run_debug.sh
```
- ログは `/tmp/kmscon_logs/kmscon_YYYYMMDD_HHMMSS.log` に保存
- 画面にも同時表示（tee使用）

### 2. ログを解析
```bash
./analyze_log.sh
```
- 最新のログファイルを自動解析
- FBO、スクロール、クリア操作の統計を表示

特定のログを解析:
```bash
./analyze_log.sh /tmp/kmscon_logs/kmscon_20260129_162000.log
```

## 手動でログ記録

### 標準出力+エラーをファイルへ
```bash
sudo builddir/kmscon --debug --render-engine=gltex 2>&1 | tee /tmp/kmscon.log
```

### バックグラウンド実行
```bash
sudo builddir/kmscon --debug --render-engine=gltex > /tmp/kmscon.log 2>&1 &
```

### 特定の情報だけフィルタ
```bash
sudo builddir/kmscon --debug --render-engine=gltex 2>&1 | grep -E "(FBO|SCROLL)" | tee /tmp/kmscon_filtered.log
```

## 確認すべきログパターン

### ✅ 正常動作
```
FBO differential rendering enabled: 1920x1080
CLEARING FBO (need_clear=true)          # 初回のみ
SKIP clear (differential mode)          # 2回目以降
SCROLL: Detected scroll UP              # スクロール時
```

### ❌ 問題あり
```
FBO: prepare() called but no FBO exists!  # FBO作成失敗
CLEARING FBO (need_clear=true)           # 毎フレーム出る
glClearColor                             # uterm側で出る（削除済みのはず）
```

## ログファイルの場所
- `/tmp/kmscon_logs/` - スクリプト使用時
- 手動実行時は任意の場所に指定可能

## Tips
- `Ctrl+C`で終了
- ログが大きくなりすぎる場合: `head -1000`で制限
- リアルタイム監視: `tail -f /tmp/kmscon.log`
