# Fold5 AirDrop移植プロジェクト

**2026-10-19 以降の方針は `ESP32-AIRDROP.md`（ESP32 を AWDL 無線機にする方式）。** 以下は Codex 時点の記録。

移動先: `C:\Users\1kkim\project\dev\andorid-airdorp`

- `src/`: Moseyブリッジ、Shim、Fold5向け診断・移植コード
- `tools/`、`tests/`: ビルド・一時配置・検証用ツール
- `module_mosey/`: 上流モジュールと端末保護ガード。Fold5へそのまま導入しないこと。
- `diagnostics/`: ビルド済み診断プログラムと実機検証記録
- `FOLD5-PORT.md`: 調査・修正・到達点の記録。末尾が最新。
- `SYSTEM-API-INVESTIGATION.md`: SamsungシステムAPI調査
- `work/fold5-analysis/`: 取得したドライバー、解析資料、関連OSS、root導入時の資料
- `.git/`: 元リポジトリの履歴と未コミットの変更を保持
- `MIGRATION-MANIFEST.json`: 移動した全ファイルのSHA-256照合記録

## 現在の到達点

SM-F946U1で、一時root、Moseyの一時システム配置、通常VINTF方式でのサービス登録・接続・interface_version=1取得を確認済み。専用monitorインターフェースの作成・3秒待機・削除も成功。ただし受信フレームは0件。

無線開始は `Could not find wiphy <wonder>` で停止する。**標準AirDropからの写真受信は未完成。** 実際の無線送信、MoseyからFold5ドライバーへの接続、AWDL同期などが残っている。

端末の通常再起動で一時root・mount・SELinux追加設定は解除される。端末側の一時配置スクリプトはソース移動によって変更していない。

## ビルド例（WSL、既存Android SDK/NDK）

プロジェクトルートで実行:

```bash
python3 tools/build-radio-probe.py \
  --ndk /mnt/c/Users/1kkim/AppData/Local/Android/Sdk/ndk/30.0.16248370 \
  --tool monitor-session --output diagnostics/fold5-monitor-session
```

指定可能な `--tool`: `radio-probe`、`monitor-session`、`binder-probe`、`mosey-adapter`、`mosey-start-probe`。

過去のログ中の旧作業フォルダーへの絶対パスは、当時の検証記録として保持している。
