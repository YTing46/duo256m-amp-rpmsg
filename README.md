# duo256m-mainline-amp

Milk-V Duo256M（SG2002，雙 C906 AMP）的異質多核通訊機制移植：
把 vendor 5.10 SDK 的 ThreadX + RPMsg + remoteproc 堆疊搬上 **mainline Linux 7.0-rc6**，
修掉一路上的通訊 bug 與一個 mainline 級的 clock 根因，並以量化數據對照 vendor 基線。

本專題復刻並延伸 [HenryChaing 的「Linux 核心專題: 異質多核通訊機制」](https://hackmd.io/@sysprog/linux2025-projects)。

本 repo 同時收錄 vendor 5.10 對照組：原本分散在
[jeremylu0830/milkv_sdk](https://github.com/jeremylu0830/milkv_sdk)（SDK 修改）與
[jeremylu0830/duo-example-rtt](https://github.com/jeremylu0830/duo-example-rtt)（userspace 量測工具）
的內容已併入 `patches/vendor-sdk/`、`bench/vendor-5.10/`，commit 歷史保留原作者署名。
說明見 `docs/vendor-5.10-baseline.md`。

## 成果總覽

| 項目 | 狀態 |
|---|---|
| mainline 7.0-rc6 在 Duo256M 開機（SD 卡、FIT image） | ✅ |
| USB gadget ADB（免序列線工作流） | ✅ |
| RPMsg host↔C906L pingpong 穩定通訊 | ✅（修掉 3 個 bug） |
| upstream C906L remoteproc driver（Junhui Liu v2）真機打通 | ✅（+3 個 review 修正） |
| SEC_SYS clock 根因（mainline 級 bug）+ 正式修法 | ✅ |
| RTT 分解量測 + payload sweep 對照 vendor | ✅ |
| **RPMsg 100% upstream 化**（remoteproc vdev + cv1800-mailbox，glue 退役） | ✅（300/300，**127.6µs**——反超手工 glue） |
| mainline bug ×4：clk_efuse 依賴、mailbox EN 覆寫、rproc_virtio dma_coherent、mailbox txpoll hrtimer 風暴 | ✅ 皆附修法 |
| vendor 5.10 對照組：SDK patch series（套在 v2.0.1）+ RTT 分解數據 | ✅（Jeremy Lu 量測，10k 筆） |

### 延遲數據（RTT p50）

| 平台 | 4B | 480–496B | payload 斜率 |
|---|---|---|---|
| vendor 5.10（Jeremy Lu，decomp5，**32B**） | 114.0µs | 131.8µs | +17.9µs |
| mainline 7.0 glue 優化版（cached buffers + RT kthread） | 152.0µs | 162.1µs | +10.1µs |
| mainline 7.0 全 upstream（修正前） | 176.4µs | — | — |
| **mainline 7.0 全 upstream（終態：txdone-by-ACK + hard-IRQ 直送）** | **127.6µs** | 139.2µs | +11.6µs |

註：vendor 5.10 的數字全部來自 Jeremy Lu 的量測（[HackMD](https://hackmd.io/ei4rzk-OQfOreOa5fG2eKQ)，
decomp4 與素身 bench 的原始數據在 `bench/data/vendor-5.10/`）。帶分解探針的 RTT p50 隨探針組與量測輪次
落在 105.8–130.4µs：decomp4（8B）105.8、decomp5 payload sweep（32B）114.0、decomp5 主輪 130.4；
不帶探針的素身 bench（4B）為 75.3µs。mainline 終態的 127.6µs 也是帶探針的數字，
比 decomp5 32B 慢 13.6µs、比 decomp4 慢 21.8µs。逐段對照見 `docs/vendor-5.10-baseline.md`。

全 upstream 架構最初比手工 glue 慢 24µs，逐段分解後定位出兩個可修的
mainline 框架成本（mailbox poll hrtimer 空轉、threaded IRQ 排程來回），
修正後**反超手工 glue 逾 20µs**，落在 vendor 5.10 帶探針量測的範圍內（見上註）；
p99 尾巴 301 → 178µs（殘餘尾巴經機率驗證為 kernel timer tick，非 bug）。
大 payload 斜率仍優於 vendor（cached buffers + streaming DMA sync 的效果）。
原始分解數據見 `bench/data/native-*.csv`（修正前 / ACK / hard-IRQ 三組），
完整分析見 `docs/linux7-migration.md` 與 `docs/rpmsg-native-design.md`。

## 修掉的關鍵 bug

1. **rpmsg 通知遺失（host 端，兩處）**：`rtos_cmdqu_handler` 用單一全域
   `rtos_cmd_id` + `schedule_delayed_work` 合併背靠背通知，只服務最後一個
   virtqueue → 改為輪詢全部 vq；mailbox ISR 的 `set_val` 只讀一次，漏接
   ISR 進行中抵達的訊息 → 改為 do/while 重讀排空。
2. **`dma-noncoherent` 缺失**：rpmsg 節點在 DT root、不在 `/soc` 之下，
   繼承不到 `dma-noncoherent` → virtio buffer 變 cacheable，C906B 的寫入
   滯留 D-cache，無 cache 的 C906L 讀到舊資料 → 時序性 1~數回合 stall。
3. **SEC_SYS clock 根因（本專題最重要的發現）**：SEC_SYS（0x020B0000，
   C906L 開機控制暫存器所在）的匯流排存取需要 `clk_efuse`（CLK_EN_0 bit 11）。
   mainline 的 `clk_disable_unused` 在開機尾聲把它關掉，之後**任何**對該區塊的
   load/store 都會懸死整條 AXI 匯流排（連 MMC 陪葬）——一行
   `devmem 0x020B0004` 即可重現。vendor 從未把這條依賴寫進 device tree
   （藏在 `cvitek-spacc.c` 的手動 `clk_prepare_enable` 裡）。
   修法：sec_sys syscon 節點加 `clocks = <&clk CLK_EFUSE>`，
   syscon 把 clock attach 到 regmap-mmio，每次暫存器存取 just-in-time 開關。
   詳見 `patches/linux/0013`。

upstream driver 本身另有三個修正（`__iomem` cast、binding `additionalItems`、
devm_rproc_add 與手動 rproc_del 的 double-delete），見 `patches/linux/0011`。

## Repo 結構

```
patches/linux/    24 個 patch：v7.0-rc6 → 完整可跑的 100% upstream 架構 AMP 系統
                  （0009/0010 為 Junhui Liu 的 v2 系列，保留原作者署名）
patches/threadx/  C906L ThreadX 韌體修改（7 個 patch）（基於 HenryChaing/ThreadX-to-RISC-V64 amp @ 49ad38f）
patches/vendor-sdk/
                  vendor 5.10 對照組：11 個 patch，套在 duo-buildroot-sdk-v2 v2.0.1
                  （0001–0007 HenryChaing：ADB/ThreadX/remoteproc/RPMsg glue；
                    0008–0011 Jeremy Lu：RTT 探針）
bench/            RTT 量測工具（總量/分解/payload sweep）+ CSV 數據
bench/vendor-5.10/
                  vendor 5.10 userspace 工具（pingpong、LED、RTT bench、decomp4、decomp4_stamp、rtt.py）
bench/data/vendor-5.10/
                  vendor 5.10 數據（Jeremy Lu，10k 筆 CSV ×2 + 分解圖）
board/            板端開機鏈 scripts（S97adb/S98cvirtos/S99user、usb-adb.sh、run_usb.sh）、
                  boot.its（FIT）、驗證過的 kernel .config
scripts/          build-image.sh（建置 SD 映像）、run-board-tests.sh（上板測試）
docs/             工程筆記（含完整除錯歷程與翻案記錄）
```

## 重現步驟

### 一鍵建置 SD 映像（不需要 root）

```sh
sudo apt install -y git curl unzip build-essential flex bison bc xz-utils u-boot-tools \
    mtools e2fsprogs fdisk cmake ninja-build python3 kmod openssh-client \
    patch perl rsync wget cpio file adb
scripts/build-image.sh
```

腳本依序執行 `fetch kernel rtos bench adbd image`，也可以只跑其中幾步（例如 `scripts/build-image.sh kernel image`）：

| 步驟 | 內容 |
|---|---|
| `fetch` | 下載 v7.0-rc6 並套 `patches/linux`、ThreadX `amp`（`49ad38f`）並套 `patches/threadx`、milkv host-tools 工具鏈（約 7 GB）、SDK v2.0.1 的 `build/`（只用來產生小核記憶體配置）、官方 v2.0.1 SD 映像（驗 sha256），並產生板子用的 SSH 金鑰 |
| `kernel` | 用 `board/linux7x-boot-verified.config` 編 Image、DTB、模組，依 `board/boot.its` 打包 `boot.sd`（載入位址 0x82000000） |
| `rtos` | 編 C906L 韌體 `cvirtos.elf` |
| `bench` | 以 static musl、`-march=rv64imafdc` 編 `bench/` 的 4 支工具 |
| `adbd` | 用 buildroot 2024.02.13（驗 sha256）的 android-tools 編 static、`rv64imafdc` 的 adbd，與 SDK 內建的是同一份 4.2.2 原始碼 |
| `image` | 以官方映像為底：換掉第一分割區的 `boot.sd`；rootfs 加入模組、`/lib/firmware/cvirtos.elf`、`board/S98cvirtos`、adbd、`board/run_usb.sh`、`board/usb-adb.sh`（`usb.sh` 依 `DUO_USB` 指向 ADB 或 NCM）、`/root/bench/`、root 的 SSH 公鑰 |

產出在 `build/out/duo256m-mainline-<kernel release>.img`（附 `.sha256` 與 `manifest.txt`）。
可用環境變數調整：`DUO_BUILD_DIR`（工作目錄，預設 `build/`）、`DUO_HOST_TOOLS`（沿用既有的 host-tools）、
`DUO_SSH_PUBKEY`（改用自己的公鑰）、`JOBS`。

注意：
- userspace 工具不能用 SDK 預設的 `-march=...v0p7xthead`：mainline 沒開 C906 的 T-Head vector 0.7，會 SIGILL。
- 開機時的 USB 由 `DUO_USB` 決定：`adb`（預設，與 2026-07 的工作流程相同，`adb shell`／`adb push`）
  或 `ncm`（USB 網路，板端 192.168.42.1，dnsmasq 用 DHCP 配位址給主機，dropbear SSH）。
  官方映像本身沒有 adbd，由 `adbd` 步驟補上。NCM＋ADB 複合 gadget 能列舉，但 NCM 從板子送往主機的
  封包到不了，原因未解，所以二選一。
- `board/linux7x-boot-verified.config` 是 2026-07-15 那顆實際開機的 kernel 內嵌的 config（`/proc/config.gz`
  同源）：USB gadget 整組編進 kernel（`=y`），adbd 用的 functionfs 開機就在，不需要載模組。

### 燒卡與上板測試

```sh
# 確認讀卡機的裝置路徑（用 by-id 才不會寫錯磁碟），整張卡會被覆寫
ls -l /dev/disk/by-id/ | grep usb
sudo dd if=build/out/duo256m-mainline-<release>.img of=/dev/disk/by-id/<讀卡機> bs=4M conv=fsync status=progress

# 卡插上板子、USB-C 接主機開機，約 30 秒後：
scripts/run-board-tests.sh
```

`run-board-tests.sh` 經 ADB（偵測到裝置時）或 SSH 檢查 remoteproc 與 dmesg、跑 pingpong（含韌體 stop/start 後再跑一次）、
RTT、分段量測與 payload sweep，結果存在 `build/results/<時間>/`，全部通過才回傳 0。
量測輸出先寫在板子的 `/tmp` 再取回，避免 USB 傳輸干擾延遲尾巴。
2026-10-20 的實測：pass 8/8，分段 RTT p50 127.3µs / p99 182µs，payload 4→480B 128.9→138.9µs。

vendor 5.10 對照組：

```sh
git -C duo-buildroot-sdk-v2 checkout v2.0.1
git -C duo-buildroot-sdk-v2 am "$PWD"/patches/vendor-sdk/*.patch
make -C bench/vendor-5.10        # 需先 source duo-examples 的 envsetup.sh
```

ThreadX submodule 的兩個 commit 未公開；分段量測改用 `amp`（`49ad38f`）加
`patches/threadx/0001–0003` 的韌體，搭配 `bench/vendor-5.10/rpmsg_rtt_decomp4_stamp`
（`0004–0007` 不適用 vendor kernel）。細節見 `docs/vendor-5.10-baseline.md`。

序列埠：板端 console 115200（PL2303 注意 termios2 BOTHER）；C906L console 124000。
注意這塊板的 warm reboot（`reboot` 指令）會卡在 FSBL，要斷電重上。

## 來源與貢獻者

| repo | 作者 | 併入位置 |
|---|---|---|
| [YTing46/duo256m-mainline-amp](https://github.com/YTing46/duo256m-mainline-amp)（主線） | YTing46 | 全部 mainline 內容 |
| [jeremylu0830/milkv_sdk](https://github.com/jeremylu0830/milkv_sdk) | HenryChaing、Jeremy Lu | `patches/vendor-sdk/` |
| [jeremylu0830/duo-example-rtt](https://github.com/jeremylu0830/duo-example-rtt) | HenryChaing、Jeremy Lu | `bench/vendor-5.10/`、`bench/data/vendor-5.10/` |

## 參考

- [HenryChaing — Linux 核心專題: 異質多核通訊機制](https://hackmd.io/@sysprog/linux2025-projects)（vendor 5.10 原始專題）
- [Jeremy Lu — vendor 5.10 RPMsg RTT 量測與分解](https://hackmd.io/ei4rzk-OQfOreOa5fG2eKQ)（decomp4／decomp5，vendor 5.10 延遲數據的出處）
- [Junhui Liu — remoteproc: Add C906L controller for Sophgo CV1800B SoC (v2)](https://lore.kernel.org/linux-remoteproc/)（upstream driver 素材）
- [milkv-duo/duo-buildroot-sdk-v2 v2.0.1](https://github.com/milkv-duo/duo-buildroot-sdk-v2/releases/tag/v2.0.1)（vendor 參考基線：FSBL、5.10 驅動對照）
- [NCKU jserv Linux 核心設計課程](https://wiki.csie.ncku.edu.tw/linux/schedule)
