# Changelog

## Unreleased

- **Home screen pages**: the icons now sit on pages you swipe between, with
  dots for the page you are on. Webcam, Interaction and Calculator are apps of
  their own on the second page (Tools keeps the torch, level and compass).
- **Edit the home screen**: hold an icon to make the icons jiggle, then drag
  them where you like, onto other pages too (hold at the edge to turn the
  page). The − hides an app; the + (or Settings, Home screen) brings hidden
  apps back. The arrangement is kept across restarts and updates.

- **Webcam microphone**: in webcam mode the board is also a USB microphone
  ("ESP-Mosaico Microphone", 48 kHz), so calls get its voice along with its
  picture. The computer's input level and mute work; Swifty waits while the
  computer uses it.
- **Always-on display**: after a while unused (2 minutes by default; 30 seconds
  to 10 minutes or off in Settings and Web settings) the app fades to low light
  for a few seconds, then the screen becomes a dim clock on black with the date,
  the next reminder and the battery. The clock is drawn once a minute, a few
  pixels away each time, while the app under it rests. A touch, the AI key or a
  reminder brings the app back as it was. In webcam mode the screen only dims.
- **Keyboard**: the magnified key of the top row is no longer hidden behind
  the field above the keyboard.

## 1.1.0

- **Web settings**: switch it on in Settings to set servers, keys and shares
  from a browser, with a code shown on the board. It turns itself off.
- **Gallery**: tap a photo to hide the controls; double-tap or pinch to zoom,
  then drag (it glides) to look around.
- **Smoother everywhere**: scrolling at up to ~30 fps (from 12–21), faster
  Glimdock updates, and screens that do not change no longer redraw.
- **Steadier over time**: more free memory, so Swifty no longer slows down after
  a while, and no more short freezes while speech reconnects.
- Smaller fixes: Home's music tile, the compass before calibration, the webcam
  status, Glimdock's health times, Swifty's chat edge.

## 1.0.0

First release: launcher, Glimdock, Home, Swifty, Camera, Gallery, Files, Tools
(with the USB webcam) and Settings.
