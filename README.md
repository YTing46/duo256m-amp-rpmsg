# duo256m-amp-rpmsg

Milk-V Duo256M（SG2002）在 mainline Linux 7.0-rc6 上的非對稱多核（AMP）通訊實作與延遲量測。

## 專案概述

SG2002 內含兩顆 RISC-V C906 核心。本專案由大核執行 Linux、小核執行 ThreadX，兩者以 RPMsg 交換訊息。

此架構最初由 HenryChaing 於[成功大學 Linux 核心設計課程專題](https://hackmd.io/@sysprog/linux2025-projects)中，
以原廠 Linux 5.10 SDK 實作。本專案將其移植至 mainline Linux 7.0-rc6，修正移植過程中發現的問題，
最終使 RPMsg 完全採用 upstream 驅動程式，並量測通訊延遲、與原廠版本比較。

本 repo 整合以下三個專案：

| 來源 | 內容 | 位置 |
|---|---|---|
| [YTing46/duo256m-mainline-amp](https://github.com/YTing46/duo256m-mainline-amp) | mainline 移植 | `patches/linux`、`patches/threadx`、`bench/`、`board/`、`docs/`、`lore/` |
| [jeremylu0830/milkv_sdk](https://github.com/jeremylu0830/milkv_sdk) | 原廠 SDK 的修改 | `patches/vendor-sdk/` |
| [jeremylu0830/duo-example-rtt](https://github.com/jeremylu0830/duo-example-rtt) | 原廠環境的量測工具與數據 | `bench/vendor-5.10/`、`bench/data/vendor-5.10/` |

所有 commit 均保留原作者資訊。

## 成果

| 項目 | 結果 |
|---|---|
| mainline 7.0-rc6 自 SD 卡開機 | 完成 |
| 經 USB 以 ADB 操作開發板，無須序列埠 | 完成 |
| Linux 與小核之間的 pingpong 通訊 | 穩定，連續 1000 次全數成功 |
| 小核啟停改用 upstream remoteproc 驅動（Junhui Liu v2 系列） | 完成，另修正 3 個問題 |
| RPMsg 完全改用 upstream 驅動，不再使用原廠驅動 | 完成 |
| 延遲量測，含分段拆解與不同訊息長度 | 完成，並與原廠版本對照 |

## 延遲量測

量測範圍為 Linux 送出一則訊息、小核原樣回傳、Linux 讀回為止的來回時間（RTT），下表為中位數：

| 版本 | 短訊息 | 480–496 位元組 | 訊息長度增加造成的差值 |
|---|---|---|---|
| 原廠 Linux 5.10（Jeremy Lu 量測，32 位元組） | 114.0µs | 131.8µs | 17.9µs |
| mainline，沿用原廠驅動並最佳化 | 152.0µs | 162.1µs | 10.1µs |
| mainline，全面改用 upstream 驅動（修正前） | 176.4µs | — | — |
| mainline，全面改用 upstream 驅動（最終版本） | 127.6µs | 139.2µs | 11.6µs |

mainline 各列的短訊息均為 4 位元組。

全面改用 upstream 驅動後，延遲起初比沿用原廠驅動的版本高 24µs。經分段拆解，定位出兩項框架層的額外成本：
mailbox 驅動在閒置時持續輪詢，以及收訊路徑多出一次執行緒排程。兩項修正後，延遲較沿用原廠驅動的版本低 20µs 以上；
p99 由 301µs 降至 178µs，其餘的長尾延遲經分析來自 kernel 定時器中斷。

與原廠版本比較時的注意事項：

- 原廠數據均由 Jeremy Lu 量測，量測過程記錄於其 [HackMD 筆記](https://hackmd.io/ei4rzk-OQfOreOa5fG2eKQ)，
  部分原始數據位於 `bench/data/vendor-5.10/`。
- 量測探針會影響結果。原廠版本在不同探針配置下的 RTT 中位數介於 105.8 至 130.4µs，未加探針時為 75.3µs。
- mainline 的 127.6µs 同樣含探針，比原廠 32 位元組的 114.0µs 高 13.6µs，比原廠 8 位元組的 105.8µs 高 21.8µs。
- mainline 隨訊息長度增加的延遲較小，原因是共享記憶體改為可快取，並由驅動明確執行快取同步。

分段拆解的細節見 `docs/vendor-5.10-baseline.md`、`docs/linux7-migration.md` 與 `docs/rpmsg-native-design.md`，
原始數據位於 `bench/data/`。

## 修正的問題

1. **Linux 端遺失通知（兩處）。** 原廠驅動以單一全域變數記錄通知，連續兩則通知僅會處理最後一則；
   mailbox 中斷處理程序僅讀取一次狀態，處理期間到達的訊息因此遺漏。修正後改為每次檢查所有佇列，
   並重複讀取至無新訊息為止。
2. **共享記憶體的快取屬性錯誤。** RPMsg 裝置節點位於 device tree 根節點，未繼承 `dma-noncoherent` 屬性，
   導致大核寫入的資料停留在其快取中，無快取的小核讀到舊資料，通訊數回合後即中斷。
3. **SEC_SYS 時脈依賴（本專案最主要的發現）。** 小核的啟動控制暫存器位於 SEC_SYS（0x020B0000），
   存取該區塊需要 `clk_efuse` 時脈。mainline 在開機末段會關閉未被任何驅動登記的時脈，
   此後任何對該區塊的存取都會使整條匯流排停止回應，SD 卡亦隨之失效；以 `devmem 0x020B0004` 即可重現。
   原廠 SDK 並未在 device tree 中描述此依賴，而是在加密驅動中手動開關時脈。
   修正方式為在 sec_sys 節點加入 `clocks = <&clk CLK_EFUSE>`，由 syscon 在每次存取暫存器時自動開關時脈，
   見 `patches/linux/0013`。

upstream 的 C906L 驅動另修正三項問題（`__iomem` 轉型、binding 的 `additionalItems`、rproc 重複刪除），
見 `patches/linux/0011`。

本專案共發現 4 個 mainline 本身的問題，均附修正：`clk_efuse` 的時脈依賴、mailbox 啟用暫存器遭整體覆寫、
`rproc_virtio` 未繼承 DMA 一致性設定、mailbox 輪詢計時器在閒置時持續觸發。

## 目錄結構

```
patches/linux/           24 個 patch，將 v7.0-rc6 建構為完整可運作的系統
                         （0009、0010 為 Junhui Liu 的 v2 系列，保留原作者）
patches/threadx/         小核 ThreadX 韌體的 7 個 patch
                         （基於 HenryChaing/ThreadX-to-RISC-V64 amp 分支 49ad38f）
patches/vendor-sdk/      套用於原廠 SDK v2.0.1 的 11 個 patch
                         （0001–0007：HenryChaing 的 ADB、ThreadX、remoteproc、RPMsg；
                           0008–0011：Jeremy Lu 的延遲量測探針）
bench/                   mainline 量測工具與數據
bench/vendor-5.10/       原廠環境量測工具
bench/data/vendor-5.10/  原廠環境量測數據（Jeremy Lu，各 10000 筆）
board/                   開發板開機腳本、FIT 映像設定、kernel 設定檔
scripts/                 SD 映像建置與上板測試腳本
docs/                    工程筆記與除錯紀錄
lore/                    upstream patch 審查回覆草稿
```

## 建置與重現

### 建置 SD 映像

建置過程不需要 root 權限：

```sh
sudo apt install -y git curl unzip build-essential flex bison bc xz-utils u-boot-tools \
    mtools e2fsprogs fdisk cmake ninja-build python3 kmod openssh-client \
    patch perl rsync wget cpio file adb
scripts/build-image.sh
```

腳本會自動下載所需的原始碼與工具鏈（工具鏈約 7 GB），產出的映像位於 `build/out/duo256m-mainline-<版本>.img`。
建置分為下列步驟，可單獨執行其中幾項，例如 `scripts/build-image.sh kernel image`：

| 步驟 | 內容 |
|---|---|
| `fetch` | 下載 kernel、ThreadX、工具鏈、原廠 SDK 建置腳本與官方 SD 映像，套用 patch，並產生 SSH 金鑰 |
| `kernel` | 編譯 kernel 並封裝為 `boot.sd` |
| `rtos` | 編譯小核韌體 `cvirtos.elf` |
| `bench` | 編譯量測工具 |
| `adbd` | 自原始碼編譯 adbd |
| `image` | 以官方映像為基礎，加入上述產出 |

可透過環境變數調整：`DUO_BUILD_DIR`（工作目錄）、`DUO_HOST_TOOLS`（沿用已下載的工具鏈）、
`DUO_SSH_PUBKEY`（指定公鑰）、`DUO_USB`（USB 模式，見注意事項）。

### 燒錄與測試

```sh
# 確認讀卡機的裝置路徑；整張卡的內容將被覆寫
ls -l /dev/disk/by-id/ | grep usb
sudo dd if=build/out/duo256m-mainline-<版本>.img of=/dev/disk/by-id/<讀卡機> bs=4M conv=fsync status=progress
```

將 SD 卡裝回開發板並以 USB-C 供電開機，約 30 秒後執行：

```sh
scripts/run-board-tests.sh
```

測試項目包括小核啟動狀態、pingpong（含小核重啟後的再次測試）與各項延遲量測，結果存放於 `build/results/`，
全部通過時回傳 0。2026-10-20 的測試 8 項全數通過，RTT 中位數介於 124 至 129µs。

### 注意事項

- 開機時僅能啟用一種 USB 功能，由 `DUO_USB` 指定：預設 `adb`；或 `ncm`，以 USB 網路透過 SSH 連線至 192.168.42.1。
  同時啟用兩者時裝置可被偵測，但網路僅能單向傳輸，原因尚未查明。
- 官方映像不含 adbd，由 `adbd` 步驟另行編譯加入。
- 開發板上的程式須以 `-march=rv64imafdc` 編譯。原廠 SDK 預設啟用 T-Head 向量指令，
  mainline 不支援該指令集，執行時會觸發非法指令例外（SIGILL）。
- `board/linux7x-boot-verified.config` 取自 2026-07-15 實際開機的 kernel，USB gadget 直接編入 kernel，無須載入模組。
- `reboot` 指令無法重新啟動開發板，須斷電後重新上電。
- 序列埠主控台速率為 115200 baud，小核主控台為 124000 baud。使用 PL2303 相容晶片的轉接線時，可能需以 termios2 的 BOTHER 設定速率。

### 原廠 5.10 對照組

```sh
git -C duo-buildroot-sdk-v2 checkout v2.0.1
git -C duo-buildroot-sdk-v2 am "$PWD"/patches/vendor-sdk/*.patch
make -C bench/vendor-5.10        # 須先 source duo-examples 的 envsetup.sh
```

如需分段量測，可改用 ThreadX `amp` 分支（49ad38f）加上
`patches/threadx/0001–0003`，搭配 `bench/vendor-5.10/rpmsg_rtt_decomp4_stamp`。
`0004–0007` 不適用於原廠 kernel，原因與細節見 `docs/vendor-5.10-baseline.md`。

## 參考資料

- [HenryChaing — Linux 核心專題：異質多核通訊機制](https://hackmd.io/@sysprog/linux2025-projects)
- [Jeremy Lu — 原廠 5.10 的 RPMsg 延遲量測與拆解](https://hackmd.io/ei4rzk-OQfOreOa5fG2eKQ)
- [Junhui Liu — remoteproc: Add C906L controller for Sophgo CV1800B SoC (v2)](https://lore.kernel.org/linux-remoteproc/)
- [milkv-duo/duo-buildroot-sdk-v2 v2.0.1](https://github.com/milkv-duo/duo-buildroot-sdk-v2/releases/tag/v2.0.1)
- [成功大學 Linux 核心設計課程](https://wiki.csie.ncku.edu.tw/linux/schedule)
