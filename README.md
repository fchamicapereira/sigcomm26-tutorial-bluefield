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
$ ib_send_bw -d mlx5_2 -x 1 -F --run_infinitely
```

Client:

```
$ ib_send_bw -d mlx5_3 -x 1 -F localhost --run_infinitely
```