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

## JSON Configuration

Instead of module parameters, json files are used to configure driver settings.
Each `pf` has it's own json file under `json` directory. 

* `queue_base` and `queue_max` are used to restrict the range of queues for each `pf`.
* `used_queues` is the real number of used queues. The value of `0` means that the number equals that of data interrupts.
* In the only one of `pf`s, `pci_master_pf` should be `true`. In the others, the value should be `false`.
* It runs on direct interrupt mode when `poll_mode` is `false`. Otherwise, it runs on poll mode.
* RS-FEC of cmac is enabled when `rsfec_en` is `true`.
* `port_id` is used for cmac id and pf id.
* Each `pf` must have a unique `mac_addr`.

For installation,
```sh
$ sudo make json_install
```

## Test Setup

The following test setup is valid for a machine which has an alveo card with two QSF28 ports.
One port is plugged directly into the other. 
`open-nic-shell` project should be built with the option of `-num_cmac_port 2`

* ip setting
```sh
$ sudo ip netns add ns_enp2s0f0
$ sudo ip netns add ns_enp2s0f1
$ sudo ip link set enp2s0f0 netns ns_enp2s0f0
$ sudo ip netns exec ns_enp2s0f0 ip addr add dev enp2s0f0 192.168.253.1/24
$ sudo ip netns exec ns_enp2s0f0 ip link set dev enp2s0f0 up
$ sudo ip link set enp2s0f1 netns ns_enp2s0f1
$ sudo ip netns exec ns_enp2s0f1 ip addr add dev enp2s0f1 192.168.253.2/24
$ sudo ip netns exec ns_enp2s0f1 ip link set dev enp2s0f1 up
```

* iperf server
```sh
$ sudo ip netns exec ns_enp2s0f0 iperf -s -P8
------------------------------------------------------------
Server listening on TCP port 5001
TCP window size:  128 KByte (default)
------------------------------------------------------------
[  4] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45060
[  5] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45056
[  6] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45058
[  7] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45054
[  8] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45052
[ 10] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45064
[ 12] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45066
[ 11] local 192.168.253.1 port 5001 connected with 192.168.253.2 port 45062
[ ID] Interval       Transfer     Bandwidth
[  7]  0.0-10.0 sec  4.17 GBytes  3.58 Gbits/sec
[ 11]  0.0-10.0 sec  2.50 GBytes  2.15 Gbits/sec
[  4]  0.0-10.0 sec   765 MBytes   640 Mbits/sec
[  5]  0.0-10.0 sec   506 MBytes   423 Mbits/sec
[  6]  0.0-10.0 sec   844 MBytes   707 Mbits/sec
[  8]  0.0-10.0 sec   833 MBytes   697 Mbits/sec
[ 10]  0.0-10.0 sec   512 MBytes   429 Mbits/sec
[ 12]  0.0-10.0 sec   364 MBytes   304 Mbits/sec
```

* iperf client
```sh
$ sudo ip netns exec ns_enp2s0f1 iperf -c 192.168.253.1 -P8
------------------------------------------------------------
Client connecting to 192.168.253.1, TCP port 5001
TCP window size:  264 KByte (default)
------------------------------------------------------------
[ 10] local 192.168.253.2 port 45066 connected with 192.168.253.1 port 5001
[  8] local 192.168.253.2 port 45062 connected with 192.168.253.1 port 5001
[  9] local 192.168.253.2 port 45064 connected with 192.168.253.1 port 5001
[  3] local 192.168.253.2 port 45052 connected with 192.168.253.1 port 5001
[  6] local 192.168.253.2 port 45056 connected with 192.168.253.1 port 5001
[  5] local 192.168.253.2 port 45058 connected with 192.168.253.1 port 5001
[  7] local 192.168.253.2 port 45060 connected with 192.168.253.1 port 5001
[  4] local 192.168.253.2 port 45054 connected with 192.168.253.1 port 5001
[ ID] Interval       Transfer     Bandwidth
[  8]  0.0-10.0 sec  2.50 GBytes  2.15 Gbits/sec
[  4]  0.0-10.0 sec  4.17 GBytes  3.59 Gbits/sec
[ 10]  0.0-10.0 sec   364 MBytes   305 Mbits/sec
[  9]  0.0-10.0 sec   512 MBytes   429 Mbits/sec
[  3]  0.0-10.0 sec   833 MBytes   698 Mbits/sec
[  6]  0.0-10.0 sec   506 MBytes   424 Mbits/sec
[  5]  0.0-10.0 sec   844 MBytes   708 Mbits/sec
[  7]  0.0-10.0 sec   765 MBytes   641 Mbits/sec
[SUM]  0.0-10.0 sec  10.4 GBytes  8.93 Gbits/sec
```
