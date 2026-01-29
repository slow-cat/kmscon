# FBO差分レンダリング実装完了サマリー

## 実装内容

### 1. FBO (Framebuffer Object) 差分レンダリング
- **ファイル**: `src/text_gltex.c`
- **変更行**: +280行, -3行

#### 主要機能
- セル状態追跡（ID + 属性比較）
- 変更セルのみ再描画
- FBO上に未変更セル保持
- 条件付きクリア（初回/背景色変更時のみ）

#### 構造体追加
```c
struct gltex_cell {
    uint64_t id;
    struct tsm_screen_attr attr;
};

struct gltex_row {
    uint64_t hash;  // スクロール検出用
};
```

#### フロー
1. `gltex_prepare()`: FBOバインド、条件付きクリア
2. `gltex_draw()`: セル比較、変更時のみ描画
3. `gltex_render()`: FBO→画面Blit

### 2. uterm無駄glClear削除
- **ファイル**: `src/uterm_drm3d_video.c`
- **変更**: 毎フレームのglClear削除（初期化時のみ保持）

### 3. スクロール最適化（基礎実装）
- スクロール検出ロジック追加
- 行移動パターン認識
- FBO内Blitの準備（TODO）

### 4. GLES2拡張対応
- `glBlitFramebufferNV`を動的ロード
- 拡張がない場合のフォールバック
- EGLヘッダー追加

## ビルド済みファイル
```
builddir/src/mod-gltex.so        # FBO差分レンダリング版
builddir/src/mod-drm3d.so        # glClear削除版
builddir/kmscon                  # メインバイナリ
```

## デバッグツール
- `run_debug.sh` - ログ記録実行
- `analyze_log.sh` - ログ解析
- `README_logging.md` - 使い方

## 実行方法
```bash
cd /tmp/kmscon
./run_debug.sh
```

## 検証項目

### ✅ 正常動作の確認
```bash
# ログから確認
grep -E "(glBlitFramebuffer|FBO|CLEARING|SKIP)" /tmp/kmscon_logs/kmscon_*.log

# 期待される出力
glBlitFramebufferNV extension loaded
FBO differential rendering enabled: 1920x1080
CLEARING FBO (need_clear=true)      # 初回のみ
SKIP clear (differential mode)      # 以降連続
SCROLL: Detected scroll UP          # スクロール時
```

### ❌ 問題のパターン
```bash
# FBO使えない
FBO disabled: glBlitFramebufferNV not available

# 毎フレームクリア（バグ）
CLEARING FBO (need_clear=true)
CLEARING FBO (need_clear=true)
CLEARING FBO (need_clear=true)
...

# 背景色が毎回変わる（バグ）
bg color changed from (0.00,0.00,0.00) to (0.00,0.00,0.00)
```

## 性能期待値
- **通常操作**: 変更セルのみ描画（1-10セル）
- **スクロール**: 1行分のみ描画（将来: FBO内Blit）
- **全画面更新**: 初回/背景色変更時のみ

## 制限事項
- GLES2 + `GL_NV_framebuffer_blit`拡張が必要
- 拡張がない場合はbbulkバックエンドにフォールバック
- スクロール最適化は検出のみ（Blit未実装）

## 次のステップ（オプション）
1. スクロール時のFBO内Blit実装
2. 行ハッシュによる高速スクロール検出
3. 複数行スクロール対応
4. ベンチマーク測定

## トラブルシューティング

### undefined symbol: glBlitFramebufferNV
→ 修正済み（動的ロード）

### FBOが作成されない
→ ログで"glBlitFramebufferNV extension loaded"を確認

### 画面が消える
→ need_clearフラグとbg_r/g/b保存を確認

### ビルドエラー
→ EGL/egl.h インクルード確認

