# Glimdock for ESP-Mosaico

A full-screen launcher for the ESP-Mosaico board that turns it into a desk and
wall companion for your homelab and home.

## What's inside

- **Glimdock**: a homelab dashboard for a [Glimdock](https://github.com/gkostadinov/glimdock)
  collector: hosts, services, storage, sensors, printers, alerts and the Kordlet
  agent chat.
- **Home**: a wall display for a self-hosted Swifty Home hub: rooms, lights,
  music, TV and plugs, paired with a one-time display code.
- **Swifty**: a voice and text assistant for any OpenAI-compatible model, with
  push-to-talk speech, optional web search, and tools to open apps and control
  the board (lights, IR TV remote, Interaction module).
- **Camera** and **Gallery**: viewfinder, filters, photos and NAS sync for the
  camera module, which detects whether it faces front or rear.
- **Files**: the board's storage and SMB shares, with image and text previews.
- **Tools**: torch, spirit level, compass, calculator, the Interaction module and
  a **USB webcam** mode (1280 × 720 or 640 × 480 MJPEG, upright whichever way the
  board is held, with the flash as a light).
- **Settings**: brightness, haptics, auto-rotate, the panel's corner radius, time
  zone and camera options.

## Setting up

Glimdock installs next to Vibe Mode and uses Vibe Mode's Wi-Fi. Each app is set
up on the board, or all at once from a computer with the configuration tool in
the source repository. Nothing points at a server until you configure it:
Glimdock needs your collector's address and display token, Home a hub address
and pairing code, and Swifty a model server (speech and web search are
optional).

## Privacy

The board only talks to the servers you configure. With the time zone on
Automatic, it looks up its time zone from its public IP address at `ipapi.co`;
choose a time zone in Settings to turn that off.

## Source

Open source under the Apache License 2.0:
[github.com/gkostadinov/glimdock-mosaico](https://github.com/gkostadinov/glimdock-mosaico).
