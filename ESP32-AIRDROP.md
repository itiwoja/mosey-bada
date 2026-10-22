# ESP32 を使った Fold5 の AirDrop 受信

2026-10-19 に Codex から引き継いだあとの方針と現状。それ以前の記録は FOLD5-PORT.md。

## 方針

**2026-10-22: iPhone標準AirDrop → ESP32 → Fold5の写真1枚の受信・保存・MediaStore登録を実機確認。** 小さいJPEGで成功。大容量写真の安定受信と検出の再現性には課題が残る。

- iPhone 側には何も入れない（AirDrop を「すべての人（10分間）」にするだけ）。
- USB Wi-Fi アダプターは買わない。追加のハードウェアは手持ちの ESP32（ESP32-WROOM-32 / DevKitC、2.4GHz のみ）だけ。
- Fold5 内蔵の qca6490 は、モニターモードの送信処理が空で AWDL を実現できない（FOLD5-PORT.md）。公式の Quick Share AirDrop も Fold5 は対象外。

## 構成

```
iPhone ──AWDL(2.4GHz ch6)── ESP32 ──USB(UART 921600)── Fold5
                          OWL コア               awdl_bridge → TAP awdl0
                          (sync/選出/チャネル/   Alpine chroot: OpenDrop(iOS26パッチ)
                           action/data フレーム) tidy.py → Pictures/AirDrop
```

| 部分 | 場所 |
|---|---|
| ESP32 ファームウェア | `esp32/awdl-radio/`（ESP-IDF v5.4.3） |
| ESP32⇔Fold5 の通信方式 | `esp32/awdl-radio/main/hostlink_proto.h`（SLIP + CRC16） |
| AWDL 実装 | `third_party/owl/`（jedbillyb/owl。変更点は `PORTING.md`） |
| OpenDrop の iOS 26 パッチ | `third_party/airdrop-mt7921/patches/`（17本） |
| Fold5 側ブリッジ | `src/fold5/awdl_bridge.c`（usbfs で CP210x/CH34x を直接操作。カーネルにシリアルドライバーがない） |
| 端末での制御 | `tools/fold5-airdrop/fold5-airdrop.sh`、`tidy.py` |
| パッケージ作成 | `tools/fold5-airdrop/package.sh` |

ESP32 は 6ch に固定。iPhone のチャネル順序をそのまま宣言し（iOS 26 は自分の順序と違う相手に送らない: airdrop-mt7921 FINDINGS §25）、送信は順序が 6ch のスロットだけで行う（`AWDL_FIXED_RADIO_CHANNEL`）。

## 確認済み

- ファームウェアのビルド（DRAM 21%）。
- `tests/hostlink_proto_test.c`: 通信方式の往復 16,000 回、壊したフレーム 4,000 個はすべて CRC で除外。
- `tests/run-awdl-sim.sh`: OWL 2台のシミュレーション（ESP32 と同じ受信処理）。ピア検出、iPhone 役のマスター選出、順序の採用、送信ゲートが 6ch スロットだけで開くこと、データ受信、1400 バイトの IPv6 パケットが 1500 バイト上限に収まること。ASan/UBSan でエラーなし。
- このテストで OWL のバグ4件を修正（受信データが 1518 バイト固定、A-MSDU で配列あふれ、選出処理のメモリリーク、アラインメント違反）。
- Fold5 実機（ESP32 なし、`AIRDROP_TAP_ONLY=1`）: chroot で Python と OpenDrop が動く。awdl0 にリンクローカルアドレスとルーティング規則が付く。OpenDrop が 8771 で待ち受け、awdl0 経由の HTTPS に 200 を返す。mDNS の告知が awdl0 から出る。

## 未確認・リスク

### 2026-10-22 ESP32 初回実機確認

- Windows COM7（CP210x）経由で ESP32-D0WD-V3 revision v3.1 へ書き込み成功。全イメージのハッシュ検証済み。
- 921600 baud の SLIP/CRC 通信を確認。PING に HELLO（version 1、MAC `8e:94:df:b4:f1:4c`、ch6）が返り、CRC も一致。
- 5秒間隔の統計2件で、受信フレーム 552→665、AWDL action 69→105、AWDL data 4→4、action送信 241→362。キュー廃棄・送信APIエラーはいずれも0。ピア数は0で、相手端末との接続・ファイル転送は未確認。
- Fold5 は ADB 未接続のため、端末との USB ブリッジ接続は未確認。

### 2026-10-22 Fold5 接続試行

- 一時root復元後、PC接続からESP接続への差し替えを `tools/fold5-airdrop/wait-for-esp.sh` で待機。CP210x検出、HELLO、awdl0構成、OpenDropの告知・HTTPS待受まで成功。
- AWDLピア1台を検出し、同期先のch6スロットを採用。action/data送信APIエラー0。iPhoneのAirDrop一覧にはFold5が表示されず、OpenDropへのHTTP接続も未記録。送信API成功だけでは無線到達は証明できない。
- 初回ログ: `diagnostics/esp32-first-bridge.log`、`diagnostics/esp32-first-opendrop.log`。
- ブリッジに任意の `AIRDROP_PCAP` を追加。UART両方向のEthernetフレームを最大8MiBまで保存する。診断モードで14フレーム（mDNS 7件）のPCAP形式を検証済み。次回は詳細無線ログとPCAPで検出通信の停止箇所を確認する。
- 既存rootfsへのinstall再実行は既存シンボリックリンクをAndroid tarが拒否して失敗。今回は既存環境のPython/OpenDrop/SSL動作を確認し、bridgeとtidy.pyを個別更新した。

### 2026-10-22 検出成功・アップロード途中停止

- iPhoneにFold5が表示され、選択後に送信中になった。OpenDropでも `/Discover` 200、`/Ask` 200、`/Upload`（TotalBytes 6,834,439、dvzip）開始を確認。写真転送の完了は未達。
- PCAPのアップロード接続では約85秒にiPhone→Fold5のTCPペイロード約602KB（同一seq/length重複を除くと約551KB）。これにはTLS等も含み、完成ファイルのバイト数ではない。切断までにOpenDropが読んだアーカイブは約0.38MB。大きな写真の完了には現状の速度が問題。
- tidy.pyが5秒以上更新のない受信途中のraw dvzipを移動し、OpenDropの展開がENOENTになる不具合を確認。raw dvzip/cpioと.partialを移動対象から除外し、不完全ファイルを含むフォルダーも残すよう修正。停止中の開いたアーカイブへ追記しても元パスが残り、完成ファイルだけ移動される回帰テストを実行済み。
- 証拠は `diagnostics/esp32-transfer.pcap`、`diagnostics/esp32-transfer-opendrop.log`、`diagnostics/esp32-transfer-bridge.log`。次は小さな画像で完了経路を検証。

- **ESP32 の起動・ホスト通信・無線API動作は確認済み。** 特に次の点は未確認。
  - iPhone が 6ch にいるのは 16 スロット中 1 スロット（1秒あたり約65ms）。届くか、届いても数分かかる見込み。
  - ESP32 が自分宛てのユニキャストにハードウェアで ACK を返すか（AWDL の転送に必須）。
  - `esp_wifi_80211_tx` の送信タイミングと、6ch スロットへの同期の精度。
- 受信したファイルがギャラリー（MediaStore）に登録されるか。端末の内部ストレージが外れていたため未確認。
- 受信の承認は自動（AIRDROP_CONFIRM 未設定）。受信は `AIRDROP_MINUTES`（既定10分）で自動的に止まる。

## 手順

### 大きい写真向けの受信処理修正（2026-10-22、実転送は検証待ち）

- 上流recv-windowパッチがSO_RCVBUF設定をget_propertiesのreturn後に挿入していたため、実行されていなかった。`tools/fold5-airdrop/opendrop-esp32-stream.patch`でサーバーソケット作成後、TLSラップ前に移動。Fold5診断起動ログで `SO_RCVBUF = 8388608 B` を確認。
- HTTPチャンク全体を一括readしていた処理を、呼び出し元バッファー以下のread1へ変更。長いチャンクでも到着したデータからディスクへ書き出し、次の読み取りのタイムアウトを更新できる。チャンク拡張・終端・trailerにも対応。
- chrootへ無通信タイムアウト120秒、下限30秒を渡す（環境変数で上書き可能）。再試行は `AIRDROP_MINUTES=30`。これは受信可能時間を延ばすもので、無線の速度向上を保証しない。
- `tests/opendrop_stream_test.py`の4テスト（6MiB単一チャンクを1KiBずつ受信、途中切断、境界・不正フレーム、実ソケット設定）とtidy回帰テストが成功。Fold5上の受信サービス起動も確認。数MBの写真受信完了はまだ未確認。
- package.shで既存17パッチの後に追加パッチを適用する。distのrootfsと端末上のOpenDropを更新。端末のserver.pyは `.before-esp32-stream` として変更前のバックアップを保持。

### 普通の写真（数MB）向けの改善（2026-10-22、実機未検証）

失敗した6.8MB転送の分析（PCAPとブリッジログ）:
- 1スロット＝64TU≈65ms。転送中のiPhoneは16スロット中ch6が1〜2個（約1.05秒に1回）。ESP32はiPhoneの順序をそのまま宣言しているため、iPhoneはch44のスロットでもESP32宛てに送り、届かない。
- iPhone→Fold5のTCP再送は22.7%。RTOの後退とみられる4〜17秒の空白がある。Fold5側のACKは即時（中央値0.6ms）で、ゼロウィンドウはなかった。主因は無線での欠落。
- OpenDropが「HEIF非対応」と宣言していたため、iPhoneはHEICをJPEGに変換して送っていた（容量が2〜3倍になる）。

変更:
- OpenDrop: `tools/fold5-airdrop/opendrop-heic.patch` でHEIF入れ物（ContainerFormats）とHEVC符号（Codecs hvc1）への対応を宣言（`AIRDROP_HEIC=1` が既定、0で従来どおり）。HEICはhvc1の画像をHEIFに入れたもので、ContainerFormatsだけでは実機2回目でJPEGが届いた。Live PhotoのMOVが付かないよう、Vendor（LivePhotoFormatVersion）は宣言しない。カメラが「互換性優先」ならJPEGのまま届く。tidy.pyは拡張子で区別せず、HEIC・JPEGとも同じように移す（tests/tidy_test.py）。
- ESP32: 802.11の再送による重複を除去（送信元とseq_ctrlで判定）。unicastの送信キューを8から32に増やし、満杯時は古いものから捨てる。
- ESP32: チャネル順序の宣言を `AIRDROP_CHANSEQ=verbatim|intersect` で切り替えられるようにした（`HL_SET_CHANSEQ`）。`intersect` はch6のスロットだけを宣言し、ほかを空にする。重なりが0のときはverbatimと同じ宣言になる。既定はverbatim（今までの動作）。
- 統計行の末尾に `rx retry / dup / ampdu | uq drop | chanseq` を表示。旧ファームの統計もそのまま表示できる。

実機での試し方（ファーム書き込みと `package.sh --push` が必要）:
1. 既定（verbatim＋HEIC）で普通の写真を1枚送る。bridge.log の `rx retry` が `awdl_data` に対して多ければ、ESP32がACKを返せていない疑いがある。
2. `AIRDROP_CHANSEQ=intersect` で同じ写真を送る。iPhoneにFold5が表示されなくなったら、§25の制約に当たったので verbatim に戻す。表示されたら、ch6のスロット数と転送速度を比べる。
3. どちらの場合も `AIRDROP_PCAP` を付け、再送率と空白を前回と比べる。

実機1回目（2026-10-22 12:29、verbatim＋HEIC宣言）: iPhoneに表示されず、/Discoverも来なかった。約2.5分で終了（`diagnostics/run1` はなし、端末の `/data/local/airdrop/run1/`）。
実機2回目（12:35、同じ設定、無線ログ7とPCAPあり、`diagnostics/run2/`）: 表示→/Discover→/Ask→/Upload。ファイルは前回と同じ6,834,439バイトの`.JPG`で、HEIC宣言は効かなかった（元の写真がJPEGだった可能性もある）。約113秒で切断し、届いたのは0.38MB。データフレームの約19%（141/750）が802.11の再送で、そのうち重複として捨てたのは11件だけ。つまり大半は、最初の送信がESP32に届いていない。unicastの送信キューは32本でも12回あふれた。→ 次は `AIRDROP_CHANSEQ=intersect` を試す。

**実機3回目（12:47、intersect＋HEIC宣言(hvc1込み)、`diagnostics/run3/`）: 6.0MBの普通の写真（4032×3024 JPEG）の受信・保存・MediaStore登録に成功。**
- /Discover→/Ask→/Upload。TotalBytes 6,299,212。アップロード開始から約3分48秒（約30KB/s、前回の約5倍）。
- 最後の494バイトの手前でiPhoneが送信を止め、30秒の無通信検出で救済した。展開したJPEGは先頭FFD8・末尾FFD9で、MediaStoreに `image/jpeg 4032x3024 6,299,212B` として登録された。
- iPhoneは、ESP32がch6のスロットだけを宣言すると、自分の順序でもch6を増やした（1〜2/16 → 最大5/16。例 `44×8,6,6,44,6,6,6,44,44`）。データフレームは5,137件（2回目は750件）。retry 1,063件、dup 104件、uq drop 173件。
- 届いたのはJPEGだった。カメラ設定が「互換性優先」だったのか、HEIC宣言がまだ足りないのかは未確認。
- この結果を受けて `fold5-airdrop.sh` の既定を `AIRDROP_CHANSEQ=intersect` に変更した。

run3の律速分析（PCAP）と対策（2026-10-22、実機は検証待ち）:
- TCPは健全だった。RTOの空白は0、穴埋めの再送は2.9%。速度を決めていたのはiPhoneのch6スロット数で、5スロットのときは約50KB/s、3スロットのときは約16KB/s。どちらにするかはiPhoneが決める。
- 最後の30.1秒は、全データが届いてACKも返した後の待ちだった。iPhoneは応答を待ち、OpenDropは30秒の無通信を待っていた（`AIRDROP_STALL_FLOOR=30` のため適応タイムアウトも効いていなかった）。TotalBytesは本体のファイルの合計（6,299,212）と一致した。
- `tools/fold5-airdrop/opendrop-early-finish.patch`: 3秒の無通信（`AIRDROP_DONE_QUIET`）で受信済みのアーカイブを調べ、完全なファイルの合計がTotalBytes以上なら完了扱いにする。読み途中では中断せず、読む前にselectとpeekで待つ。足りなければ従来のタイムアウトに任せる。テストは `tests/opendrop_early_finish_test.py`。端末に反映済み。
- ESP32: 送信キュー内で、同じフローの未送信の純粋なACKを、ACK番号が進んだ新しいもので置き換える（`ackcoalesce.h`）。SACK付き、データ付き、制御フラグ付き、ACK番号が同じもの（重複ACK）は置き換えない。ESP32に書き込み済み（13:2x）。

実機4回目（13:26、早期完了＋ACK間引き、`diagnostics/run4/`）:
- 1枚目 1,232,756 B（1179×2096 JPEG）: アップロード開始から32.2秒（約38KB/s）。`upload complete by content ... after 3s quiet` で即完了し、MediaStoreに登録された。終盤の30秒待ちは消えた。
- 2枚目 8,342,595 B: 44秒で1.82MB（約41KB/s）まで進んだところで、ESP32を外したため切断（`usb read: Protocol error`）。最後まで受けていれば約3.4分の見込み。
- ACK間引きは1,136件。2枚目の開始直後に、ESP32の受信キュー（EVQ 96）のあふれが83件出た。UARTの送り出しが詰まったと見られる（推測）。対策は921600 baudより上げること。ただしCP210xの型番で上限が違う。

直近の成功記録（2026-10-22 11:47 JST）:

- `/Discover`、`/Ask`、`/Upload` がすべて200。38,846バイトの転送はアップロード開始から約7.3秒で展開完了。
- `Pictures/AirDrop/IMG_0659-compressed.jpeg` に38,846バイトで保存。tidyログとMediaStore照会の表示名・サイズが一致。
- SHA-256: `dd07b33f4cd2f9bd4bd841c40c277d65182bf1f00e7d971fecb87145a89d9584`（受信後の値。送信元とのハッシュ照合は未実施）。
- 証拠: `diagnostics/esp32-success-{opendrop,bridge,tidy}.log`、`diagnostics/esp32-success.pcap`。
- PCとESPを同時接続できない場合は、一時rootのシェルで `nohup sh /data/local/tmp/fold5-airdrop/wait-for-esp.sh </dev/null >/data/local/airdrop/wait-for-esp.log 2>&1 &` を実行してから、5分以内にPCから外してESPへ差し替える。ESP検出後に10分間の受信を開始する。待機スクリプトは事前に同じディレクトリへ配置する。

ビルド（WSL）:

```bash
export PATH=~/esp/pybin:$PATH; . ~/esp/esp-idf-v5.4/export.sh
cd esp32/awdl-radio && idf.py build && ./flash-windows.sh      # ESP32 を PC につないで書き込み
cd ../.. && ANDROID_SERIAL=RFCW713KDJB PY=~/esp/pybin/python3 tools/fold5-airdrop/package.sh --push
```

Fold5（DirtyFrag で一時 root を取ったあと、ESP32 を USB-C につなぐ）:

```bash
su -c 'sh /data/local/tmp/fold5-airdrop/fold5-airdrop.sh install'   # 初回だけ
su -c 'sh /data/local/tmp/fold5-airdrop/fold5-airdrop.sh start'
su -c 'sh /data/local/tmp/fold5-airdrop/fold5-airdrop.sh status'
```

iPhone: AirDrop を「すべての人（10分間）」→ 写真 → 共有 → AirDrop → 「Fold5」。保存先は `Pictures/AirDrop`。

## 注意

- framework や servicemanager を再起動しない。framework だけを再起動すると内部ストレージがマウントし直されず、通常の再起動まで戻らない（2026-10-19 に発生）。
- KernelSU の `su` は init のマウント名前空間で動き、そこでは `/storage/emulated` が使えないことがある。保存は MediaProvider の名前空間経由（`tidy.py` の `mediaprovider:` 指定）にしている。

### コード変更だけでの高速化（2026-10-22、実機は検証待ち）
- HEIC: Discover応答に `Vendor`（com.apple、OSVersion 10.13.3、LivePhotoFormatVersionなし）も足した。ContainerFormatsとCodecsだけでは2回ともJPEGだった。
- UART高速化: `HL_SET_BAUD`/`HL_BAUD_OK`（手順は hostlink_proto.h）。ブリッジは最初のHELLOの後、`AIRDROP_BAUD`（既定 `2000000,1500000,1000000`）を順に試し、CP210xは型番（GET_PARTNUM）で候補を絞る。新しい速度で1.5秒通じなければ両側とも921600に戻る。ESP32は、高速で確定した後にホストから6秒フレームが来なければ921600に戻る（ブリッジだけの再起動に対応）。ブリッジは、高速中に4秒無音ならESP32の再起動とみなして再交渉する。速度切替では制御線を触らない（`serial_set_baud`。DTR/RTSはESP32のEN/IO0につながっている）。
- ESP32の受信キュー EVQ_LEN を96から160に増やした。空きヒープが32KB未満なら積まない。
- `AIRDROP_CHANSEQ=fill`（実験）: intersectに加え、iPhoneが空にしているスロットにもch6を宣言する。既定はintersectのまま。
- ログで見る場所: `uart at N baud`、`chanseq fill`、stats の `dropped`。

## 内蔵Wi-Fiでのch44受信試験（受信専用）

目的: iPhoneはAWDLのch44（5220MHz、80MHz幅のことがある）でもESP32宛てにデータを送るが、ESP32は2.4GHz専用のため取りこぼす。Fold5の内蔵Wi-Fiを受信専用のmonitorにして5220MHzを聞き、ESP32のMAC宛てのAWDLデータフレームが何件届くかを数える。TUNへ流す処理はまだ作らない。

試し方: 受信と同じ手順のまま環境変数を付ける。`wait-for-esp.sh` は `exec` で `start` を呼ぶので、環境変数はそのまま引き継がれる。
```sh
AIRDROP_CHANSEQ=verbatim AIRDROP_SNIFF=5220 nohup sh /data/local/tmp/fold5-airdrop/wait-for-esp.sh </dev/null >/data/local/airdrop/wait-for-esp.log 2>&1 &
```
`AIRDROP_SNIFF_WIDTH`（既定 80）、`AIRDROP_SNIFF_PCAP=1`（`$BASE/sniff.pcap` も保存）も指定できる。monitorの秒数は `AIRDROP_MINUTES` × 60（上限1800秒）。

見る場所: `/data/local/airdrop/sniff.log`（`fold5-airdrop.sh status` の `sniff:` 行も同じ）。
- `set_channel_result=`: 0なら5220MHz/80MHzへの切替に成功。0以外なら切替に失敗しており、受信は続くが判定しない。
- `to_target=`: 宛先がESP32のMACのAWDLデータフレーム数。
- `chan=`: radiotapで最も多かった周波数（5220になっているか）。
- `top_dst`: AWDLデータの宛先の上位8件。

判定: `to_target` が数百件以上なら、5GHzでも拾えているので TUN へ流す実装に進む。0件なら（`set_channel_result=0` を確認したうえで）この案は終わり。

注意:
- 受信中はFold5の通常Wi-Fiが不安定になりうる。
- 無線送信はしない。wlan0・規制・firmwareの設定は変えない。
- 停止は `fold5-airdrop.sh stop`（`sniff.pid` も止める。monitor_session は SIGTERM でインターフェースを消してから終わるため、最大3秒待つ）。

実機の事前試験（2026-10-22、iPhoneなし、`monitor_session --sniff` を直接実行）: 通常モード（`/sys/module/qca6490/parameters/con_mode` = 0）では使えない。
- wlan0 UP（eduroam 2462MHzに接続中、またはWi-Fiオフで未接続）: fold5monは作れるが、`SET_WIPHY`/`SET_CHANNEL` とも errno 16（EBUSY）。5220/80MHz、2437/20MHz、wlan0と同じ2462/20MHzのどれも同じで、受信は10秒で0件。cfg80211はmonitor以外のインターフェースがUPだとmonitorのチャネル変更を断る。
- Wi-Fiオフ＋`ip link set wlan0 down`: fold5monの作成が -22（EINVAL）で失敗。
- p2p0、wifi-aware0は元からDOWN。試験後にWi-Fiをオンに戻し、eduroamへの再接続を確認した。
- 残る手段は `con_mode=4`（ドライバー全体をmonitorモードで再初期化）だけ。Wi-Fiドライバーの作り直しになり、戻らなければ再起動が必要で、そのとき一時rootも消える。未実施。
