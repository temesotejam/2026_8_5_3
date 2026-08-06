# 本番実装仕様

## 構成

現在の通信側CoreS3は、成立済みの手動開始経路を残したまま自動運転を統合した版です。921600 bps、8N1、COBS＋CRC32で制御側XIAOと通信し、100 ms周期のHeartbeat、左右前翼・後部ヨー・推進の一括手動指令、固定Waypoint、安全指令、統合Web画面を担当します。CoreS3 Port C（RX GPIO18／TX GPIO17）でGNSSを115200 bps受信し、10 HzでXIAOへ転送します。

制御側XIAOはBNO08X、ToF、INA226、VESC、PCA9685、本番制御器を維持し、既存モード番号を変えずに`Waypoint Only`を追加しています。

AS5600は使用しません。推進モータの回転状態はVESC UARTのERPMで監視します。モータの極対数が未確定のため、機械RPMへの推定換算は行いません。

独自MahonyおよびESKFは実装・実行経路から削除しました。姿勢の唯一の正規入力はBNO08Xの`Rotation Vector`クォータニオンです。

## 制御側ピン

| XIAO端子 | GPIO | 用途 |
|---|---:|---|
| D0 | 1 | 周辺I2C SCL |
| D1 | 2 | 周辺I2C SDA |
| D2 | 3 | BNO08X RST |
| D3 | 4 | BNO08X INT |
| D4 | 5 | BNO08X SDA |
| D5 | 6 | BNO08X SCL |
| D6 | 43 | CoreS3 UART RX |
| D7 | 44 | CoreS3 UART TX |
| D8 | 7 | VESC UART RX |
| D9 | 8 | VESC UART TX |
| D10 | 9 | VESCモータ安全リレー（HIGHで接続、LOWで切断） |

周辺I2CにはToF `0x29`、PCA9685 `0x40`、INA226 `0x44`を接続します。BNO08Xは専用I2C `0x4A`（代替`0x4B`）です。

## 制御則

```text
前翼共通 = pitch PD + ToF height P
左前翼   = 前翼共通 + roll PD
右前翼   = 前翼共通 - roll PD
後部ヨー = yaw PD
```

Auto Waypointでは現在位置から目標点への単純方位ではなく、開始位置から選択目標へのLOS（look-ahead 4 m）方位を使います。目標半径は既定1.5 mで、到達時は推進を停止してDISARMEDへ遷移します。速度上昇時はヨーゲインと最大ヨー指令を下げます。姿勢制御ONではpitchが危険域へ近づくと高さ・roll・推進を抑え、pitch回復を優先します。姿勢制御OFFでも危険姿勢停止は維持します。

| Waypoint | 姿勢制御 | XIAOモード | 動作 |
|---|---|---|---|
| OFF | OFF | Manual | 4出力を手動操作 |
| OFF | ON | Attitude Assist | 左右前翼は姿勢・高さ制御、後部ヨーと推進は手動 |
| ON | OFF | Waypoint Only | 左右前翼は通電中立、後部ヨーはLOS、推進は自動 |
| ON | ON | Auto Waypoint | LOS、姿勢・高さ、自動推進 |

固定点はA〜Hです。座標は`temesotejam/waypoint_drift_los_goal_time_sim`の`main@24868f3`から転記し、`communication/include/fixed_waypoints.h`を唯一のファームウェア定義とします。

制御値と安全閾値は`control/include/app_config.h`に集約しています。現行の主要値は次のとおりです。

| 項目 | 値 |
|---|---:|
| 目標高さ | 0.45 m |
| 自動推進指令 | 55% |
| VESC Duty上限 | 60% |
| サーボ範囲 | 1200–1800 µs |
| サーボ更新 | 50 Hz |
| サーボ変化速度 | 300 µs/s |
| 低電圧制限／停止 | 9.5 V／8.5 V |
| 過電流制限／停止 | 22 A／28 A |
| 拘束判定 | 指令25%以上、8 A以上、VESC 100 ERPM未満が1秒 |

## 安全状態

起動状態はDISARMEDです。ARM条件はモード別です。

| モード | ARMに必要なもの |
|---|---|
| Manual | PCA9685、CoreS3 heartbeat、500 ms以内の手動指令、1つ以上の出力選択 |
| Attitude Assist / Heading Hold | Manualの条件に加えてBNO08X |
| Waypoint Only / Auto Waypoint | PCA9685、CoreS3 heartbeat、BNO08X、有効GNSS、固定目標、VESC電源・回転情報 |

ToFは欠測時に高さ項だけを無効化するためARM必須ではありません。INA226が未接続でも、有効なVESCテレメトリの入力電圧・入力電流を電源保護へ使用できます。AS5600は使用しません。

Manualでは左前翼CH0、右前翼CH1、後部ヨーCH2、推進の4出力を1つのUARTパケットで同時に指令します。推進指令は0.00〜1.00をVESC Duty上限60%へ比例変換します。VESCまたは電源情報が無効なら、推進だけを0へ抑止し、3本のサーボ操作は継続します。非ゼロDutyを送るときだけD10をHIGHにします。

START後に次のどれかを検出すると、推進Dutyを即時0、PCA9685を全チャンネルFull OFFにします。

- CoreS3 heartbeat途絶
- BNO08X姿勢または角速度の無効・期限切れ
- GNSS期限切れ（Waypoint Only / Auto Waypoint）
- 電源監視または回転数監視の無効・期限切れ
- VESCテレメトリ期限切れまたはfault
- 臨界低電圧、臨界過電流、モータ拘束
- 非有限値、危険姿勢、E-STOP

ToFだけが一時的に無効になった場合は、高さ項を0にして姿勢制御を継続し、テレメトリへdegraded flagを残します。

D10の安全リレーは起動直後からLOWです。VESCへ非ゼロDutyを送る直前だけHIGHにし、Duty 0では0指令を送信してからLOWに戻します。STOP、DISARM、E-STOP、FAULT、通信途絶、DRY RUN、VESC指令のUART送信失敗時もLOWです。VESCのテレメトリ要求だけではHIGHになりません。リレー状態は`ActuatorState.motorRelayEnabled`としてCoreS3とWeb APIへ送ります。

## Web操作

CoreS3のAP `BOAT-CONTROL`へ接続し、画面に表示されるIPアドレスを開きます。画面でWaypointと姿勢制御を個別に選びます。

1. Waypointと姿勢制御のON/OFFを選びます。
2. Waypoint ONではA〜Hから固定目標を選び、CoreS3とXIAOの両方がGNSS FIXになるまで待ちます。
3. Waypoint OFFでは必要な手動入力値を決めます。
4. `開始`を押します。CoreS3本体がDISARMED確認、必要ならWaypoint ACK、Mode ACK、必要なら手動値ACK、ARM確認、START確認を順番に実行します。
5. 通常停止は`停止`、緊急時は`緊急停止`を使用します。

手動系ではCoreS3が4値を1パケットにまとめて200 ms周期で更新します。Waypoint系では手動更新を要求せず、XIAOがGNSS・BNO08X・VESC情報から出力を生成します。姿勢制御ONの場合だけToFを高さ制御に使います。停止時は推進Duty 0、D10 LOW、PCA9685全チャンネルFull OFFです。

ブラウザは`開始`、`停止`、`緊急停止`という単発要求だけをCoreS3へ送ります。ブラウザ側JavaScriptはARMやSTARTを直接並べて送らないため、手動指令の実送信前にARMが追い越すことはありません。

## 現在外している通信側機能

SD自動記録、時刻同期、任意座標編集、BNO転送診断、ベンチマークAPIは現行CoreS3ビルドから外しています。GNSS受信と固定Waypoint選択は復帰済みです。大会運用前に必須のSD記録は、成立済みの統合運転経路と分離したモジュールとして再追加します。

## 検証

`tests/controller_test.cpp`は自動航行、ToF degraded、電源制限、拘束停止、危険pitch優先、サーボ変換、VESCランプを検証します。`tests/protocol_test.cpp`はCOBS＋CRC32往復、コマンド受付、重複排除、不正CRCのACKを検証します。GitHub Actionsでは両テストと、通信側・制御側のPlatformIOビルドを実行します。
