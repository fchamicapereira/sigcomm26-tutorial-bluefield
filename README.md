# Testing the setup

```
$ sudo /opt/mellanox/dpdk/bin/dpdk-testpmd \
  -l 1-15 -n 4 \
  -a 0000:03:00.0 -a 0000:03:00.1 \
  -- \
  --forward-mode=txonly --txonly-multi-flow \
  --eth-peer=0,f0:fb:7f:e2:e2:77 \
  --eth-peer=1,f0:fb:7f:e2:e2:76 \
  --rxq=8 --txq=8 --nb-cores=14 \
  --txd=4096 --rxd=4096 --burst=64 \
  --total-num-mbufs=524288 \
  --stats-period=1 -i
testpmd> set promisc all on
testpmd> set txpkts 1518
testpmd> start
testpmd> show port stats all
```

# DOCA Flow

Initial setup of the build directory:

```
$ meson setup build
```

Building:

```
$ cd build
$ ninja
```

Running:

```
$ sudo ./doca-flow-ecn/doca_flow_ecn
```

### Running client/server example

Server:

```
$ sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -x 1 -F --report_gbits --run_infinitely -D 1
```

Client:

```
$ sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -x 1 -F 10.0.0.1 --report_gbits --run_infinitely -D 1
```

