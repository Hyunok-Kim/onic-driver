# onic-driver

onic-driver is the brand-new driver for [open-nic-shell](https://github.com/Xilinx/open-nic-shell).
It is written refering to [qep-driver](https://github.com/Xilinx/qep-drivers/tree/master/linux-kernel/driver)
based on [libqdma](https://github.com/Xilinx/dma_ip_drivers/tree/master/QDMA/linux-kernel/driver/libqdma)<br>
Because libqdma has the full-blown functions for qdma, we can treat it in high level in the application driver.<br>

However, two modifications are required.
1. [open-nic-shell patch](https://github.com/Hyunok-Kim/open-nic-shell/commit/a1ba78308efded589967431eddf0a397f69f2806)
2. [libqdma patch](https://github.com/Hyunok-Kim/dma_ip_drivers/commit/a6c6e41243a2eecc66f6e157f93017b8aa4941e2)(The patch is applied to this driver)

The first patch is required because libqdma require the specific data formats for st h2c descriptor and st c2h completion entry.<br>
The second patch is to fix the problem the irq allocation order of original libqdma issues the abnormal user/error interrupts 



