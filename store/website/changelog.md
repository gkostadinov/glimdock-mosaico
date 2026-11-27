# Changelog

## Unreleased

- **Games**: a new app with three retro 3D games, each with a title screen
  where it plays itself, a pause when you leave, and a best score that is kept.
  **Ember Crypt** is a lantern-lit crawl through crypts that grow as you go:
  gather the shards to wake the gate before the oil runs out, and flare the
  wisps away. **Starbore** sends a dart of a ship down a twisting neon bore:
  slip through spinning bars, plates and half walls, sweep up orbs for the
  shield, and pulse a gate away when it is too late to dodge. **Skyglide** is a
  glider over hills and lakes at golden hour: thread the hoops for more time,
  ride the thermals for height, and keep off the ground. Drag a finger to steer
  and tap for each game's action, or play with Tilt: hold the board the way you
  like during a short countdown, then tilt it to steer (back towards you is up,
  or forward in the crypt, in every game; Flip swaps them), turn it to look
  around the crypt, and shake it for the action. The
  Interaction module's buttons and lights join in when it is fitted, and the
  games make short retro sounds at Glimdock's sound volume.
- **Home screen pages**: the icons now sit on pages you swipe between, with
  dots for the page you are on. Webcam, Interaction and Calculator are apps of
  their own on the second page (Tools keeps the torch, level and compass).
- **Edit the home screen**: hold an icon to make the icons jiggle, then drag
  them where you like, onto other pages too (hold at the edge to turn the
  page). The − hides an app; the + (or Settings, Home screen) brings hidden
  apps back. The arrangement is kept across restarts and updates.
- **Shortcuts**: one-tap tiles on the home screen, like Shortcuts on a phone.
  Starters come from your Swifty Home rooms ("All lights off", "Living room
  lights off", ...); make your own in Web settings, on the board or by asking
  Swifty ("make me a shortcut that turns off the bedroom lights"). Steps: Swifty
  Home lights (on, off, toggle, brightness, colour), scenes, plugs, the TV and
  speaker, waits, TV remote keys, the Interaction module's lights, the screen's
  brightness, opening an app and the flashlight. A tap shows a tick or a cross
  and a toast with how it went or which step failed.
- **Swifty's home control**: exact names win over near ones (a light called
  "Bedroom lamp" no longer counts as the bedroom), and any of the hub's scenes
  can be applied by name.
- **VPN**: a new app that connects to your WireGuard server. While it is on,
  all of the board's internet traffic, DNS included, goes through the tunnel,
  and nothing goes around it; the local network stays direct. Paste a
  WireGuard configuration in Web settings (or load it with
  `mosaico-config.py`). The app has a big switch, says what is happening in
  plain words, shows the tunnel address, last handshake and traffic, and can
  connect at start-up. It reconnects by itself after Wi-Fi drops, and Swifty
  can switch it.
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
