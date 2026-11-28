# Changelog

## Unreleased

- **Profiles**: who uses the board and what each may do. Each profile has its
  apps (hidden, read-only or full), what Swifty may do for it and whether it may
  change the network or the profiles; Guest is what the board shows when nobody
  is signed in. Sign in with a PIN, your face or both; full mode, for the risky
  things, always needs face and PIN. Faces are recognised on the board by the
  camera module and never leave it. Until a profile exists, nothing changes.
- **Widgets** move like icons, on any page, in two sizes, from a widget
  gallery. New ones: **weather** (Open-Meteo, for the network's location or a
  place you set), the date, the next reminder and the network.
- **Shortcuts** became small programs: if / else, repeats, variables, waits and
  messages, started by a tap, a time, every few minutes, sunrise or sunset, the
  Interaction module's motion or buttons, or a Home device. Web settings has a
  block editor with a dry run, and Swifty writes them for you.
- **Swifty**: dictations of up to five minutes that carry on through a dropped
  connection, errors that say what went wrong (the server's address, a timeout,
  a stalled handshake), Paseo agents (read, message, start, schedules), and a
  developer mode for profiles allowed it.
- **Home**: feeding the pet, the litter box's clean cycle, the TV's power and
  Movie work again; switches, air conditioning (power, temperature, mode, fan),
  the 3D printer and other devices the hub lists.
- **Games**: Skyglide and Ridge Rally get harder stage by stage, with real ways
  to lose: lift to keep up, spires, crosswinds and downdrafts in the air; a
  clock that tightens, puddles, rocks, logs and damage on the road.
- **VPN**: a 1280-byte tunnel by default (phone hotspots dropped bigger
  packets), split tunnels use their DNS, and Web settings shows the tunnel
  address.
- **The AI key is Back** on the home screen: it closes Wi-Fi, the shortcuts'
  sheet, editing and Settings one step at a time.

## 1.2.0

- **Wi-Fi in Settings**: tap the Wi-Fi card to see the networks around you and
  join one, typing its password on the board (or Other network... for a hidden
  one). A wrong password or a network out of reach takes the board back to the
  one it was on. Networks you join are remembered, the board starts on the one
  chosen last, and when that one is away it joins another network it knows.
  Vibe Mode's network stays the default, and every app (Glimdock included)
  follows the board's choice.
- **Games**: a new app with six retro 3D games, each with a title screen
  where it plays itself, a pause when you leave, and a best score that is kept.
  **Ember Crypt** is a lantern-lit crawl through crypts that grow as you go:
  gather the shards to wake the gate before the oil runs out, and flare the
  wisps away. **Starbore** sends a dart of a ship down a twisting neon bore:
  slip through spinning bars, plates and half walls, sweep up orbs for the
  shield, and pulse a gate away when it is too late to dodge. **Skyglide** is a
  glider over hills and lakes at golden hour: thread the hoops for more time,
  ride the thermals for height, and keep off the ground. **Jellystorm** holds
  back swarms of glowing space jellies, with pearls for bigger guns, a pulse
  bomb and a mother jelly every fourth wave. **Lunar Flip** is a moonbase
  pinball table: bumpers, slingshots, lanes that raise the multiplier, drop
  targets and a ball save; the flippers are the screen's halves (or the
  Interaction module's buttons) and a shake nudges. **Ridge Rally** is a gravel
  stage through autumn pines against the clock, with rivals to pass and
  checkpoints for time. Drag a finger to steer
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
- **Scrolling**: no more tear line (diagonal or upright) across the screen
  while scrolling with the board turned sideways.
- **Swifty after the camera**: speech no longer stops working after the camera
  or webcam was used; the camera gives back its memory when it closes.

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
