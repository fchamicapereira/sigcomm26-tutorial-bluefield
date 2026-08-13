---
title: "Pre-tutorial setup: Tailscale"
---

<!--
  DECIDE BEFORE PUBLISHING: the join key below is a placeholder and will not work.

  It is a bearer credential -- anyone holding it can register a device on the tailnet. Either
  replace it with the real key (accepting that the published PDF then carries it), or leave the
  placeholder and email the real key to registered participants. The surrounding text is written
  to work either way: it says the key is emailed, which stays true if you also print it here.

  Whichever you choose, generate the key as: reusable, ephemeral, pre-approved, tag:participant.
  Ephemeral matters -- participant laptops then remove themselves from the tailnet when they go
  offline, instead of leaving forty stale nodes behind.
-->

During this tutorial, you will be programming [NVIDIA BlueField-3 SmartNICs](https://resources.nvidia.com/en-us-accelerated-networking-resource-library/datasheet-nvidia-bluefield).
The tutorial focus on hands-on programming, and you will be given access to 12 BlueField-3 cards,
each with a different configuration. You will be able to run your own code on the cards, and see
how it behaves in a real network environment.

The BlueField-3 cards you will be programming live in racks at five sites across four universities
and NVIDIA. They are not reachable from the public internet, and no SmartNIC will be physically
provided during the tutorial.

You will use **Tailscale** to connect to the tutorial network and access the
BlueField cards.
Tailscale is a VPN that allows you to put your laptop and the BlueField cards on the same small
private network, as if they were on the same LAN.

We ask you to **install the tailscale client and join the tutorial network before you arrive.**
With that in mind, we wrote this small guide to help you get set up for the tutorial.

You do **not** need a Tailscale account, and you will not be asked to sign in with Google, GitHub,
or anything else. Your laptop joins with a key we provide to you.

At the end of this guide, you will be be able to join the tutorial network and see the Bluefield-3 cards.
However, ssh access to the cards is only provided at the start of the tutorial, and closed again by the end
of the conference.

# What you need

- A laptop, with permission to install software on it.
- Internet connection.

# Step 1 — Install the client

We will now install the `tailscale` client on your laptop.
After installing, there is *no need* for you to sign in or create an acount.
The authentication key we provide to you in step 2 is all you need to join the tutorial network.

## macOS

You can download it from [tailscale.com/download/mac](https://tailscale.com/download/mac),
or install it with [Homebrew](https://brew.sh/):

```bash
$ brew install --cask tailscale
```

Launch Tailscale from Applications once, so it can install its system components. It runs as a
menu-bar icon.

The `tailscale` command lives inside the app bundle and may not be on your path. Link it, so step 2
works:

```bash
$ sudo ln -s /Applications/Tailscale.app/Contents/MacOS/Tailscale /usr/local/bin/tailscale
```

## Windows

Download and run the installer from
[tailscale.com/download/windows](https://tailscale.com/download/windows). Tailscale then starts on
its own and lives in the system tray, and `tailscale.exe` is available in PowerShell.

## Linux

The install script covers every mainstream distribution:

```bash
$ curl -fsSL https://tailscale.com/install.sh | sh
```

It installs the client and enables the `tailscaled` service.

# Step 2 — Join the tutorial network

Run the command for your platform, with the key we provide to you in place of the one below.

On **Linux**:

```bash
$ sudo tailscale up --auth-key=tskey-auth-PLACEHOLDER-NOT-A-REAL-KEY
```

On **macOS**:

```bash
$ tailscale up --auth-key=tskey-auth-PLACEHOLDER-NOT-A-REAL-KEY
```

On **Windows**, in PowerShell:

```powershell
$ tailscale.exe up --auth-key=tskey-auth-PLACEHOLDER-NOT-A-REAL-KEY
```

# Step 3 — Check that it worked

You can now check that your laptop is on the tutorial network, and able to see the BlueField-3 cards:

```bash
# On Windows, use `tailscale.exe` instead of `tailscale`.
$ tailscale status
100.124.27.9     bf3-nvidia-1          tagged-devices              linux    -
100.115.117.14   bf3-nvidia-2          tagged-devices              linux    -
100.103.195.108  bf3-nvidia-3          tagged-devices              linux    -
100.88.192.90    bf3-nvidia-4          tagged-devices              linux    -
100.103.77.119   bf3-ulisbon-1         tagged-devices              linux    -
100.97.207.2     bf3-ulisbon-2         tagged-devices              linux    -
100.116.169.79   bf3-ulisbon-3         tagged-devices              linux    -
100.101.195.64   bf3-umich-1           tagged-devices              linux    -
100.118.32.66    bf3-umich-2           tagged-devices              linux    -
100.83.137.121   bf3-uwashington-1     tagged-devices              linux    -
100.110.170.20   bf3-uwashington-2     tagged-devices              linux    -
100.118.190.119  bf3-uwaterloo-1       tagged-devices              linux    -
```

You should also see your own laptop listed with a `100.x.y.z` address.

# On the day

Bring the laptop as you left it. At the start of the session we open access to the cards, and you
will be able to ssh into them with the shared tutorial account:

| **User** | **Password**        |
| -------- | ------------------- |
| `s26t`   | `sigcomm26tutorial` |

