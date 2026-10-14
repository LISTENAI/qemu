# LISTENAI QEMU 维护约定

`listenai` 分支维护 LISTENAI 芯片及板型模型，基于 QEMU 10.1.0。
上游项目说明和许可证保持有效；新增代码沿用相应组件的许可证。

## 代码边界

- `target/riscv/nuclei_n300.*`：N300 指令、CSR、ECLIC 与异常入口。
- `accel/tcg/`：指令计数、每核时钟、双核轮转与实时节拍。
- `hw/riscv/arcs/` 与 `include/hw/riscv/arcs/`：ARCS 芯片控制器、板级组合与宿主控制接口；
  `hw/riscv/arcs/meson.build` 汇总这些模型的构建依赖。
- `hw/display/`、`hw/i2c/`、`hw/audio/`：显示、传感器、触控及音频通道。
  芯片寄存器、独立器件和宿主传输保持分层，不将固件业务放入模型。
- ROM 和 LUNA 二进制由构建方提供，不提交 SDK、业务固件或私有组件源码。
  `scripts/lisem-assets.py` 校验目标、ABI、大小及哈希，在构建目录生成头文件。

## 构建与验证

ARCS 使用 `--target-list=riscv32-softmmu`、`--without-default-devices`、
`--with-devices-riscv32=arcs`，并指定两个绝对路径选项：
`-Dlisem_rom_dir=...` 指向含 AP/CP ROM 与清单的目录；
`-Dlisem_luna_dir=...` 指向当前宿主目标的静态库及清单目录。
Lisem 负责产品打包和完整固件回归，本仓库保留 QEMU 原生构建入口。
不需要为 ARCS 构建初始化 `roms/` 的上游固件子模块。

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
