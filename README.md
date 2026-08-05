# 2026_8_5_3 — 水上翼船ファームウェア

通信側と制御側を完全に分離した、実機運用用の2ノード構成です。各フォルダは独立したPlatformIOプロジェクトで、片方から他方のソースを参照しません。

| フォルダ | 基板 | 担当 |
|---|---|---|
| `communication/` | M5Stack CoreS3 | GNSS、SD自動記録、Web操作画面、制御側UART通信 |
| `control/` | XIAO ESP32S3 | BNO08X、ToF、INA226、AS5600、VESC、PCA9685、安全管理、自動制御 |

## 実装済みの本番経路

- 姿勢はBNO08X内蔵`Rotation Vector`を直接使用。独自姿勢推定器はありません。
- GNSS位置からローカル座標を作り、LOS誘導で最大16点のウェイポイントを追従します。
- ToF高さ、BNO姿勢・角速度を使って左右前翼と後部ヨーを50 Hzで制御します。
- CH0は左前翼、CH1は右前翼、CH2は後部ヨーです。
- INA226、AS5600、VESC UARTを常時監視し、低電圧、過電流、拘束、VESC fault、センサ途絶で安全停止します。
- DISARMED、E-STOP、FAULTではPCA9685をFull OFFにし、VESCへDuty 0を送ります。
- CoreS3のWeb画面からARM、START、STOP、E-STOP、モード、手動指令、方位、ウェイポイントを操作できます。
- CoreS3は起動時からSDへ記録し、制御出力、実PWM、電源、回転数、安全状態も保存します。

## ビルド

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
