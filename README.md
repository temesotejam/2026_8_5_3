# 2026_8_5_3 — 水上翼船ファームウェア

通信側と制御側を完全に分離した、実機運用用の2ノード構成です。各フォルダは独立したPlatformIOプロジェクトで、片方から他方のソースを参照しません。

| フォルダ | 基板 | 担当 |
|---|---|---|
| `communication/` | M5Stack CoreS3 | GNSS、SD自動記録、Web操作画面、制御側UART通信 |
| `control/` | XIAO ESP32S3 | BNO08X、ToF、INA226、VESC、PCA9685、安全管理、自動制御 |

## 実装済みの本番経路

- 姿勢はBNO08X内蔵`Rotation Vector`を直接使用。独自姿勢推定器はありません。
- GNSS位置からローカル座標を作り、LOS誘導で最大16点のウェイポイントを追従します。
- ToF高さ、BNO姿勢・角速度を使って左右前翼と後部ヨーを50 Hzで制御します。
- CH0は左前翼、CH1は右前翼、CH2は後部ヨーです。
- INA226とVESC UARTを常時監視し、低電圧、過電流、VESC ERPMによる拘束判定、VESC fault、通信途絶で安全停止します。AS5600は使用しません。
- 制御側XIAOのD10はVESCモータ安全リレーです。非ゼロDuty指令中のみHIGHとし、それ以外はLOWに固定します。
- DISARMED、E-STOP、FAULTではPCA9685をFull OFFにし、VESCへDuty 0を送ります。
- ManualではWeb画面で選択した出力だけを有効にし、未選択のサーボチャンネルはFull OFFにします。
- GNSS・VESC・INA226が未接続でも、PCA9685とCoreS3–XIAO通信が正常なら接続済みサーボをManualで個別試験できます。
- VESC応答が無効な間は推進出力とD10安全リレーを有効にしません。
- CoreS3の通常Web画面は、接続状態、1チャンネルの選択、出力値、開始、停止、緊急停止だけに絞っています。開始時のManual設定、ARM、STARTは画面内部で順番に実行します。
- CoreS3は起動時からSDへ記録し、制御出力、実PWM、電源、回転数、安全状態も保存します。

## ビルド

### Webからコンパイル済みファームウェアを書き込む

GitHub Actionsが通信側と制御側を自動コンパイルし、次のWeb Installerへ公開します。

**[水上翼船 Firmware Installer](https://temesotejam.github.io/2026_8_5_3/)**

- 制御側XIAO ESP32S3と通信側CoreS3は別ボタンです。
- PC版ChromeまたはEdgeのWeb Serialを使用します。
- 通常はXIAOがCOM4、CoreS3がCOM6です。COM3は使用しません。
- 書き込み前にモータ用バッテリーとサーボ外部電源を切ってください。
- 各ビルドの結合済みBIN、SHA-256、manifestはGitHub Actionsのartifactからも取得できます。

### PlatformIOを使う

```bash
pio run -d control
pio run -d communication
```

書き込み時は制御側をCOM4、通信側をCOM6へ指定してください。COM3は使用しません。

```bash
pio run -d control -t upload --upload-port COM4
pio run -d communication -t upload --upload-port COM6
```

配線、制御則、安全条件、操作手順は[`docs/PRODUCTION_IMPLEMENTATION.md`](docs/PRODUCTION_IMPLEMENTATION.md)にまとめています。
