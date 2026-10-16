# LISTENAI QEMU 维护约定

`listenai` 分支维护 LISTENAI 芯片及板型模型，基于 QEMU 10.1.0。
上游项目说明和许可证保持有效；新增代码沿用相应组件的许可证。

## 代码边界

- `target/riscv/nuclei_n300.*`：N300 指令、CSR、ECLIC 与异常入口。
- `accel/tcg/`：指令计数、每核时钟、双核轮转与实时节拍。
- `hw/riscv/arcs/`、`hw/riscv/venusa/` 及对应头文件目录：芯片地址图、
  中断与时钟接线、板级组合和宿主控制接口。每个芯片目录的 `meson.build`
  汇总其构建依赖；不同寄存器代际保持独立模型。
- `hw/char/`、`hw/dma/`、`hw/gpio/`、`hw/i2c/`、`hw/mem/`、`hw/rtc/`、
  `hw/ssi/`、`hw/timer/`、`hw/watchdog/` 中的 ListenAI 模型承载共用 IP。
  芯片适配层传入地址空间、IRQ、DMA 请求、时钟与差异参数，不反向依赖板型。
- `hw/core/listenai-board*`、`listenai-camera-scene*`：检查后的板级装配、
  GPIO/扩展器端点和宿主光学场景通道，不依赖 SoC 类型，不编程 PinMux。
  具体芯片适配层提供控制器、焊盘路由、时钟和 IRQ。独立器件按 qdev 类型
  注册；固定 EXTIO 1.3 协议使用 `listenai-extio-ch32v003-v1.3`。
  HUSB320-BA 接收实际虚拟主机的连接边沿，在虚拟时钟上完成 CC/VBUS
  去抖；USB 控制器及 TCP 适配层不生成器件的寄存器应答。
- `hw/display/`、`hw/i2c/`、`hw/audio/`：显示、传感器、触控及音频通道。
  芯片寄存器、独立器件和宿主传输保持分层，不将固件业务放入模型。
- 共用控制器和器件的接口放在对应的 `include/hw/` 目录，公共头不依赖
  ARCS 或 VenusA 的 SoC 类型。独立 MMIO 控制器按 qdev/SysBusDevice
  管理子总线、IRQ、GPIO、realize 和 reset；SoC 负责实例化、地址映射与接线。
  共用串行算法可以作为内部组件，不为辅助状态机虚构独立硬件设备。
- `hw/block/listenai_nor.c` 与 `hw/sd/listenai_sd.c`：共享 NOR 命令引擎与
  SDHCI/vendor 挂接；板型传入地址、IRQ、介质和 CD 接线，控制器复位不
  清除 NOR 内容，SD 的介质状态与机械检测信号保持分离。
- `hw/misc/listenai_luna.c` 通过固定 LUNA ABI 提供共享 MMIO 与完成定时，
  芯片只接入地址、复位、时钟与诊断；不解析组件参数或实现算子。
- ROM 和 LUNA 二进制由构建方提供，不提交 SDK、业务固件或私有组件源码。
  `scripts/lisem-assets.py` 校验目标、ABI、大小及哈希，在构建目录生成头文件。

## 构建与验证

共同构建使用 `--target-list=riscv32-softmmu`、`--without-default-devices`、
`--with-devices-riscv32=listenai`，并指定三个绝对路径选项：
`-Dlisem_rom_dir=...` 指向含 AP/CP ROM 与清单的目录；
`-Dlisem_venusa_rom_dir=...` 指向含 VenusA AP ROM 与清单的目录；
`-Dlisem_luna_dir=...` 指向当前宿主目标的静态库及清单目录。
仅构建一个芯片时可选择 `--with-devices-riscv32=arcs` 或 `venusa`，
并仅提供相应的 ROM 目录及 LUNA 组件。
Lisem 负责产品打包和完整固件回归，本仓库保留 QEMU 原生构建入口。
不需要初始化 `roms/` 的上游固件子模块。

变更先运行相关模型测试，再执行 Lisem 的芯片和原始固件回归。
时钟/JIT 优化须保持指令预算、异常、IRQ 和 GDB 行为，性能比较使用同一输入
并验证结果一致。寄存器语义依据芯片资料及真机证据；未实现模式明确报错，
不得通过修改固件、假成功、跳过中断或修改虚拟时间绕过缺口。
宿主音视频、网络和 USB 只传递实际外设数据，使用有界缓冲和取消机制。
ROM 固定原字节；LUNA 通过 `include/lisem/luna.h` 调用，不解析内部算子。

## 交付

维护文档使用中文，代码、诊断和提交信息使用英文。文档只描述当前行为、
维护约束和必要理由，不记录开发过程或私人路径。
按完整逻辑组织 Conventional Commits，正文每行不超过 75 字符，附实际模型
的 `Co-authored-by`。提交前核对维护者身份；推送须获得明确授权，
不重写已推送历史。不得以本地构建通过代替跨平台及原固件验证。
