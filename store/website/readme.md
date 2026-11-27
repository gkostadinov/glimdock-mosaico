# Glimdock for ESP-Mosaico

A full-screen launcher for the ESP-Mosaico board that turns it into a desk and
wall companion for your homelab and home.

## What's inside

- **Glimdock**: a homelab dashboard for a [Glimdock](https://github.com/gkostadinov/glimdock)
  collector: hosts, services, storage, sensors, printers, alerts and the Kordlet
  agent chat.
- **Home**: a wall display for a self-hosted
  [Swifty Home](https://github.com/gkostadinov/swifty-home) hub: rooms, lights,
  music, TV and plugs, paired with a one-time display code.
- **Swifty**: a voice and text assistant for any OpenAI-compatible model, with
  push-to-talk speech, optional web search, and tools to open apps and control
  the board (lights, IR TV remote, Interaction module). It keeps notes, lists
  and schedules in its own folder on your NAS, and sets reminders that come up
  over any app with a chime.
- **Camera** and **Gallery**: viewfinder, filters, photos and NAS sync for the
  camera module, which detects whether it faces front or rear. In the gallery,
  tap a photo to hide the controls, and double-tap or pinch to zoom.
- **Files**: the board's storage and SMB shares, with image and text previews.
- **Webcam**: the board as a **USB webcam** (1280 × 720 or 640 × 480 MJPEG,
  upright whichever way the board is held, with the flash as a light, plus its
  microphone). On a Mac, [MosaicoCam](https://github.com/gkostadinov/mosaico-cam)
  adds live filters and 4K upscaling and passes it on to FaceTime, Zoom and the
  like.
- **Interaction**: the Interaction module's buttons, motion and light sensors,
  LEDs and IR TV remote.
- **Tools** and **Calculator**: torch, spirit level and compass; a calculator.
- **Settings**: brightness, haptics, auto-rotate, the panel's corner radius, time
  zone, camera options and **Web settings**.

The apps sit on home screen pages you swipe between. Hold an icon to rearrange
them like on a phone: drag icons around and across pages, hide the ones you
don't use, and bring them back from Settings.

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

## Source and links

Open source under the Apache License 2.0:
[github.com/gkostadinov/glimdock-mosaico](https://github.com/gkostadinov/glimdock-mosaico).

- Found a problem or have an idea? [Open an issue](https://github.com/gkostadinov/glimdock-mosaico/issues).
- [Glimdock](https://github.com/gkostadinov/glimdock): the homelab collector the Glimdock app reads.
- [Swifty Home](https://github.com/gkostadinov/swifty-home): the self-hosted smart-home hub the Home app shows.
- [MosaicoCam](https://github.com/gkostadinov/mosaico-cam): a macOS app for the board's USB webcam.
