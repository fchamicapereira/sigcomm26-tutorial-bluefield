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

The BlueField-3 cards you will be programming live in racks at five sites across four universities
and NVIDIA. They are not reachable from the public internet, and no SmartNIC will be physically
provided during the tutorial.

**Tailscale** closes that gap. It is a VPN that will put your laptop and the
BlueField cards on the same small private network, as if they were on the same LAN.
We ask you to **install the tailscale client and join the tutorial network before you arrive.**
Doing it at home takes about five minutes; doing it in the room, forty people at once on the
conference Wi-Fi, takes considerably longer.

You do **not** need a Tailscale account, and you will not be asked to sign in with Google, GitHub,
or anything else. Your laptop joins with a key we send you.

# You will get access to the cards at the start of the session, not before

Joining the network and reaching the cards are two different things. This guide gets you onto the
network. Access to the cards is opened at the start of the tutorial and closed again afterwards —
they are shared lab machines at several institutions, so they are reachable for the session and to
the people in the room.

So when you finish this guide, **you will see no tutorial machines, and that is the correct
result** — not a sign that something went wrong. You will also never see other participants'
laptops, before or during the session; the network is configured so that participants can reach the
cards and nothing else.

# What you need

- Your laptop, with permission to install software on it.
- The **join key** we email you before the tutorial. It looks like `tskey-auth-...`. If it has not
  arrived by the day before, check your spam folder and then email us.

# Step 1 — Install the client

Pick your platform.

## macOS

Install with Homebrew, or download it from
[tailscale.com/download/mac](https://tailscale.com/download/mac):

```bash
brew install --cask tailscale
```

Launch Tailscale from Applications once, so it can install its system components. It runs as a
menu-bar icon.

The `tailscale` command lives inside the app bundle and may not be on your path. Link it, so step 2
works:

```bash
sudo ln -s /Applications/Tailscale.app/Contents/MacOS/Tailscale /usr/local/bin/tailscale
```

## Windows

Download and run the installer from
[tailscale.com/download/windows](https://tailscale.com/download/windows). Tailscale then lives in
the system tray, and `tailscale` is available in a terminal.

## Linux

The install script covers every mainstream distribution:

```bash
curl -fsSL https://tailscale.com/install.sh | sh
```

It installs the client and enables the `tailscaled` service.

# Step 2 — Join the tutorial network

Run the command for your platform, with the key we emailed you in place of the one below.

On **Linux**:

```bash
sudo tailscale up --auth-key=tskey-auth-PLACEHOLDER-NOT-A-REAL-KEY
```

On **macOS** and **Windows**:

```bash
tailscale up --auth-key=tskey-auth-PLACEHOLDER-NOT-A-REAL-KEY
```

**Your operating system will ask you to allow a VPN configuration. Say yes.** This is the step that
is worth doing at home: on a managed or work-issued laptop it can require an administrator, and it
is the one part of this that occasionally needs someone else's help.

# Step 3 — Check that it worked

```bash
tailscale status
```

You should see your own laptop listed with a `100.x.y.z` address. There will be no other machines.
That is exactly right — see the second section above. If your own machine is there, you are done.

# On the day

Bring the laptop as you left it. At the start of the session we open access to the cards, and they
will appear in `tailscale status` within a few seconds without you doing anything. We will hand out
machine assignments and connection details then.

# If something goes wrong

**You cannot install software on your laptop.** Some managed or work-issued machines block it, and
so do some VPN-configuration policies. Find out now rather than at 08:30 — email us and we will
sort out an alternative.

**The key is rejected, or has expired.** Join keys have a lifetime and we may have rotated it.
Email us and we will send a fresh one.

**Your employer's VPN is already running.** Corporate VPNs and Tailscale often compete over routes.
Disconnecting the other one while you run step 2 is usually enough; if it is not, tell us at the
start of the session rather than debugging it alone.

**`tailscale: command not found`.** On macOS, the symlink at the end of step 1 is what fixes this.
On Linux, the install script needs `sudo` and a systemd-based distribution — if yours is neither,
the manual packages at [tailscale.com/download](https://tailscale.com/download) cover the rest.

**Anything else.** Email us before the tutorial, or find us in the room a few minutes early. We
would much rather fix it then than at 08:30.
