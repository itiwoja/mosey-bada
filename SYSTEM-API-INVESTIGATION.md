# Fold5：開発者向けオプション・Samsung API経由の追加調査

対象：実機SM-F946U1、F946U1UES8GZI1、Android 16 / One UI 8.5。
調査日：2026-10-19。目的は相手のiPhoneが標準AirDropから写真を送り、Fold5単体で受信すること。

## 結論

ADBから使えるWi-Fi管理機能は存在し、実機で呼び出せた。低レベルソケットの拒否だけを理由に、すべてのシステムAPI経路を否定するのは広すぎる説明だった。

ただし、調査した実機の既存部品・公開メソッド・Quick Shareの判定処理からは、開発者向けオプションだけで標準AirDrop受信を有効にする経路は確認できなかった。ハードウェアそのものが対応不可能という結論ではない。

## 実機で確認できたこと

読み取り専用の `src/fold5/FrameworkProbe.java` を作成し、ADBのapp_processから実行した。対象パッケージはcom.android.shell、実行UIDは2000。

| 項目 | 実測 |
|---|---|
| MANAGE_WIFI_INTERFACES | granted |
| NETWORK_SETTINGS | granted |
| NEARBY_WIFI_DEVICES | granted |
| CONNECTIVITY_USE_RESTRICTED_NETWORKS | granted |
| NETWORK_FACTORY | denied |
| CREATE_APP_SPECIFIC_NETWORK | denied |
| WifiAwareManager.isAvailable | true |
| Wi-Fi Aware instant communication対応 | false |
| Wi-Fi Aware pairing対応 | false |
| Samsung isVendorNanServiceAvailable | false |
| Samsung isAirdropInterfaceActive | false |

普通のWi-Fi Awareが使えることと、AppleのAWDL互換処理が使えることは別である。

`cmd wifiaware help` と `cmd sem_wifi_aware help` はUID 2000では拒否されたが、これはshellコマンド入口の制限であり、Wi-Fi Aware管理APIすべての拒否を意味しない。実際、上記の直接API呼出しは成功した。

ShizukuをADB方式で利用しても実行UIDは同じshellであり、ここで拒否されている権限まで自動的に増える根拠はない。Shizuku自体はインストールしていない。

## Quick Shareのコードを確認した結果

端末にインストール済みのQuick ShareをPCへ読み出して解析した。アプリの変更や再インストールはしていない。

- パッケージ：com.samsung.android.app.sharelive
- バージョン：13.8.53.20 / versionCode 1385320205
- DEX上の `Lpf/f;->e()Z` にMosey対応判定がある。
- その判定は、通常のNearby共有の利用条件に加え、Moseyパッケージの署名、システムアプリ属性、共有サービスの存在、Google Play開発者サービスのバージョン等を確認する。
- `Lpf/f;->c()Z` は `com.google.android.mosey` の署名証明書SHA-256を既定の値と比較する。
- `Lpf/f;->h()Z` は `ApplicationInfo.FLAG_SYSTEM` または `FLAG_UPDATED_SYSTEM_APP` を確認する。
- `com.google.android.mosey` は `pm list packages -u` にも現れず、確認したMoseyAppの標準ディレクトリにも存在しなかった。全ストレージの不在を証明するものではない。
- このため、単に同名の自作アプリを入れる、あるいは通常のユーザーアプリとしてMoseyを追加するだけでは、このQuick Shareの対応判定を満たせない。

Samsungの `com.samsung.android.aware.service` はインストール済みだが、調べたDEX文字列にAirDrop/AWDL/Mosey参照はなかった。この文字列検索だけで全機能の不存在までは断定しない。

## 隠しAirDrop APIは何をしていたか

`com.samsung.android.wifi.SemWifiManager.isAirdropInterfaceActive()` が存在し、ADB実行コンテキストで呼び出し可能だった。

端末の `/system/framework/semwifi-service.jar` をPCへ読み出して解析した結果、呼出しは以下につながる。

- SemWifiServiceImpl.isAirdropInterfaceActive
- SemWifiApServiceImpl.isAirdropInterfaceActive
- NetworkInterface.getNetworkInterfacesを列挙
- 名前が `mosey0` で、isUpがtrueのものがあるか確認

このメソッド自身には、mosey0を作成する処理やAWDL無線を開始する処理はない。ホットスポット等との競合を避けるための状態確認に使われていた。

Samsungの `setNanCommand(int, byte[])` も見つかったが、Vendor NANサービスに転送する処理であり、今回のサービス利用可能判定はfalseだった。未確認のコマンド送信や設定変更は実行していない。この入口があるだけでAWDL通信できるとは言えない。

## 作成した診断と記録

- `src/fold5/FrameworkProbe.java`：読み取り専用のAPI診断コード。
- `fold5-framework-probe.txt`：実機出力。
- `diagnostics/framework-probe.dex`：今回実行したAndroid用診断コード。AirDrop受信機ではない。
- `diagnostics/system-api-evidence.json`：解析対象のバージョン・ハッシュと要点。

診断ファイルを一時配置した以外、設定変更、受信モード開始、Wi-Fi切断、root取得、APKインストール、システム書換えは行っていない。一時配置ファイルは診断終了後に削除した。

## 次に必要な証拠

Fold5で使えるAWDL通信バックエンド、またはその代わりを提供する実際のシステムサービスが必要。対応端末の表示用設定だけをコピーしても受信完成の証拠にはならない。現在の調査では、この条件を満たすroot不要の経路は未発見。

## Moseyをシステムアプリとして追加する方法の調査

### 条件付きで実現できる方法

rootと起動時マウント機構を確保した端末では、MagiskまたはKernelSUのsystemlessモジュールで、正規署名のMosey APKをシステム領域に配置した状態を作り、PackageManagerにシステム/特権アプリとして読み込ませる方法がある。候補の論理配置先は `/system_ext/priv-app/MoseyApp/MoseyApp.apk`、特権権限XMLは同じsystem_extの `/etc/permissions/`。実際のモジュール配置規則は選択するマウント機構に合わせる必要がある。

- 正規APKの署名を保持する。自作署名への置き換えではQuick Shareの既定署名チェックを満たせない。
- privileged配置と許可リストは、signature-only等の権限を無条件に与える仕組みではない。各権限の実機定義に従って許可・拒否を確認する必要がある。
- 既存mosey-badaのXMLをFold5へ無検証で流用しない。
- 再読込後にSYSTEM/PRIVILEGED属性、要求権限、SHARING_PROVIDERサービス解決を検証する。システムアプリとして認識されることと、AirDropの無線通信・写真受信が完成することは別。

### 今回の端末で確認した制約

- ro.build.type=user、ro.debuggable=0、ro.secure=1。
- ブートローダーはlocked、ビルドはF946U1UES8GZI1。
- `/system_ext/priv-app` と `/system_ext/etc/permissions` はroot所有で、shellから書込み不可。
- ro.control_privapp_permissions=enforce。
- MoseyはPackageManagerのアンインストール済みを含む一覧にも不在。このため「既存システムアプリをユーザーに復元するinstall-existing」で復活できる証拠はない。

### ADBの --preload を調べた結果

AOSP android16-releaseのPackageManagerShellCommandは `--preload` を `setInstallAsVirtualPreload()` に渡し、PackageInstaller側ではINSTALL_VIRTUAL_PRELOADを設定する。ScanPackageUtilsはこれをシステム判定とは別に扱い、system属性はSCAN_AS_SYSTEM等から設定する。従って「--preloadで通常APKをシステムアプリへ昇格できる」という手順の根拠にはならない。端末への試験インストールは行っていない。

### root取得手段の対応照合

2026-10-19に公開カタログを再取得して照合した。本家BuSung-devにはF946の一致エントリなし。HyperRamzey派生版にはSM-F946B / F946BXXS7GZE5・F946BXXS7GZH2があるが、SM-F946U1 / F946U1UES8GZI1の一致エントリはなかった。対応カタログにないことはあらゆるroot手法の不存在を証明しないが、この端末で検証済みの手順として案内・実行できる根拠は得られていない。

### 出典

- https://source.android.com/docs/core/permissions/perms-allowlist
- https://topjohnwu.github.io/Magisk/install.html
- https://kernelsu.org/guide/module.html
- https://github.com/aosp-mirror/platform_frameworks_base/blob/android16-release/services/core/java/com/android/server/pm/PackageManagerShellCommand.java
- https://github.com/aosp-mirror/platform_frameworks_base/blob/android16-release/core/java/android/content/pm/PackageInstaller.java
- https://github.com/aosp-mirror/platform_frameworks_base/blob/android16-release/services/core/java/com/android/server/pm/ScanPackageUtils.java
- https://github.com/BuSung-dev/Root-My-Galaxy-Payloads/blob/main/support/targets-v3.json
- https://github.com/HyperRamzey/Root-My-Galaxy-Payloads/blob/main/support/targets-v3.json

この調査ではシステムアプリの追加やroot取得、再起動、端末への書込み試験は実行していない。

## root取得方法の調査拡大：DFRoot / DirtyFrag

前の照合はRoot-My-Galaxyの型番別カタログに偏っていた。別方式の候補を確認したため追記する。「検証済みの方法が見つからない」と「試験候補がない」は別である。

### 候補

- DFRoot v4.5（diabl0w、2026-10-15公開）
  https://github.com/diabl0w/DFRoot/releases/tag/v4.5
- Samsung向け調整を含む派生版DirtyFrag v1.16（2026-10-15公開）
  https://github.com/mitschud/DirtyFrag/releases/tag/v1.16
- 開発元がSamsung向けに指定する管理アプリ：diabl0w/KernelSU samsung-v2.2（2026-10-15公開）
  https://github.com/diabl0w/KernelSU/releases/tag/samsung-v2.2

DFRoot/DirtyFragはCVE-2026-43284を利用する一時root方式で、ブートローダー解除を前提としない。型番別のGhostLockプロファイル方式と異なるため、Root-My-GalaxyカタログにF946U1がないことだけでは除外できない。通常のMagisk導入と同一の手法ではない。

### Fold5との照合

実機カーネルは5.15.189-android13-8、アーキテクチャARM64。DirtyFragには `dfroot-android13-5.15.ko`、Samsung KernelSUの公開リリースには `lkm-aarch64-android13-5.15_kernelsu.ko` がある。OSがAndroid 16でもカーネル系統はandroid13-5.15であり、OSの数字だけでandroid16-6.12向けを選ぶ根拠はない。KMI系統の一致はビルド全体の互換性の証明ではない。

追加の読み取り診断で、XFRM、XFRM_USER、INET_ESP、INET6_ESP、CRYPTO_CBC、CRYPTO_AES、MODULES、MODVERSIONS、KALLSYMS、KPROBESがyであることを確認した。この構成確認は、脆弱性が未修正であることもroot取得成功も証明しない。

GitHubの公開Issue検索では、この型番・F946U1UES8GZI1での成功報告は確認できなかった。他機種・別更新の成功報告をこのビルドの検証結果として扱わない。2026-09-05というパッチ日付だけでは、対象CVEの修正有無を確定できない。

### 開発元の導入フロー（未実行）

1. DirtyFragまたはDFRootの公式リリースAPKを選ぶ。両者を同時実行しない。
2. 開発元指定のSamsung向けKernelSU管理アプリを導入する。
3. DirtyFragでその管理アプリを選び、root取得処理を1回実行する。初回は起動時自動実行や自動再起動を有効にしない。
4. 成功表示だけで判断せず、許可後のsu経由でuid=0を実測する。
5. root取得、モジュール用マウント機構、Moseyのシステムアプリ化、AWDL通信はそれぞれ独立して検証する。

この方式はカーネル内の状態やSELinux等を変更する実験的手法で、失敗によるフリーズ・再起動があり得る。再起動でrootは消える設計だが、アプリやモジュールがディスクへ書いた変更まで必ず消えるという意味ではない。Knoxやデータへの影響が絶対にないとも保証しない。

結論：Fold5で試験候補になる方式は見つかった。ただし、この個体・ビルドでroot取得できるかは未検証。今回行ったのは公開資料と部品一覧の照合および読み取り診断のみで、root APKのインストール・実行はしていない。

## 実機root取得に成功（2026-10-19）

ユーザーの明示指示により、公式DirtyFrag v1.16とSamsung向けKernelSU samsung-v2.2を導入して実行した。ダウンロードした両APKはGitHub公開SHA-256と一致し、APK署名検証も成功した。

1回目は「KernelSU ModulesをDisabledにする」設定でbootstrapのdisable_modules処理が失敗し、KernelSU開始前で停止した。コードではmodulesディレクトリをopendirできない場合に失敗する。ENOENT等の正確なerrnoはログにないため原因を断定しない。再起動してSELinux=Enforcingへ戻ったことを確認した。

2回目はModules設定を標準のEnabledへ戻し、起動時自動実行と自動再起動はオフのまま実行。KernelSU管理画面が「動作中[Jailbreak mode]」となり、com.android.shellにSuperuser許可を設定したうえで、管理アプリ同梱libksud.soのdebug suから次を実測した。

```
uid=0(root) gid=0(root) groups=0(root) context=u:r:ksu:s0
Enforcing
```

この型番・ビルドのroot取得が未検証という従前の記録は、ここで実機検証済みに更新する。rootは一時的で、起動時自動実行は無効。再起動時には今回のrootは消える。アプリと設定の導入はディスク上に残る。

追加の読み取り専用診断では、rootコンテキストでAF_PACKETソケットを作成でき、nl80211 GET_WIPHY照会が完了した。phy0はmonitorインターフェース種別を広告している。実際のmonitor起動、無線フレーム送信、AWDL同期、AirDrop写真受信は未検証。この試験ではMoseyや追加KernelSUモジュールは導入していない。

証拠: diagnostics/root-verification.txt、diagnostics/dirtyfrag-attempt-result.json、diagnostics/dirtyfrag-attempt-log.txt、diagnostics/dirtyfrag-attempt-2-log.txt、diagnostics/fold5-radio-probe-root.txt。
