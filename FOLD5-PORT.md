# Fold5 移植作業（未対応・診断段階）

対象: SM-F946U1 / One UI 8.5 / Android 16（ユーザー申告）。
目的: iPhone の写真アプリから標準 AirDrop で送信し、Fold5 が写真を受信する。
上流: https://github.com/FeelLiao/mosey-bada
取得コミット: 718f321d5ec9aca81567591ddb7b7467f47462c8

## 現時点の変更

- `tools/fold5-probe.sh`: rootを要求せず、型番・起動ロック・カーネル・通信ドライバー候補を読み取る。端末設定は変更しない。シリアル番号・連絡先・写真は収集しない。
- `module_mosey/customize.sh`: Fold5ではインストール開始前に停止し、上流のOnePlus用設定が適用されることを防ぐ。これは移植完成を意味しない。

## 実機診断

PCにUSB接続し、開発者向けオプションのUSBデバッグを有効にしてPCを許可する。
リポジトリのフォルダーで実行する（adbのある環境）。

```sh
adb devices -l
adb shell sh < tools/fold5-probe.sh > fold5-probe.txt
```

端末が複数ある場合は `adb -s 対象端末` を指定する。
アクセス拒否・項目欠落は「不明」と扱う。`su`の存在だけではroot化済みと判定しない。

## コードから確認した未解決事項

1. 通信本体 `mosey_server` と `libmosey_daemon_ffi.so` のソースは含まれない。READMEは同梱なしと記載しているが、取得コミットには両方のARM64 ELFバイナリが存在する（実ファイル確認で訂正）。Fold5との互換性は未確認。
2. 上流のインストーラーと検証には `peach/bdwlan.*` や `WCNSS_qcom_cfg.ini` 等の端末固有パスがある。Samsungの設定であるとは確認できないため流用しない。
3. `wonder` 通信ドライバーとFold5のカーネル・Wi-Fiハードウェアの互換性は未確認。同じQualcommというだけでは互換性を判断できない。
4. 特権アプリ、rootデーモン、SELinux設定、システムへのマウントに依存する。通常APKの権限だけでは上流方式を移植できない。
5. U1端末のブートローダー解除・root取得可否は実機情報と信頼できる機種別資料で追加確認が必要。SIMロック解除とブートローダー解除は別。
6. 上流ビルドはmacOS用NDKパスを固定している。通信バックエンドと依存部品の方針決定後にビルド環境対応を行う。

## 完成の判定

iPhoneに追加アプリ・Web操作を要求せず、写真→共有→AirDropでFold5が見つかり、選択後にFold5で承認して写真が保存されること。まず単一写真、次に複数写真と動画を実機確認する。現在は未達成。

## 2026-10-19 実機診断結果

USBデバッグ承認後、読み取り専用の診断を実行した。インストール・root取得・Wi-Fi設定変更は行っていない。

- 実機: SM-F946U1 / q5q / Android 16 / One UI 80500。
- ビルド: F946U1UES8GZI1、セキュリティパッチ2026-09-05。
- カーネル: 5.15.189-android13-8-33404244-abF946U1UES8GZI1。
- `ro.boot.flash.locked=1`、`vbmeta.device_state=locked`、Verified Bootはgreen。
- ADBはuid=2000のshell、SELinuxはEnforcing。PATH上にsuなし（これだけで端末のあらゆるroot手段を否定はできない）。
- ロード済みWi-Fiモジュールはqca6490。実体候補はqca_cld3_qca6490.ko。wonderはロード済み一覧にない。
- Wi-Fi Awareの機能宣言は存在するが、AWDL対応の証拠にはならない。
- MODULES=y、MODVERSIONS=y、MODULE_SIG=y、TUN=y。MODULE_SIG_FORCEはこの照会では出力なし。署名強制の有無は未確定。
- 検査した公開サービス・パッケージ名・標準ライブラリディレクトリではmosey/wonderなし。端末全領域の不在を証明するものではない。

判定: 上流方式に必要な特権環境と通信バックエンドが現在の端末で利用できる証拠はない。上流モジュールの単純移植・通常APK化では完成条件を達成できない。次の開発条件は、SM-F946U1のこのビルドで特権環境を確保する正当な方法、およびqca6490向けAWDLバックエンドを特定すること。端末名・UIの変更では解決しない。

生の診断記録: fold5-probe.txt、fold5-probe-extra.txt。

## 追加調査と訂正

READMEに反して取得コミットにはmosey_server（約981 KiB）とlibmosey_daemon_ffi.so（約2.5 MiB）がある。実体がないとの初回説明は誤り。ソース未同梱と実行互換性の課題は残る。

関連実装 https://github.com/thelok1s/mosey-extended のREADMEも確認した。仮想wonderインターフェース方式は実際の無線フレーム送受信が未実装と明記されており、そのままFold5の通信層の代わりにはならない。

build.shのホスト固定をLinux/macOS判定に変更し、SDKとbuild-toolsを環境指定可能にした。外部ランタイム欠落時はビルド前に停止する。構文検査済み、APKビルド・実機受信テストは未実施。

## 追加実装・実機テスト（継続調査）

### ネイティブ無線診断

`src/fold5/radio_probe.c` を追加。ARM64 Android 16向けにNDK 30でビルドし、USB接続したSM-F946U1で実行した。

- `/proc/self/status`: `CapEff=0000000000000000`。
- `socket(AF_PACKET, SOCK_RAW, 0)`: errno 13 / Permission denied。
- `socket(AF_NETLINK, ..., NETLINK_GENERIC)`: errno 13 / Permission denied。
- したがってこの実行コンテキストでは無線機能の列挙にも到達できない。MONITORやAWDLがハードウェア非対応だと断定する結果ではなく、アクセス権限が阻害要因である。
- 無線送信、スキャン、インターフェース変更、Wi-Fi停止は行わない。診断用実行ファイルを `/data/local/tmp` に一時配置して実行後に削除した。
- 詳細: `fold5-radio-probe.txt`。追加sysfs照会結果: `fold5-radio-sysfs.txt`。ドライバーファイルの直接取得もSELinux等で拒否された。

再ビルド（既存NDKを使い、外部ダウンロードしない）:

```sh
python3 tools/build-radio-probe.py --ndk /path/to/android/ndk --output diagnostics/fold5-radio-probe
```

Linux/macOS/Windows、WSLからWindows版NDKを使う構成に対応。今回実測した構成はWSL＋Windows NDKのみ。

`tests/radio_probe_parser_test.c` で、切断・不正長・ランダムなnetlink属性4,080件をAddressSanitizer/UndefinedBehaviorSanitizer付きで検査し、通過した。

### 上流の完成度の再確認

mosey-bada作者の報告では、OnePlus 15でAirDrop送信先としての検出はできるが、incoming file-transfer flowはデバッグ中で、完全互換を主張していない。
出典: https://github.com/thelok1s/mosey-extended/issues/4

コードの `MoseyShimService.handleHttps` では `/Ask` と `/Upload` を別のBadaアプリへ転送している。転送先アプリを含めた受信完了テストも必要。mosey-badaだけをビルドしても写真保存機能の完成は保証できない。

### 無線ドライバー移植の条件

SM8550公開ソースの `configs/qca6490_defconfig` は共通defconfigを読み込む。monitor RXやchannel設定のコードは存在するが、実機のF946U1UES8GZI1と同一ソース・設定であるとは確認できていない。
参照: https://github.com/GoRhanHee/android_kernel_samsung_sm8550/tree/universal/vendor/qcom/opensource/wlan/qcacld-3.0

別機種WCN7851のQCACLD実験では、monitorのTXコードを通ってもfirmwareが実際に無線送信しなかったと報告されている。この結果をFold5に直接当てはめることはできないが、monitor設定だけでAWDLが実現する根拠にはできない。
参照: https://github.com/spiral009/aviumui-wlan-tools

必要な実装は次の順序:

1. この型番・ファームウェアで、NET_ADMIN/NET_RAWと無線制御を許可された実行環境を確保する。
2. 同一ビルドのドライバー/カーネルソースと設定を取得し、正規のモジュール読込条件を満たす。
3. qca6490向けに実際の802.11送受信とチャネル同期を実装・測定する。架空のwonderインターフェースだけで成功扱いにしない。
4. そのバックエンドにmoseyの要求を接続し、BLE→AWDL→IPv6/mDNS→TLSの経路を実機確認する。
5. Bada側の受信処理または代替実装で、承認→写真保存まで確認する。

### 権限確保の調査

Root-My-Galaxyの公開サポートカタログも読み取りのみで確認した。BuSung-dev本家のtargets-v3にはFold5エントリがなく、HyperRamzey派生版にはSM-F946BのGZE5/GZH2があるが、今回のSM-F946U1 / F946U1UES8GZI1の一致エントリはなかった。カーネル番号だけ一致する他型番のペイロードは実行していない。

- https://github.com/BuSung-dev/Root-My-Galaxy-Payloads/blob/main/support/targets-v3.json
- https://github.com/HyperRamzey/Root-My-Galaxy-Payloads/blob/main/support/targets-v3.json
- 米国Fold5の別ビルドの移植相談: https://github.com/BuSung-dev/Root-My-Galaxy/issues/571

### 現在の到達点

診断コードの実装・ビルド・実機実行は完了。AirDrop受信の移植は未完了。現端末のアクセス権限と、動作実績のあるqca6490用AWDLバックエンドが未解決であり、モデル名変更やAPK化だけでは進められない。端末のroot化、SELinux変更、システム書換え、再起動は実行していない。

## システムAPI経由の追加調査

SYSTEM-API-INVESTIGATION.mdを参照。ADBからWi-Fi管理APIは一部呼び出し可能と実証した。低レベルソケット拒否だけで全API経路を否定はできない。ただし標準AirDropを有効化できる経路は今回の追加調査でも未発見。

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

## root取得後の継続移植（2026-10-19）

実機の `/vendor/lib/modules/qca_cld3_qca6490.ko` をrootで読み出した。ELFのvermagicは実行カーネルと一致。文字列・シンボルにmonitor受信処理はあるがwonder/mosey/awdlは見つからなかった。文字列不在だけでハードウェア非対応とは断定しない。

GET_WIPHYを改良してベンダーコマンド総数も計測。照会完了時に98件、そのうちGoogle OUI 0x1a11は0件。通常のmonitor広告だけで既存mosey_serverのWonder制御APIを満たす証拠はない。実送信・ACK・AWDL同期は未試験。

関連OSS thelok1s/mosey-extended のコミット7656696ce2ee5929bcb84cb7e38e90f560c6162bからMosey v16753のbaseと3 splitを取得。全APKでv3署名とSourceStamp検証成功。署名証明書SHA256はQuick Share実機コードの許可リストに一致する82f90326951d14e08010b2a26cce329be2b09e74752158b7640e04a544676878。adb install-multipleはSuccess。通常アプリとしての初回登録を確認した。

続いてtools/fold5-mosey-runtime.shで、initのmount namespace内に一時tmpfsと既存アプリのbind mountを作成。既存productシステムファイルを書き換えずMoseyと専用権限XMLを追加した。contextオプション付きtmpfs mountはPermission deniedだったため、通常tmpfsを作成して追加ファイルにsystem_fileラベルを設定する方式で成功。SELinuxの無効化・ポリシー追加はしていない。通常の端末再起動でこの配置は消える。

Android framework再起動を実行し、システムアプリ認識・権限付与を確認中。この時点で写真受信は未達。OnePlus向けfirmwareや無関係なwonder.koは導入していない。

Google公式要件: https://support.google.com/product-documentation/answer/17303423?hl=en

### 今回の実機確認結果

framework再起動後、USB再接続を確認。カーネル再起動は起きておらずuptimeとroot保持を確認した。PackageManagerはMoseyを SYSTEM / UPDATED_SYSTEM_APP / PRIVILEGED / PRODUCT と認識。CREATE_APP_SPECIFIC_NETWORK、MANAGE_WIFI_INTERFACES、BLUETOOTH_PRIVILEGED等はgranted=true。

NETWORK_FACTORYだけは実機の定義がsignature|roleで、privapp許可XMLでは付与されないことを確認。既存のシステムロールを別アプリから奪う変更はしていない。

Quick Shareの実機ログ:
- isTrustedMoseyInstall=true
- FLAG_SYSTEM=true, FLAG_UPDATED_SYSTEM_APP=true
- isProtocolXAvailable=true
- protocolXSupported=true

UIにも「Appleデバイスと共有」の案内が表示された。これは機能判定の通過を示すだけで、無線送受信の成功ではない。受信タブを開き、画面のON操作からBluetoothを有効にした。MoseyRepositoryのavailabilityはfalse、ProtocolXInUse=false。nativeサービスやwonder/mosey無線インターフェースは見つからず、iPhoneからの受信テストへ進める状態ではない。

src/fold5/mosey_path_redirect.cを追加し、upstreamの/odm固定dlopenパスだけを専用一時ディレクトリへ転送する小さなライブラリをビルドした。Binder/無線制御の成功判定は改変しない。upstream同梱mosey_serverをroot・最大6秒で起動検証したところ、サービス登録に失敗してSIGABRT（exit134）で終了。エラーはFailed to register service: UNKNOWN_ERROR。SELinux、VINTF、service_contexts等のどれが直接原因かはこの出力だけでは確定できない。通信本体の互換性が成立していないため、実無線送受信は未検証のまま。

今回残したもの:
- Google署名Mosey v16753の通常インストールと一時システムアプリ配置
- 専用権限XMLと実機検証ログ
- 再適用・解除用のruntimeスクリプト（rootかつinitのmount namespace内で実行）

通常の端末再起動で一時rootとシステム配置は解除される。APK自体と付与済み通常権限は残る。次に必要なのは、正しいnativeサービス登録と、qca6490実無線に接続されたMosey互換バックエンドの実装・測定。仮想wonderだけやモニター広告だけでは完成条件を満たさない。**標準AirDropからの写真受信は未完成。**

## サービス登録とFold5無線処理の追加実装（2026-10-19 継続）

### 実測済み: 本物のサービス登録に成功

mosey_path_redirect.cに対象サービスだけの登録アダプターを実装。AOSPが説明するAIBinder_forceDowngradeToSystemStabilityを登録前に呼ぶ診断で、実機ログは次の通り。

```
Using local system stability for com.google.android.moseyservice.IMoseyService/default
addService(com.google.android.moseyservice.IMoseyService/default) result=0
```

8秒の検証期間はサービス登録のpanicで落ちず、timeoutの終了コード124で終了した。上流mosey_preloadのように失敗時にSTATUS_OKを返してはいない。必ず本物のServiceManagerの結果を返す。

出典: https://source.android.com/docs/core/architecture/aidl/aidl-hals

### アプリからの接続にはVINTF宣言が必要

Mosey APKのDEXでServiceManager.waitForDeclaredServiceを使用していると確認。登録が成功しても未宣言だとアプリから接続できない。このため診断用のlocal stabilityは明示環境変数FOLD5_MOSEY_LOCAL_STABILITY=1の時だけ有効に変更し、通常動作は元のVINTF方式を維持。

tools/fold5-mosey-service-runtime.shを追加し、実機で次を一時適用した。
- 元の全manifest fragmentを保持した/vendor/etc/vintf/manifestの一時bind mountと、対象HAL version1の宣言追加
- 対象サービス一つに対するservice_contextsエントリー
- 新しいサービス型を作成し、ksuにadd/find、priv_appとshellにfindだけを許可

通常の再起動でmountとカーネル上のSELinux追加は消える。SELinuxはEnforcingのまま。通常のrollbackはmountだけを外し、SELinux追加の完全解除は通常再起動が必要。初回適用時のksudはtypeにデフォルトdomain属性を付けたため、保存スクリプトではservice_manager_typeを型定義時に明示するよう修正した。未検証のpolicyをディスクの正規sepolicyへ書き込んではいない。

宣言のキャッシュ再読込のためservicemanagerを一度restartした。その後USB接続が戻っておらず、アプリからの宣言確認、通常VINTF方式の再登録と接続試験は未完了。端末画面の状態をユーザーに確認中。失敗や成功を推測せず、実機再接続後に続きを検証する必要がある。

### 実機ドライバーの送信口欠落をELFで確認

tools/inspect-fold5-driver.pyを実装。取得したqca6490 ELFのシンボルと再配置テーブルを解析し、同じサイズのstation/monitor net_device_opsを比較した。driver SHA256は78cee9e94c8c8804a052e7d86b62e78491f3166607847fc8fa2bf2e644ecf7eb。

stationのhdd_hard_start_xmit.cfi_jtに相当するoffset0x20は、monitor側でNULLかつ再配置エントリーなし。monitor側の有効コールバックはopen、stop、get_statsだけ。これは「標準monitorのパケット送信経路がない」という実機固有の証拠であり、他の管理フレーム経路やfirmware側の可能性までは否定しない。

同系公開ソースwlan_hdd_main.cのwlan_mon_drv_opsも同じ構造。関連WMIヘッダーにはHOST_BUFFER型のframe injection定義がある一方、公開ホスト側wmi_host_injector_frame_paramsには任意フレームのbuffer引数がなく、send_injector_frame_config_cmd_tlvも固定長コマンドしか送らない。関数名にinjectがあるだけでAWDLデータ送信可能とは判定できない。

### 受信処理を実装・ビルド、実機試験待ち

src/fold5/monitor_session.cを追加。明示引数--capture-three-secondsの時だけphy0上に専用fold5monを新規作成し、最大3秒のraw受信を試み、作成したインターフェースを削除する。既存同名インターフェースがあれば拒否。wlan0の状態、周波数、規制地域、firmwareは変更しない。802.11送信なし。受信内容は保存せず、フレーム総数とApple AWDL action headerの件数のみ表示。

Android16 ARM64向けNDKビルドに成功。tests/monitor_frame_test.cでradiotap/802.11ヘッダーの切断、protected frame、HT-control付きaction、乱数の境界試験をASan/UBSan付きで実行して成功。実機USB切断中のため、インターフェース作成可否・受信・後片付けはまだ未試験。これは完全なMosey通信バックエンドではなく、受信経路を実機検証するための実装。TX、ACK、チャネル同期、IPv6のAWDLデータ経路は未実装。**AirDrop写真受信は未達成。**

ビルド:
```
python3 tools/build-radio-probe.py --ndk /path/to/ndk --tool monitor-session --output diagnostics/fold5-monitor-session
```

## 再接続後の実機検証と修正（2026-10-19）

### 一時rootの復元と起動理由

USB再接続時のuptimeは約243秒で、完全再起動だった。ro.boot.bootreason/sys.boot.reasonはreboot,checkpoint_commit_failed。servicemanager再起動後に発生したが、理由文字列だけでは因果を断定しない。pstoreは空。既存設定のDirtyFragを再実行してroot/SELinux Enforcingを復元。自動実行・自動再起動は有効化していない。servicemanagerの再起動は以降実行していない。

### monitor_sessionを実機実行

```
monitor_create_result=0 (success)
received_frames=0
awdl_action_frames=0
wireless_transmit=never
monitor_delete_result=0
monitor_session_exit=0
```

削除後ip link show fold5monは不存在。専用monitorの作成/UP/パケットソケットbind/3秒待機/削除の経路は動いた。受信パケットは0件なので、実際のAWDLフレーム受信成功を示す結果ではない。

### VINTF・サービス登録の本修正に成功（サービス管理機能の再起動なし）

Android16のVintfObject.cpp/Utils.hはAPEX info-listのmtime変更でmanifest cacheを失効する。Access.cppはSELinux policy-load status更新でservice_contextsを読み直す。実機libvintfの動的シンボルもGetModifiedTimeを含むことを確認した。

コピーしたapex-info-list.xmlはcp -aだけではSELinuxラベルがtmpfsとなり、通常権限のservicemanagerから読めなかった。元と同じapex_info_fileラベルを明示すると、同じservicemanagerプロセスのままdeclared=1になった。スクリプトに明示ラベル設定を追加した。XML内容は元ファイルとcmpで一致確認し、元ファイル自体は変更せず一時bind mountする。

登録アダプターの実API解決をRTLD_NEXT依存からlibbinder_ndk.soの明示handleへ変更。通常VINTF方式で次を実測:

```
declared=1
registered=1
```

INTERFACE_TRANSACTIONでcom.google.android.moseyservice.IMoseyServiceが返った。Cのone-shot clientでも本物のBinder接続とinterface_version=1取得を確認。local stability診断モードを使わず、成功応答の偽装もなし。

### システムアプリoverlayの不具合と復旧

以前の一時配置は、既存priv-appディレクトリを個別bindした後、親を通常bindしていた。通常bindは子のmountを複製しないため、配置先では既存アプリのディレクトリが空になった。その結果GMS/Play StoreがSYSTEM/PRIVILEGED認識と権限を失い、SecurityExceptionで停止した。端末の元APKは失われていない。

いったん全overlayを解除しzygoteを再読み込みして、両アプリのSYSTEM/PRIVILEGEDとMANAGE_USERS/INTERACT_ACROSS_USERS復旧を確認。その後親をrbindに変更し、既存を含め23個のAPKが配置先から実際に見えることを確認してから再適用した。再適用後もGMS/Play StoreのSYSTEMと両権限を維持し、MoseyもSYSTEM/PRIVILEGEDとなっている。rollbackも複製された子mountから順番に外すよう修正した。意図しないアプリ削除/権限縮小は移植要件ではなく、この不具合として修正した。

### 無線開始失敗の原因をnative出力で特定

src/fold5/mosey_start_probe.cを追加。上流bridgeのAIDL parcel定義を使うone-shot clientで、TCP口は開かない。channel6指定のstartとstopを直接呼び、start=-1/stop=0。上流bridgeの手書きexception解析をAParcel_readStatusHeaderへ置き換え、実際のexception=-4（NULL_POINTER）とメッセージを取得できるようにした。

native側のAndroid logを元の出力先にも渡しつつstderrにも複写できる診断モードFOLD5_MOSEY_STDERR_LOG=1を追加した。実測:

```
Starting mosey on channels [6]
radio=AsNeeded { iface_name: "wonder0", wiphy_name: "wonder" }
Error starting mosey: init_driver
Caused by:
    Could not find wiphy <wonder>
Start mosey returned null
```

これによりNULL_POINTERは無線ドライバー初期化失敗後にnull handleが返る流れだと確認。サービス登録の失敗と区別できた。Moseyが実機phy0へ接続するradio adapter、Googleベンダー制御、データフレーム送信、ACK/チャネル同期は未実装。phy名だけを変えて成功扱いにする変更はしていない。

現在は修正した一時システム配置とVINTF宣言が適用中。native serviceは時間制限付き検証プロセスとしてのみ起動し、試験後は停止。rootと配置は通常の端末再起動で解除される。写真受信は未完成。

証拠: diagnostics/mosey-live-registration-success.txt、fold5-monitor-session-result.txt、preserved-system-app-permissions.txt、mosey-native-radio-diagnostic.txt。
