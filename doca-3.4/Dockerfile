# syntax=docker/dockerfile:1
#
# Build/dev environment for the SIGCOMM 2026 BlueField tutorial.
#
# Base is the DOCA *devel* image (not full-rt): it ships the DOCA SDK dev files
# (headers + pkg-config for doca-common / doca-argp / doca-flow / doca-dpdk-bridge
# and the bundled DPDK) plus the DPA toolchain (dpacc, already on PATH) and the
# native build tools (meson, ninja, gcc, pkg-config, git). full-rt only carries
# the runtime libs — no .pc files — so it can't compile these programs.
#
#   docker build -t sigcomm26-tutorial .
#
# The DOCA Flow programs are compiled at image-build time below to validate the
# toolchain. ACTUALLY RUNNING them needs the real hardware, so run the container
# privileged with the NIC/RDMA devices and hugepages exposed, e.g.:
#
#   docker run --rm -it --privileged --net=host \
#     -v /dev/infiniband:/dev/infiniband \
#     -v /dev/hugepages:/dev/hugepages \
#     sigcomm26-tutorial
#
FROM nvcr.io/nvidia/doca/doca:devel-3.4.0

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# The devel base already provides meson/ninja/gcc/pkg-config/git and dpacc.
# We add: patch (for the Part IV PCC patch step, not in the base image), libbsd-dev
# (optional dependency picked up by meson.build), python3-pyelftools (used by dpacc/DPDK
# when rebuilding the DPA algo), tmux (run server + client side by side), and sudo (the
# tutorial scripts call it for their privileged steps — see the ubuntu user setup below).
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
    patch \
    libbsd-dev \
    python3-pyelftools \
    tmux \
    sudo \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Bring in the project sources (the .dockerignore keeps build/, env/ and the
# git-ignored doca-pcc-ecn/app/ out of the build context).
COPY . /workspace

# Build the three DOCA Flow programs (doca_flow_nop / _mac / _ecn). This is a
# toolchain smoke-test; the resulting binaries need the real DPU to run. The
# DOCA PCC part is left as a runtime step (it copies and patches
# /opt/mellanox/doca/applications, which is git-ignored — see the README).
RUN meson setup build && ninja -C build

# Build the ttyplot helper (used by benchmark.sh for the live throughput chart) by running
# the same setup/setup_ttyplot.sh used on the host. It runs as root here (its sudo calls are
# auto-skipped), installs libncurses-dev via apt, and clones ttyplot from GitHub into
# /workspace/ttyplot (git-ignored, so excluded from COPY — it's cloned fresh). apt lists are
# refreshed for its apt-get install, then removed again to keep the layer small.
RUN apt-get update \
    && ./setup/setup_ttyplot.sh \
    && rm -rf /var/lib/apt/lists/*

# Run the container as the non-root 'ubuntu' user (already in the base image: UID 1000, in the
# sudo group) with passwordless sudo, so the tutorial scripts — which call sudo for their
# privileged steps (ip netns / ovs-vsctl / hugepages / running the DOCA programs) — work
# unchanged inside the container, including setup/setup_roce_loopback.sh. Everything above this
# line runs as root at build time.
RUN echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/ubuntu \
    && chmod 0440 /etc/sudoers.d/ubuntu \
    && chmod +x /workspace/docker-entrypoint.sh \
    && chown -R ubuntu:ubuntu /workspace \
    # Pre-create the marker that silences Ubuntu's /etc/bash.bashrc "To run a command as
    # administrator ... use sudo" hint (shown to sudo-group users until they've sudo'd once).
    && touch /home/ubuntu/.sudo_as_admin_successful \
    && chown ubuntu:ubuntu /home/ubuntu/.sudo_as_admin_successful
USER ubuntu

# On start, bring up the RoCE loopback (setup_roce_loopback.sh) before handing off to the
# command. Skippable with -e SKIP_ROCE_SETUP=1. See docker-entrypoint.sh.
ENTRYPOINT ["/workspace/docker-entrypoint.sh"]
CMD ["/bin/bash"]
