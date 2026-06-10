# Boot Image Network/Wi-Fi/Bluetooth Failure Investigation

## 背景

当前内核源码路径：

```bash
/home/lrc/patent/kernel/kernel_platform/common
```

构建命令：

```bash
cd ~/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
```

用户的修改是在原厂开源内核代码上叠加的额外内容，没有主动修改内核结构体。构建后使用新生成的 kernel image 重新打包 `boot.img` 并刷入/启动手机。

已确认：

- `/home/lrc/patent/original_boot.img` 是正确的原厂 boot 镜像。
- 刷回原厂 `original_boot.img` 后，手机 Wi-Fi 可以正常使用。
- 使用新构建内核打包出的 `/home/lrc/patent/boot.img` 启动后，手机可以点亮进入系统，但 Wi-Fi/蓝牙不可用，默认网络不可用。

本次排查重点不是纠结版本号本身，而是从 dmesg/logcat 看网络、Wi-Fi、蓝牙驱动实际挂点。

## 日志获取

手机通过跳板机访问：

```bash
ssh lrc@192.168.61.230
```

跳板机上的 macOS 环境可以直接执行 `adb`/`fastboot`。

启动坏 boot 的参考命令：

```bash
scp -q /home/lrc/patent/boot.img lrc@192.168.61.230:~/Documents/ssh_dir/bad_boot.img
ssh lrc@192.168.61.230 'export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"; adb reboot bootloader; sleep 8; fastboot boot ~/Documents/ssh_dir/bad_boot.img'
```

抓取坏内核日志：

```bash
ssh lrc@192.168.61.230 'export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"; adb shell su -c "dmesg" > /tmp/bad_dmesg.txt; adb logcat -d -b all > /tmp/bad_logcat.txt'
```

关键日志文件：

- `/tmp/bad_dmesg.txt`
- `/tmp/bad_logcat.txt`
- `/tmp/bt_retry_dmesg.txt`
- `/tmp/bt_retry_logcat.txt`

## 内核与模块组合

原厂可用 boot 启动时内核版本：

```text
6.12.23-android16-5-g188c695beb6d-ab14367805-4k
```

新构建坏 boot 启动时内核版本：

```text
6.12.23-android16-5-o-gfbffb6e59e1e-4k
```

手机上原厂 `/system/lib/modules/rfkill.ko`：

```text
vermagic: 6.12.23-android16-5-g188c695beb6d-ab14367805-4k SMP preempt mod_unload modversions aarch64
scmversion: g188c695beb6d
```

本地构建产物 `/home/lrc/patent/kernel/kernel_platform/out/rfkill.ko`：

```text
vermagic: 6.12.23-android16-5-o-gfbffb6e59e1e-4k SMP preempt mod_unload modversions aarch64
scmversion: gfbffb6e59e1e
```

本地同时存在配套 system_dlkm 镜像：

```text
/home/lrc/patent/kernel/kernel_platform/out/system_dlkm.erofs.img
/home/lrc/patent/kernel/kernel_platform/out/system_dlkm.ext4.img
/home/lrc/patent/kernel/kernel_platform/out/system_dlkm.flatten.erofs.img
/home/lrc/patent/kernel/kernel_platform/out/system_dlkm.flatten.ext4.img
```

这说明当前失败组合是：新 boot 里的新 kernel + 设备上仍保留的原厂 system_dlkm/vendor_dlkm 模块。

## Wi-Fi 挂点

坏内核启动后，`lsmod` 中能看到 `cnss2`、`rmnet/ipam` 等模块，但没有：

```text
rfkill
cfg80211
mac80211
qca_cld3_peach_v2
```

`/sys/class/rfkill` 为空。

dmesg 中最早、最明确的 Wi-Fi 依赖链错误是：

```text
rfkill: exports protected symbol rfkill_alloc
```

该错误出现多次，例如：

```text
[    1.252192] rfkill: exports protected symbol rfkill_alloc
[    1.493000] rfkill: exports protected symbol rfkill_alloc
[   41.592491] rfkill: exports protected symbol rfkill_alloc
```

Wi-Fi HAL 上层失败：

```text
Failed to access driver state control param No such file or directory
Failed to load WiFi driver
Failed to initialize firmware mode controller
Wifi HAL start failed
Failed to create ClientInterface
```

触发 Oplus Wi-Fi 驱动重载后，系统执行：

```bash
/odm/bin/wlan_ko_insmod.sh
```

脚本内容：

```sh
#!/vendor/bin/sh
echo "oplus wlan try to insmod wlan ko again" > /dev/kmsg
rmmod qca_cld3_peach_v2
sleep 2
insmod vendor/lib/modules/qca_cld3_peach_v2.ko
```

该脚本只 insmod `qca_cld3_peach_v2.ko`，不处理 `rfkill/cfg80211/mac80211` 依赖。

随后 `qca_cld3_peach_v2.ko` 失败，dmesg 中出现大量缺失符号，例如：

```text
qca_cld3_peach_v2: Unknown symbol ieee80211_get_channel_khz
qca_cld3_peach_v2: Unknown symbol cfg80211_connect_done
qca_cld3_peach_v2: Unknown symbol cfg80211_disconnected
qca_cld3_peach_v2: Unknown symbol wiphy_register
qca_cld3_peach_v2: Unknown symbol cfg80211_scan_done
qca_cld3_peach_v2: Unknown symbol ieee80211_hdrlen
```

依赖关系：

```text
cfg80211.ko -> rfkill.ko
mac80211.ko -> libarc4.ko + cfg80211.ko + rfkill.ko
qca_cld3_peach_v2.ko -> cfg80211.ko + rfkill.ko + 其他 vendor 依赖
```

因此 Wi-Fi 的根因链是：

```text
只替换 boot.img
  -> 新 kernel 仍加载原厂 system_dlkm 中的 rfkill.ko
  -> rfkill 被 GKI protected symbol 机制拒绝
  -> cfg80211/mac80211 无法正常加载
  -> qca_cld3_peach_v2 缺 cfg80211/mac80211 符号
  -> Wi-Fi HAL 无法创建 ClientInterface
  -> Wi-Fi 不可用
```

## 运行时验证

为了验证不是内核功能本身坏掉，而是模块组合不配套，把本地构建的 `rfkill.ko` 临时推到手机：

```bash
scp -q /home/lrc/patent/kernel/kernel_platform/out/rfkill.ko lrc@192.168.61.230:/tmp/rfkill_new.ko
ssh lrc@192.168.61.230 'export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"; adb push /tmp/rfkill_new.ko /data/local/tmp/rfkill_new.ko >/dev/null; adb shell su -c "insmod /data/local/tmp/rfkill_new.ko 2>&1; echo rc=$?; lsmod | grep rfkill"'
```

结果：

```text
rc=0
rfkill                 40960  0
```

这证明本次构建产物里的 `rfkill.ko` 可以在坏内核上加载。问题不是 rfkill 逻辑被代码修改破坏，而是手机分区中原厂 `rfkill.ko` 与新 kernel 不匹配。

继续尝试加载手机现有分区里的 Wi-Fi 模块：

```text
insmod /vendor/lib/modules/cfg80211.ko
insmod /vendor/lib/modules/mac80211.ko
insmod /vendor/lib/modules/qca_cld3_peach_v2.ko
```

结果均失败：

```text
Operation not permitted
```

同时 `/vendor/lib/modules/cfg80211.ko` 的 vermagic 是另一套 vendor 模块版本：

```text
vermagic: 6.12.23-android16-5-o-g1e600dc9e153-4k SMP preempt mod_unload modversions aarch64
scmversion: g3c3cafa16878
```

这说明不能只临时补一个 `rfkill.ko` 完整修复 Wi-Fi，需要刷入/使用配套的 dlkm 模块集合。

## 蓝牙挂点

坏内核下，蓝牙 framework 进程反复崩溃：

```text
com.android.bluetooth Fatal signal 6
Abort message: '[hci_backend_aidl.cc:38] initializationComplete: status == AidlStatus::SUCCESS'
```

现场清 logcat 后重新触发蓝牙，抓到更干净的日志：

```text
bluetooth: Starting dependencies of HciHal
BluetoothHci: Using initialize_aidl.
BluetoothHci::initialize_aidl()
vendor.qti.bluetooth@1.0-data_handler: isProtocolAdded: status:1
bluetooth: [hci_backend_aidl.cc:38] initializationComplete: status == AidlStatus::SUCCESS
android.hardware.bluetooth.aidl_service: Client dead, callback initializationComplete failed
android.hardware.bluetooth.aidl_service: BluetoothHci: error INITIALIZATION_ERROR
```

当前蓝牙内核模块状态：

```text
btfmcodec
qbt_handler
```

`/dev` 下存在：

```text
/dev/bt_cp_ctrl
/dev/btfmcodec_dev
/dev/ttyHS0
```

目前没有看到蓝牙内核模块的 `Unknown symbol`，蓝牙 HAL 是在 `IBluetoothHci.initialize()` 过程中返回 `INITIALIZATION_ERROR`，framework 收到非 SUCCESS 后 abort。

由于坏内核下 `rfkill` 类设备不存在，而 rfkill 类型也覆盖 Bluetooth，蓝牙失败可能与同一套 system_dlkm/GKI 模块不匹配有关。但当前证据只能明确到：

```text
Bluetooth AIDL HAL 初始化底层 transport/protocol 失败
  -> 回调 initializationComplete 返回非 SUCCESS
  -> com.android.bluetooth abort
```

建议先修复配套 dlkm，再复测蓝牙。如果刷入配套 `system_dlkm` 后 Wi-Fi 恢复但蓝牙仍失败，再继续单独追 `android.hardware.bluetooth@aidl-service-qti` 的固件、ttyHS0、btpower、qbt_handler、HCI 初始化细节。

## 蜂窝网络状态

坏内核下，蜂窝底层驱动不像 Wi-Fi 那样完全断裂：

```text
rmnet_core
rmnet_ctl
rmnet_*
ipam
ipanetm
```

dmesg 中能看到：

```text
rmnet_ctl driver probed
ipa-wan ... dev(rmnet_data0) register to IPA
```

RIL/logcat 能读到 NR 小区信息、radio capability、modem activity：

```text
DATA_REGISTRATION_STATE ... rat: NR
GET_RADIO_CAPABILITY
GET_ACTIVITY_INFO
```

但系统当前没有默认网络：

```text
Active default network: none
mVoiceRegState=OUT_OF_SERVICE
mDataRegState=OUT_OF_SERVICE
mActiveDataSubId=-1
```

`ip -br addr` 中 rmnet 设备存在但是 DOWN：

```text
rmnet_ipa0       UNKNOWN
rmnet_data0@rmnet_ipa0 DOWN
rmnet_data1@rmnet_ipa0 DOWN
rmnet_data2@rmnet_ipa0 DOWN
```

所以移动网络问题目前更像是上层未注册/未附着、SIM/运营商状态、或者系统网络管理未建立默认网络；证据上不像 Wi-Fi 那样是明确的内核模块缺符号。优先级应低于先修复 Wi-Fi/system_dlkm 配套问题。

## 结论

最可信根因：

```text
新 boot.img 只替换了 kernel，
但手机仍使用原厂/不匹配的 system_dlkm 与 vendor_dlkm 模块。

新 kernel + 旧 dlkm 模块组合导致：
  1. rfkill.ko 被 protected symbol 机制拒绝；
  2. cfg80211/mac80211 无法加载；
  3. qca_cld3_peach_v2 缺少 Wi-Fi 栈符号；
  4. Wi-Fi HAL 无法加载驱动；
  5. 蓝牙 HAL 初始化也失败，可能受同一模块组合问题影响。
```

本次排查已经通过临时加载本地构建的 `rfkill.ko` 验证：配套构建产物能够在新内核上加载，问题不是简单的代码逻辑破坏。

## 建议修复路线

优先验证配套 `system_dlkm`：

```text
/home/lrc/patent/kernel/kernel_platform/out/system_dlkm.erofs.img
```

或根据设备分区格式选择：

```text
/home/lrc/patent/kernel/kernel_platform/out/system_dlkm.ext4.img
```

刷入前确认当前 slot 与分区名：

```bash
adb shell getprop ro.boot.slot_suffix
adb shell ls -l /dev/block/by-name/system_dlkm*
```

如果设备需要在 fastbootd 下刷动态分区，应进入 fastbootd 后刷入对应 slot 的 `system_dlkm`：

```bash
adb reboot fastboot
fastboot getvar current-slot
fastboot flash system_dlkm_a /home/lrc/patent/kernel/kernel_platform/out/system_dlkm.erofs.img
```

注意：上面分区名和镜像格式需要以设备实际情况为准，不要在未确认 slot/格式时直接执行。

刷入配套 `system_dlkm` 后复测：

```bash
adb shell su -c 'dmesg | grep -Ei "rfkill|cfg80211|mac80211|qca_cld3|Unknown symbol|protected symbol"'
adb shell su -c 'lsmod | egrep "rfkill|cfg80211|mac80211|qca_cld3"'
adb shell dumpsys wifi | head -120
adb shell cmd bluetooth_manager enable
adb logcat -d -b all | grep -Ei "BluetoothHci|IBluetoothHci|INITIALIZATION_ERROR|initializationComplete|bt_stack"
```

预期修复后：

- `rfkill` 能在启动阶段正常加载。
- `cfg80211/mac80211` 能正常加载。
- `qca_cld3_peach_v2` 不再报大量 `Unknown symbol cfg80211_*`。
- Wi-Fi HAL 能创建 ClientInterface。
- 若蓝牙仍失败，再单独追 BT HAL/firmware/transport。

## 快速定位命令

查看 Wi-Fi 模块链：

```bash
ssh lrc@192.168.61.230 'grep -Ei "rfkill|cfg80211|mac80211|qca_cld3|Unknown symbol|protected symbol" /tmp/bad_dmesg.txt | tail -260'
```

查看 Wi-Fi HAL：

```bash
ssh lrc@192.168.61.230 'grep -Ei "Failed to access driver state|Failed to load WiFi driver|Wifi HAL start failed|Failed to create ClientInterface" /tmp/bad_logcat.txt'
```

查看蓝牙 HAL：

```bash
ssh lrc@192.168.61.230 'grep -Ei "BluetoothHci|IBluetoothHci|bt_stack|initializationComplete|INITIALIZATION_ERROR|Fatal signal|Abort message" /tmp/bad_logcat.txt /tmp/bt_retry_logcat.txt'
```

查看移动网络：

```bash
ssh lrc@192.168.61.230 'export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"; adb shell dumpsys connectivity | sed -n "1,80p"; adb shell dumpsys telephony.registry | grep -Ei "mServiceState|mDataConnectionState|mActiveDataSubId"'
```
