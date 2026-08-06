# 本番実装仕様

## 構成

通信側CoreS3はGNSSを受信し、100 ms周期で制御側へ送ります。制御側XIAOはBNO08X、ToF、INA226、VESCを読み、本番制御器を50 Hzで実行します。全テレメトリは921600 bps、8N1、COBS＋CRC32でCoreS3へ戻り、SDとWeb画面へ同時に反映されます。

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

Auto Waypointでは現在位置から目標点への単純方位ではなく、前区間からのLOS（look-ahead 4 m）方位を使います。目標半径は既定1.5 mで、最終点到達時は推進を停止してDISARMEDへ遷移します。速度上昇時はヨーゲインと最大ヨー指令を下げます。pitchが危険域へ近づくと高さ・roll・推進を抑え、pitch回復を優先します。

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
| Auto Waypoint | PCA9685、CoreS3 heartbeat、BNO08X、有効GNSS、1点以上の経路、VESC電源・回転情報 |

ToFは欠測時に高さ項だけを無効化するためARM必須ではありません。INA226が未接続でも、有効なVESCテレメトリの入力電圧・入力電流を電源保護へ使用できます。AS5600は使用しません。

Manualでは左前翼CH0、右前翼CH1、後部ヨーCH2、推進を個別に選択します。未選択サーボは中立PWMではなくPCA9685 Full OFFです。VESCまたは電源情報が無効な状態で推進を選んでも、推進指令だけを0へ抑止し、接続済みサーボの手動操作は継続します。D10はLOWのままです。

START後に次のどれかを検出すると、推進Dutyを即時0、PCA9685を全チャンネルFull OFFにします。

- CoreS3 heartbeat途絶
- BNO08X姿勢または角速度の無効・期限切れ
- GNSS期限切れ（Auto Waypoint）
- 電源監視または回転数監視の無効・期限切れ
- VESCテレメトリ期限切れまたはfault
- 臨界低電圧、臨界過電流、モータ拘束
- 非有限値、危険姿勢、E-STOP

ToFだけが一時的に無効になった場合は、高さ項を0にして姿勢制御を継続し、テレメトリへdegraded flagを残します。

D10の安全リレーは起動直後からLOWです。VESCへ非ゼロDutyを送る直前だけHIGHにし、Duty 0では0指令を送信してからLOWに戻します。STOP、DISARM、E-STOP、FAULT、通信途絶、DRY RUN、VESC指令のUART送信失敗時もLOWです。VESCのテレメトリ要求だけではHIGHになりません。リレー状態は`ActuatorState.motorRelayEnabled`としてCoreS3、Web API、SDログへ送ります。

## Web操作

CoreS3のAP `BOAT-CONTROL`へ接続し、画面に表示されるIPアドレスを開きます。

1. DISARMED中にモードとウェイポイントを設定します。
2. Manualの場合は連続手動送信を有効にします。
3. 手動出力では、動かすチャンネルだけをチェックして連続送信を開始します。
4. `ARM`で選択モードのプリフライト条件を確認します。
5. `START`で物理出力を開始します。
6. 通常停止は`STOP`、緊急時は`E-STOP`を使用します。

Web画面は日本語表示です。制御側通信を「未受信／正常／通信途絶」に分け、PCA9685・BNO08X・ToF・GNSS・INA226・VESCの有効状態、実サーボパルス、D10リレー状態を表示します。手動指令送信と状態更新は別タイマーで動くため、連続手動操作中も状態表示を更新します。

ウェイポイント入力は1行に`緯度,経度`です。Web APIは最大16点、座標範囲、到達半径、リビジョン、CRCを検証し、制御側のACKを画面へ返します。

## 記録

SDが利用可能なら起動直後に新しいログを開始します。BNO08X、ToF、GNSS、INA226、VESC（ERPM・Duty・電圧・電流・温度・fault）、制御計算、物理PWM、状態遷移、通信診断を同一のBIN記録へ保存します。Web APIから記録開始・停止・一覧・ダウンロードも可能です。

## 検証

`tests/controller_test.cpp`は自動航行、ToF degraded、電源制限、拘束停止、危険pitch優先、サーボ変換、VESCランプを検証します。`tests/protocol_test.cpp`はCOBS＋CRC32往復、コマンド受付、重複排除、不正CRCのACKを検証します。GitHub Actionsでは両テストと、通信側・制御側のPlatformIOビルドを実行します。
