# My_IAP_F411 — STM32F411 IAP BootLoader

> STM32F411CEU6 的 **In-Application Programming（IAP）引导程序**，基于 ST 官方 Ymodem IAP 模板改造。
> 负责接收新固件、写入 Flash、校验合法性，并把控制权交给 App —— 是手表 OTA 升级链路的地基。

---

## 一、Flash 分区

与 App 工程 [My_Watch](../My_Watch) 共用同一片 512KB Flash：

| 区域 | 起始地址 | 大小 | 用途 |
|---|---|---|---|
| **BootLoader（本工程）** | `0x08000000` | 32 KB | 引导 + Ymodem 收发 + 跳转 |
| APP FLAG 标志区 | `0x08008000` | 16 KB | `"APP FLAG"`（8 字节魔数）+ 跨复位标志 |
| App 固件 | `0x0800C000` | 464 KB | 手表主固件（`APPLICATION_ADDRESS`） |

- `APPLICATION_ADDRESS` 定义于 `Ymodem/flash_if.h`
- 写入范围由 `USER_FLASH_SIZE = USER_FLASH_END_ADDRESS - APPLICATION_ADDRESS + 1` 约束

> ⚠️ 两个工程的地址必须成对配置：App 工程 IROM1 起始 = `0x0800C000`。

---

## 二、启动流程

上电后（`Core/Src/main.c`）：

```
上电
 ├─ KEY1 是否按下？（延时 500ms 消抖确认）
 │    ├─ 是 → LCD 显示 "Bootload / OV-Watch V2.4.1" → 进入 Main_Menu() 等待升级
 │    └─ 否 → 读取 0x08008000 处 8 字节
 │            ├─ == "APP FLAG" → 直接跳转 App
 │            └─ != "APP FLAG" → 兜底校验 App 向量表：
 │                  SP ∈ [0x20000000, 0x2001FFFF]  且
 │                  PC ∈ [0x0800C000, 0x0807FFFF]
 │                    ├─ 合法 → 跳转 App
 │                    └─ 非法 → LCD 显示 "No App! Please Download"，停在 while(1)
```

**两级判定设计的意义**：`APP FLAG` 是升级完成后的显式凭证；即使标志丢失（首次烧录 / Flash 擦除），只要能读到合法的向量表就仍然可启动，避免设备"变砖"。

---

## 三、主菜单功能

| 选项 | 函数 | 说明 |
|---|---|---|
| Download | `SerialDownload()` | 通过串口 **Ymodem 协议**接收 `.bin` 写入 App 区，完成后自动写 `APP FLAG` |
| Upload | `SerialUpload()` | 把 App 区内容以 Ymodem 方式回传（用于备份校验） |
| Execute（3） | `Execute_New_Program()` | 跳转到 App 运行 |

> 菜单头保留 ST 官方模板字样（STM32F4xx IAP Application v1.0.0）；LCD 上显示的 `OV-Watch V2.4.1` 为手表固件线版本标识。

下载过程中的关键保护：

- **空间检查**：镜像超出 App 区容量时拒绝写入并报 `The image size is higher than the allowed space memory!`
- **写入校验**：Flash 写入后逐字节比对，失败则报 `Verification failed!`
- **完成标记**：校验通过才写 `APP FLAG`，并提示 `Back to Main Menu, press 3 to run App...`

---

## 四、BootLoader → App 跳转

跳转前必须把 BootLoader 用过的资源清理干净（`Execute_New_Program()`）：

1. **校验目标向量表**：SP 落在 RAM 区（`0x2000_xxxx`）、PC 落在 Flash 区（`0x0800_xxxx`），防止跳转到空区域
2. 关闭 BootLoader 使用过的外设（BLE 模块等），避免 App 启动窗口收到脏字节
3. **排干通信 FIFO + 清错误位**（读 `SR`/`DR` 清 `ORE/FE/PE/NE`），不清则 `RXNE` 永不触发
4. `__disable_irq()` + 停 SysTick
5. 清 NVIC 挂起（ICPR）+ 清 Flash 缓存（ICRST/DCRST）
6. 切 MSP + 清 PSP → `__set_MSP(app_sp)` → 跳 `app_pc`

> 对应地，App 入口侧有 `BootLoaderJumpedPeriphCleanup()`（在 `HAL_Init()` 之前执行）做兜底清理——热跳转不复位，BootLoader 遗留的外设状态必须由 App 自己收拾。

### 与 App 的双向通路

| 方向 | 触发 | 机制 |
|---|---|---|
| App → BL | BLE 下发 `OV+UPGRADE` | 热跳转，写 `BKP1R = 0xB007` 标志（**不可用 `NVIC_SystemReset()`**，会导致 PMIC 断电黑屏） |
| BL → App | 升级完成选 3 / 上电自动 | 向量表校验 + 外设清理 + MSP 切换后跳转 |

---

## 五、目录结构

```
My_IAP_F411/
├── Core/
│   ├── Inc/                # main.h / gpio.h / spi.h / usart.h / tim.h ...
│   └── Src/                # main.c（启动流程 + 跳转）/ usart.c / spi.c ...
├── Ymodem/                 # ★ IAP 核心
│   ├── ymodem.c / .h       # Ymodem 收发包实现
│   ├── flash_if.c / .h     # Flash 擦写 + APPLICATION_ADDRESS 定义
│   ├── common.c / .h       # 串口收发与 Flash 操作公共封装
│   └── menu.c / .h         # 主菜单：Download / Upload / Execute + 跳转
├── BSP/                    # 板级驱动：KEY / KT6328(BLE) / LCD / POWER
├── SYSTEM/                 # delay / sys
├── Drivers/                # CMSIS + STM32F4xx HAL
├── MDK-ARM/                # Keil 工程（IAP_F411.uvprojx）
├── IAP_F411.ioc            # CubeMX 工程
└── KeilClear.bat           # 清理 Keil 编译产物
```

---

## 六、编译与烧录

1. **环境**：Keil MDK-ARM + STM32F4xx Device Pack
2. 打开 `MDK-ARM/IAP_F411.uvprojx`
3. 确认 IROM1 起始地址为 `0x08000000`，大小 32KB（`0x8000`）
4. 编译下载

---

## 七、升级操作

### 方式 A：串口 Ymodem（SecureCRT / Xshell）

1. 长按 KEY1 上电，进入 BootLoader 菜单
2. 串口选择菜单项 **1（Download）**，终端出现 `Waiting for the file to be sent ...`
3. 用终端软件的 **Ymodem 发送**功能选择 App 的 `.bin` 文件
4. 等待 `Programming Completed Successfully!` → `APP Flag Set Successfully!`
5. 菜单选 **3（Execute）** 运行新固件，或直接断电重启

### 方式 B：BLE 触发（配合小程序）

1. App 运行中，小程序下发 `OV+UPGRADE`
2. 固件热跳转到 BootLoader 并置 `BKP1R = 0xB007`
3. 后续升级流程同上

---

## 八、相关工程

| 工程 | 说明 |
|---|---|
| [My_Watch](../My_Watch) | 手表 App 固件（FreeRTOS + LVGL + BLE） |
| [WatchApp_MiniProgram](../WatchApp_MiniProgram) | 微信小程序控制中心（BLE 指令 + OTA 触发） |

---

## 九、致谢

- Ymodem 协议实现改编自 ST 官方 `STM32F4xx In-Application Programming` 例程（MCD Application Team）
