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
  camera module, which detects whether it faces front or rear. In the gallery,
  tap a photo to hide the controls, and double-tap or pinch to zoom.
- **Files**: the board's storage and SMB shares, with image and text previews.
- **Tools**: torch, spirit level, compass, calculator, the Interaction module and
  a **USB webcam** mode (1280 × 720 or 640 × 480 MJPEG, upright whichever way the
  board is held, with the flash as a light).
- **Settings**: brightness, haptics, auto-rotate, the panel's corner radius, time
  zone, camera options and **Web settings**.

## Setting up

Glimdock installs next to Vibe Mode and uses Vibe Mode's Wi-Fi. Nothing points at
a server until you configure it: Glimdock needs your collector's address and
display token, Home a hub address and pairing code, and Swifty a model server
(speech and web search are optional). Without them, Camera, Gallery, Files,
Tools and Glimdock's demo still work.

The easiest way to set everything: open **Settings**, switch on **Web settings**,
and open the address it shows on a phone or computer on the same network. Enter
the six-digit code from the board, then fill in the servers and keys. Saved keys
never leave the board (the page shows only whether each is set). The page is
plain HTTP on your network and turns itself off after 10 minutes unused, after 5
wrong codes and at every restart. A configuration tool in the source repository
does the same from a computer.

The tour video under Attachments shows the apps in motion.

## Privacy

The board only talks to the servers you configure. Web settings runs only while
you switch it on. With the time zone on
Automatic, it looks up its time zone from its public IP address at `ipapi.co`;
choose a time zone in Settings to turn that off.

## Source

Open source under the Apache License 2.0:
[github.com/gkostadinov/glimdock-mosaico](https://github.com/gkostadinov/glimdock-mosaico).
