<p align="center"><img src="docs/logo.svg" width="140" alt="TidyOS Pro logo"></p>

# Tidy Pro Plus for the Heltec V3 and V4

**Install: https://hardwaremack-prog.github.io/Tidy-Pro-Plus/** (Chrome or Edge, USB data cable)

This is [Tidy Pro Plus](https://github.com/hardwaremack-prog/Tidy-Pro-Plus) with the **Heltec WiFi LoRa 32 V3** added
next to the V4 and V4-R8. Pick the board at the top of the flasher.

| Board | Flash / PSRAM | What goes on it | Room for files |
|---|---|---|---|
| **Heltec V3** | 8 MB / none | TidyOS Pro only (no Shelf: it doesn't fit next to it in 8 MB) | about **4.4 MB** |
| Heltec V4 | 16 MB / 2 MB | TidyOS Pro + The Shelf (dual boot), or either one | 7.4 MB (12.4 MB TidyOS Pro only) |
| Heltec V4-R8 | 16 MB / 8 MB | the same as the V4 | the same |

### 3.4.40: update using the phone

* **Settings > Update using phone** (V3 / V4): gets the update over the internet of the phone or laptop showing the
  desktop, even when the board is on a Wi-Fi. Before it restarts it checks that the desktop page is open on a phone
  and that the phone can reach GitHub, and says what to do if not (on the hotspot: choose **Keep phone internet**,
  since with *Reach TidyOS* the phone has no internet while joined). Keep the page open and in front, screen on.
* **Updates now come from this repo** (hardwaremack-prog.github.io/Tidy-Pro-Plus), where the V3 / V4 builds are.
  Boards on 3.4.39 or older still look in hardwaremack-prog/Tidy-Pro-Plus, so install 3.4.40 once from this
  flasher; after that Check for updates and Update using phone find the new versions here.

### 3.4.38: the desktop page on phones

* **3.4.39: zoomed in, one finger moves the screen:** swipe to move around, tap to click, touch-and-hold
  (about a third of a second) then drag to use the mouse (drag windows, select). At normal size one finger is the mouse.
* **Pinch to zoom** the desktop with two fingers, and drag it around with two fingers. One finger is still the mouse
  (a press waits 80 ms, so the first finger of a pinch doesn't click). When you let go, the page redraws it at the new
  size with hard pixels, so text stays sharp. **Fit** (bottom right) or pinching back out goes back to the whole screen.
* **The top bar fits a phone:** smaller buttons, the speed / uptime text hidden on narrow screens, and it scrolls
  sideways if it's still too wide. Safari's own page zoom is off; it was what made the bar (and everything) fuzzy.

### 3.4.37: more room and a faster sweep for the RF tools

* **Left out to make room for the RF tools:** Browser, Pictures, HTML Games and HTML Programs (about 30 KB of the
  program slot). Opening a picture from Files or Screenshot now shows it in full colour in the web browser that's
  viewing the desktop. To put one back, build with `-DTIDY_BROWSER=1`, `-DTIDY_PICTURES=1` or `-DTIDY_HTML_APPS=1`.
* **Faster sweep while the desktop is open in a browser:** the radio task now has the same priority as the task that
  streams the screen, so they take turns instead of the sweep waiting (it took ~1.4 s a sweep). RF Sweep shows
  "N ms each (radio working M ms)", so you can see how much time goes elsewhere.
* **RF Exposure:**
  * Each second counts every channel, using a channel's last reading when the sweep didn't reach it that second
    (a sweep takes longer than a second; before, those channels counted as nothing and the figure read about a
    quarter low).
  * Tiny shares read as "1/110,000 of the limit" instead of "9.0e-04%".
  * The CSV's % columns are in scientific notation (they were rounded to 0.00000), and each line is written to the
    card straight away, so a copy taken while it logs has everything.

### 3.4.36: get files off the board in a web browser

* **`http://<board>/files`** (on the hotspot: **http://10.1.1.250/files**), or the new **Files** button on the TidyOS
  desktop page, opens the file page for the **SD card** and the **board's storage**. It starts the file page for now
  (it stops after 10 idle minutes) even when Wi-Fi sharing is off in Settings.
* On the file page: **tick boxes**, **Download ticked** (one file as itself, several or folders as one ZIP) and
  **Download this folder (ZIP)**, subfolders included. Click a file's name to download just that one. Upload, new
  folder, rename and delete are as before.
* Over Wi-Fi this is far quicker than the USB file copy in the flasher (which also got Save ticked / Save folder).

### 3.4.35: RF Exposure, made for a room full of meters

* **A live reading:** big *Now x% of the limit* (the last second), the **FCC 30-minute average**, and the average and
  worst second since **Reset**. Green / black / red as it gets closer to the general-public limit.
* **Exposure over time:** a graph of every second (blue bars, yellow over 10%) with the 30-minute average as a black
  line and the limit as a red line. Log scale (each grid line is 100x), so a lab at 0.01% and a meter at your ear both
  show. **2 min / 10 min / 30 min** button.
* **The band right now:** a bar per 200 kHz channel over the last second, the strongest in red, and how much of your
  exposure it gives.
* **Log to CSV:** one line a second (time, mW/cm2, % of the limit, 30-min average, strongest channel, noise, overload)
  to the SD card's `rf` folder. It keeps logging while the window is minimised.
* No more "since the last Reset in RF Sweep": the app starts the sweep itself, works each second from the sweep's
  running sums (nothing between screen updates is missed), and has its own Start / Stop and Reset. The
  "transmitter at a distance" calculator is behind the **Calculator** button.

### 3.4.31: the V3 talks over its USB port again

* The V3's USB-C port goes through a CP2102 to the serial pins (UART0), not to the ESP32-S3's own USB like the V4's.
  3.4.29 and 3.4.30 for the V3 were built with the V4's USB setting, so TidyOS's console, **Files over USB** and the
  flasher's board checks heard nothing from a V3. Now `Serial` is UART0 on the V3 (`CDCOnBoot=default` in `build.sh`).
* If a V3 won't go into the flasher: unplug it, hold **PRG**, plug it in, let go, then Install. Press **RST** after.

### 3.4.30: a hotspot Windows and phones can use

* **10.1.1.250 opens on Windows, iPhone and Android.** Before, the hotspot gave out no gateway and no DNS, so devices
  marked it "no internet" and many sent the browser over cellular (or Windows dropped it), and the page never loaded.
  Now, by default, the hotspot gives itself out as gateway and DNS server and answers Windows', Apple's and Android's
  internet checks (`connecttest.txt`, `hotspot-detect.html`, `generate_204`...) the way they expect, so the devices
  treat it as their network. `http://tidyos` works too. Every other name gets "doesn't exist", so phones' background
  traffic fails at once instead of filling the board's connections.
* **The trade-off, as a choice:** Settings &rarr; *Run TidyOS Hotspot* asks **Reach TidyOS** (the above; no internet on
  the joined device meanwhile, so no weather or news on the desktop) or **Keep phone internet** (the old way: weather
  and news come over the phone's own data, but some phones need mobile data off, or "Stay connected", to open the page).
  The choice is remembered and also used when the hotspot starts by itself because home Wi-Fi isn't in range.
  Android may still say "limited connection": tap it and choose to stay connected.
* Stronger signal (15 dBm, was 11), plain WPA2, and TidyOS stops looking for home Wi-Fi 5 s after a device joins
  (was 15 s), so a PC that's still joining doesn't lose it.
* The first-setup sign-in page (no Wi-Fi saved yet) is unchanged.

### What's different on the V3

* **Built with `-DTIDYOS_V3`**, 8 MB flash, PSRAM off, and its own partition table (`source/TidyOSPro/partitions-v3.csv`):
  boot menu, the 3 MB TidyOS Pro slot, and the rest (4.4 MB) as the board's file storage. A 3 MB update fits there
  when there's no microSD card.
* **No front-end module** (the V4's GC1109 / KCT8103L LNA) and **no GNSS header**: TidyOS Pro leaves GPIO 2, 5, 7, 46
  and 34/38-42 alone, the radio is read straight from the SX1262, and the GPS app has nothing to find. The RF tools'
  front-end correction starts at 0 dB, which is right for the V3.
* **No PSRAM**: the desktop starts in 4 colours (as on a plain ESP32) and drops to black and white while an update downloads.
* **microSD**: The Shelf's V3 in-line wiring is tried first (CS 42, MOSI 41, CLK 40, MISO 39), then CS 4, MISO 6, CLK 47, MOSI 48.
* **Battery**: V3.0/3.1 switch the battery divider on with ADC_CTRL (GPIO 37) low, V3.2 with it high; TidyOS reads both ways
  and keeps the higher reading, the same as The Shelf.
* On the 900 MHz Coms mesh it says it's a V3 (board type 3), like The Shelf for V3 does.
* Clone copies onto another board of the same kind (8 MB for a V3).
* Settings &rarr; **Check for updates** reads `manifest-pro-v3.json` from this repo (V4: `manifest-pro.json`, R8: `manifest-pro-r8.json`).

The rest of this page is the Tidy Pro Plus notes; they apply to the V3 too unless they talk about The Shelf or dual boot.

Tidy Pro Plus is a copy of [TidyOS Pro](https://github.com/hardwaremack-prog/TidyOS-Pro) with a better web flasher:

- **Tick what you want copied**: TidyOS Pro, The Shelf, The Shelf's web app, the bootloader and the boot menu, each on its own.
- **See the free space as you tick**: a map of the board's 16 MB shows where each piece goes and how much is left
  (room for files, spare room for updates, and empty space), with the total free in big numbers.
- **Clean-up choices**: empty the slots you didn't tick, forget Wi-Fi and settings, or wipe either system's on-board files.
- **Pick TidyOS Pro's apps**: all 32, ticked one by one. That's the field tools, the everyday apps, and every app from
  [TidyOS-ESP32](https://github.com/hardwaremack-prog/TidyOS-ESP32) that TidyOS Pro had left out: Ready, Steady,
  Rugged, Latch, Games, Doodle, Pictures, Music, HTML Games and HTML Programs. Each one shows how much flash it adds.
- Quick picks for dual boot, TidyOS Pro only or The Shelf only, plus the original one-click buttons (those now install the full build, every app on).

### How the app picking works

TidyOS Pro is built twice: the **field build** (`tidyos-pro-<board>.bin`, 2.59 MB: the field tools and everyday apps) and
the **full build** (`tidyos-pro-<board>-full.bin`, 2.86 MB: every TidyOS app as well, built with `-DTIDY_ALL_APPS=1`).
The flasher uses the field build unless a TidyOS-ESP32 app is ticked. Before writing TidyOS Pro, it puts the ticked apps
into the image's app list (`TIDY_APP_LIST` in `core.cpp`) and fixes the image's checksum and SHA-256, so the board
accepts it. On its first start the board saves that list in its settings, so a Wi-Fi update keeps your choice.
Settings is always on the desktop. With more than 24 apps the desktop's icon rows get shorter so all 32 fit.

The flasher writes the pieces with esptool-js. They're in `firmware/parts/` (made by `tools/parts.py`, which `build.sh` runs
after `package.py`), and `parts.json` lists their sizes, where they go and the app list.
Version 3.4.34. Settings → Check for updates gets updates from this repo, **with or without a microSD card**.

### 3.4.34: V3 memory switches

* On a V3 the sixth start-up switch (and Switches' fourth row) is the **OLED screen** instead of the GPS (a V3 has no GPS header): off, the board's own screen stays dark and its task and memory (~10 KB) are never set up. Holding PRG to forget Wi-Fi needs the OLED on.
* The V3 no longer runs the developer stack-size logger (3 KB).
* The other memory switches work on every board as before: SD card, wireless mesh, status light, Shelf messages and screen colours (a V3 starts in black & white).

### 3.4.33: V3 starts in black & white

* The V3's black & white default really applies now (3.4.32 still picked 4 colours: the V4's colour rule came first).

### 3.4.32: Heltec V3, merged

* The Heltec V3 support (TidyOS Pro only, 8 MB, no PSRAM) from Tidy-Pro-Plus-V3V4 is now here, with its hotspot work ("Reach TidyOS" / "Keep phone internet") and the V3 serial fix, on top of everything since 3.4.29.
* V3 memory, so its hotspot page actually opens: black & white screen unless you pick colours, the 900 MHz radio starts only when an app or the 900 MHz mesh needs it, and the screen is sent with a small fingerprint per row instead of a full copy of the picture (which a V3 can't hold).
* Updates for every board come from this repo again.

### 3.4.31 / The Shelf 1.20.0: Music (Opus) shelf

* The Shelf has a new **Music (Opus)** shelf. Songs shrunk to Opus (.opus) live there and MP3s stay on Music. Uploads and shrunk songs go to the right shelf by themselves, and .opus songs already on Music move over at start-up with their plays and album art. The Shelf on computers and Pis, the standalone Heltec / ESP32 Shelf (1.20.0) and this dual-boot Shelf all have it.
* TidyOS Pro's MP3 button picks songs from the Opus shelf too.

### 3.4.30: media controls, NEWS button, tidier music buttons

* Songs playing in the browser show their name (and artist, from "Artist - Title" names) on the phone's lock screen / notification player and a computer's media controls, and play, pause, stop and next work there. TidyOS keeps up when you pause or play from there.
* The NEWS tag is a button: click it to run the headlines the other way (the arrow shows which way).
* The music buttons sit evenly spaced between the clock and the drop box, slightly bigger and rounder.

### The Shelf 1.19.7 (dual boot): less memory for the Music list, faster Opus tags

* Looking up which songs have album art no longer lists the whole art folder at once, and Opus tags and covers are read in 512-byte pieces instead of a byte at a time (seconds per cover before). The same fix went into the standalone Shelf for Heltec / ESP32 as 1.19.2.

### 3.4.29: OLED sleep

* The board's screen goes off after 5 minutes without a PRG press, like The Shelf's (not while music plays). A press wakes it (that press only wakes it). Switches > OLED sleep turns this off: the screen then stays on and dims after 30 minutes, as before.

### 3.4.28: Messages on the hotspot

* Messages and the Coms mesh (2.4 GHz and 900 MHz) now work when TidyOS is only on its own hotspot (work, the car). They start by themselves there, and the mesh uses the hotspot's side of Wi-Fi, switching back to home Wi-Fi when it joins.
* The hotspot now uses Wi-Fi channel 1, the same as The Shelf's own Wi-Fi, so TidyOS and Shelf boards on their own Wi-Fi hear each other over the 2.4 GHz mesh. (900 MHz works whatever the channel.)
* Messages to Shelves at home over Wi-Fi still need home Wi-Fi.

### 3.4.27: battery gauge, round music buttons

* Battery gauge on the taskbar (left of the clock, red at 15% or less) and at the top of every OLED page. On USB with no battery the taskbar shows nothing and the OLED says USB. Read every 10 s, the same way The Shelf does.
* The desktop's music buttons are round (MP3 is a pill).

### 3.4.26: N for the next song

* A fourth music button, **N**, skips to the next song. The buttons now sit with the same space on each side, between the clock and the drop box.

### 3.4.25: music buttons on the desktop

* Between the clock and the drop box: **MP3** (play / pause a random song from the Music folder; lit while playing) and volume **-** / **+**. What they did shows in the drop box for a few seconds. Songs carry on to the next one by themselves.

### 3.4.24 / The Shelf 1.19.6: internet through your web browser

* When TidyOS Pro has no internet of its own (only its hotspot: at work, in the car), the web page showing its desktop now fetches things over that phone's or laptop's internet and passes them to the board:
  * **Settings > Check for updates** works on the hotspot: the page downloads the update from GitHub, and the board checks and installs it as usual. Keep the TidyOS page open while it runs.
  * TidyOS's own **web browser app** gets pages the same way, for sites that allow it (browsers only let a page fetch from sites that permit it; many don't). https sites are fine this way.
  * (The weather, sun times, headlines and city temperatures already came this way.)
* The Shelf's update page already downloads through your browser; it now says so. On its own Wi-Fi it works whenever your phone or laptop has cellular data or another connection.

### 3.4.23: start-up progress on the board's own screen

* While TidyOS Pro starts, the OLED shows what it's doing with a progress bar: getting ready, waiting for Wi-Fi, the start-up switches countdown, the SD card, the radio, the web screen... until its own pages take over.

### The Shelf 1.19.5 (dual boot): crash recorder

* If The Shelf crashes, the next start prints "[shelf] last run crashed: ..." with the reason and where, on the flasher's USB console (5 s and 30 s after start). The crash report itself goes to a port the USB console can't see.

### 3.4.22: works from its own hotspot (work, car)

* Music, game music and Full colour pictures now play when you're on the TidyOS hotspot, not only on home Wi-Fi.
* Once Wi-Fi has been set up, the hotspot no longer acts as a sign-in page: no gateway and no DNS, like The Shelf's own Wi-Fi. Phones stay joined without "Action needed" pop-ups, and keep using cellular data for the internet (the desktop's weather and news come over that).
* While a device is on the hotspot and home Wi-Fi is out of range, TidyOS stops searching for home Wi-Fi. Each search hopped the radio across channels and knocked phones off the hotspot. It looks again 30 s after the last device leaves.

### 3.4.21 / The Shelf 1.19.4: PRG hold to reset, Shelf status light

* TidyOS Pro: hold **PRG** (on any OLED page but the stopwatch). The OLED counts down. Let go after 6 s: forget Wi-Fi and restart in setup mode (hotspot at 10.1.1.250). Keep holding to 12 s, then let go: every TidyOS setting goes back to how it came. Files are kept.
* The Shelf on a V4 (and V3): the white LED is its status light again. It blinks while starting, breathes when ready, flickers while the SD card is busy and flashes fast during an update. (The V4-R8 has no LED free: GPIO 35 belongs to its PSRAM.)

### The Shelf 1.19.3 (dual boot): the right Shelf again

* Since 3.4.16 the dual-boot images and The Shelf's own updater carried an old Shelf build by mistake (it only looked for the SD card on CS 4, MISO 6, CLK 47, MOSI 48, and its own Wi-Fi was at 192.168.4.1). 1.19.3 is the current Shelf again: in-line SD card on 39-42 (either way round) and its own Wi-Fi at 10.1.1.250.

### 3.4.20: dropped files go straight to Incoming

* Files dropped on the desktop page go into the Incoming folder without a "where should it go?" question. The drop box says "Added to Incoming" with the file name for a few seconds (or "3 added to Incoming").
* Flasher, Copy files over USB: Connect now waits up to 90 s for TidyOS Pro, because opening the USB port can restart the board. Before, it gave up after 8 s with "The board didn't answer".

### 3.4.19: no restart while copying songs

* Copying several songs to the board from the browser no longer restarts TidyOS. A fast upload kept core 0 busy without a break, so the task watchdog fired; the file server now takes a short pause every 50 ms.

### 3.4.18: GPS switch

* New **GPS module** switch, in Switches and on the start-up switches screen (key 6). Off: TidyOS doesn't look for a GPS at start-up and leaves pins 39-42 alone. On by default; a change takes effect at the next start.

### 3.4.17: SD before GPS, hotspot at 10.1.1.250

* The in-line SD card (39-42) is now tried before looking for a GPS, the same order as The Shelf. Looking for a GPS switches pins 40 and 42 (the GPS wake and reset lines), which could stop the card answering afterwards. With the card on those pins, the GPS search is skipped.
* TidyOS's own hotspot is now at **http://10.1.1.250**, the same address as The Shelf's (phones get 10.1.1.100 and up).

### 3.4.16: no false GPS, steadier hotspot screen

* A GPS module now only counts once it sends two complete GPS messages with good checksums. Before, stray signals on GPIO 39 (for example an in-line SD card's MISO line) could look like a GPS, and the in-line SD card was then skipped.
* When TidyOS's screen is open in two browser pages at once (often a phone's hotspot sign-in sheet plus the browser), the page that's taken over now gets the message and stops, instead of the two taking turns every second.

### 3.4.15 / The Shelf 1.19.2: an in-line card module the other way round works too
- TidyOS Pro and the dual-boot Shelf also look for the card with the in-line module turned round (CS 39,
  MOSI 40, CLK 41, MISO 42): easy to do when it's fitted from the bottom of the board, and usually soldered by
  the time anyone finds out. The power jumpers are the same either way.

### The Shelf (dual boot) is now 1.19.1
- The Shelf that's installed next to TidyOS Pro was an old 1.10.12 copy that only looked for the microSD card on
  CS 4, MISO 6, CLK 47, MOSI 48. It's now the current Shelf for Heltec V4 (1.19.1, from
  The-Shelf-Heltec-V4900mhzMesh) with the dual-boot changes: it finds the card on the in-line wiring
  (CS 42, MOSI 41, CLK 40, MISO 39) or on CS 4 / 6 / 47 / 48, and has everything since 1.10 (Opus tags and covers,
  Meshed File Transfer, the faster screen...). Its updates come from this repo (`shelf/ota`).

### 3.4.14: free space you can see change
- The free space is shown to 10 KB (taskbar `flash 3.07M`, Files `3.07 MB free`): it was to 100 KB on the
  taskbar and whole MB in Files, so deleting a photo or a short song didn't change the number.
- Deleting, moving or cleaning up in Files updates the taskbar's free space straight away.
- Files says when a file couldn't be deleted (open in Music, or a folder that isn't empty) instead of quietly
  leaving it there.
- The start-up log says "TidyOS Pro only" for the board storage on the TidyOS-Pro-only layout.

### 3.4.13: Files: Clean up, and several files at once
- **Files → Clean up...** looks through all of the board's storage (or the SD card) and shows what can be given
  back, with sizes: unfinished copies (`.part`), an old update download, files left in Incoming, songs that also
  have a shrunk Opus copy, exact copies of other files (one of each stays) and empty folders, plus the five
  biggest files. Tick what should go and press **Clean up ticked** (Incoming isn't ticked to start with).
- **Files → Select several**: click files to tick them, then **Move to...** (pick the folder) or **Delete** does
  them all.

### 3.4.12: free space every 5 seconds
- The taskbar's free flash / SD space is looked up every 5 s (it was every 20 s); the RAM figures stay at every 20 s.

### 3.4.11: game music, take two
- MP3 / MIDI in a game go by what the browser says is playing, not what the board remembers: after the page was
  reloaded (or the board's idea was out of date) the first tap could send "pause" or "resume" to music that
  wasn't there, so nothing played until Next. Now a tap starts the song if it isn't really playing.
- If the browser hasn't started a song 5 s after it was asked, the board asks again (twice at most).
- The Music window's Play does the same after a page reload.
- The browser now reports music errors to the board's serial log ("[browser] audio: ..."), with "[music]" lines
  for what the board asked for, so a song that won't play can be tracked down.

### 3.4.10: smoother games, music starts first time
- **Smoother games in the browser**: the board now sends only the changed stretch of each screen row (it keeps a
  copy of what the browser has), not whole rows. A game's moving pieces are a few dozen pixels wide, so a frame
  is a fraction of the data it was, and StuntCopter and the other fast games keep up instead of jerking along.
- **The news line stops while a window covers it**, so it doesn't take drawing time and Wi-Fi from the game.
- **MP3 / MIDI in a game play on the first tap**: the music file server is listening before the browser is told
  to fetch the song (it wasn't yet, so nothing played until Next).
- **Crash reports survive the boot menu**: after a crash a dual-boot board starts TidyOS through the boot menu,
  which hid the "last run crashed" report. It's shown (serial console, Task Manager, crash.txt) again.

### 3.4.9: the map's white box, properly gone
- 3.4.8 stopped the panel blanking the map, but the desktop underneath still painted its own colour over the
  panel's area first on every 20 s redraw, so the map could still flash as a plain box. The desktop now paints
  around the panel and leaves its area to the panel.

### 3.4.8: no more glitch lines in the map
- The desktop panel redraws itself every 20 s. It used to blank the whole panel first, so the world map was empty
  for a moment while it was drawn again, and the browser could catch a line of it half-done (more likely with
  the news line moving all the time). The map is now drawn over in place, never blanked.
- The news line never draws past the panel's right edge.

### 3.4.7: music fixes
- **Music in a game goes on to the next song** for Opus and MP3 too. With the Music window closed, the games'
  music bar only looked for another MIDI file when a song ended, so other music stopped after one song.
- **The Music list highlights the song that's playing**: Next, Prev, shuffle and the next song after one ends
  move the highlight (and scroll the list to it) instead of leaving the old song lit.

### 3.4.6: browser screen keeps up with fast games
- `screen.cpp`: the search for changed screen rows carries on from where the last batch stopped and wraps round,
  instead of starting at the top every time. Fast games (StuntCopter, Paradrop...) change ~100 rows a frame, so the
  rows near the top changed again before the search got lower down, and the browser kept old bits of the picture
  (half a wagon, a stuntman's feet under a cloud).

### Flasher: TidyOS Pro only with 12.4 MB for files
- New tick box under the systems, shown when The Shelf is unticked: **Give The Shelf's space to my files**. It
  installs a TidyOS-Pro-only partition table (`source/TidyOSPro/partitions-tidyonly.csv`): The Shelf's program
  slot, its web app and the old 1 MB area become one **12.4 MB** file area instead of 7.4 MB. The quick pick
  "TidyOS Pro only" ticks it. Same names in the table, so the same TidyOS Pro and boot menu run on either layout.
- Before writing, the flasher reads the board's partition table. Changing layout wipes the files kept on the
  board (never the microSD card); installing again on the same layout keeps them. Going back to dual boot needs
  the bootloader + partition table ticked, and wipes the board's files once.
- `tools/parts.py` makes `firmware/pro-<board>-boot-tidyonly.bin` (bootloader + that table) with `tools/ptable.py`.

### 3.4.5: copy files over USB, a real update bar, ticking seconds
- **Copy files over USB** (the flasher page): connect with the USB cable, pick the SD card or the board's own
  storage, open a folder (Music and Pictures are one click) and drop files in; Save and Delete what's there. Works
  through the same port as the serial console, while TidyOS Pro runs (`~` lines in, `\x02TF` answers out:
  `source/TidyOSPro/usbfiles.cpp`). Roughly 50-150 KB a second.
- **The update bar** follows the real download and shows its percent beside it (no creeping ahead, no icon
  parade slowing the download); the KB count stays on one line.
- **Clock**: the taskbar's seconds tick every second again (the colon stays steady). The OLED clock page ticks too.
- **Files → Move to...**: pick a file, then the folder (and a new name if you like) to move it to. Between the SD
  card and the board's own storage it's copied, checked, and only then deleted from where it was.
- **News** is back to one line, every headline in turn, still sliding smoothly; the wind and storm graphs are
  their full height again.

### 3.4.4: 900 MHz Coms mesh, two lines of news, a calmer bar
- **Messages → Coms mesh 2.4 + 900**: on a Heltec V4 the Coms mesh now runs over **900 MHz LoRa** as well as
  2.4 GHz, with the Shelf boards' rules: 915.0 MHz, 125 kHz, SF9, CR 4/5, sync word 0x12, the same packets, every
  board a relay (each message passed on once, ids remembered so nothing loops), receipts and catch-up. TidyOS
  carries messages between its 2.4 GHz and 900 MHz sides, like a Shelf Heltec. The radio listens whenever no RF
  app (RF Sweep, Meter Traffic...) is using it, and checks the channel is clear before it sends. The button under
  the board list turns 900 MHz on and off; the status line shows what it has sent and heard.
- **News**: two lines of headlines above the map, different ones on each, sliding smoothly (2 pixels every
  0.1 s) instead of jumping a letter at a time. (The wind and storm graphs are a little shorter to make room, and
  NWS alerts take one line: the first alert and how many more.)
- The bar: **PRO** is the same size as **TidyOS**; the Optimize button is gone; the clock's colon no longer blinks
  and its seconds move every 5 s; the RAM figures update every 20 s.
- A desktop lookup that fails (weather, wind and the rest, headlines, cities) tries again after about 30 seconds.

### 3.4.3: updates in colour
- Check for updates no longer switches the screen to black & white: the V4's screen lives in PSRAM, so colour costs
  the download nothing. (A board without PSRAM still drops to black & white for the update and switches back after.)
- **NEWS page on the OLED**: the desktop's headlines, one at a time (a new one every 5 s), between Storms and 900 MHz.
- **The desktop fills in faster**: weather, wind/humidity/storms, space weather, city temperatures and headlines are
  all looked up the moment the browser connects (wind and the rest used to wait up to 30 s), the location is looked
  up once instead of twice, and a slow location service is given up on after 5 s. They then refresh every
  10 minutes (weather, field data) and 15 minutes (headlines, cities) instead of every 25-30, and a failed lookup
  tries again after a minute.

### 3.4.2: Music and Pictures folders
- The board's own storage gets **Music** and **Pictures** folders, and the Music app plays from Music when there's no
  SD card. Songs dropped on the desktop start in Music, photos in Pictures (on the card, or the board without one).
- Flasher, *Make room*: files go into **Music** and **Pictures** lists with tick boxes, Select all, **Remove** per file
  and Remove selected. Already-Opus files (`.opus`) can be dropped in as they are. **Download selected (.zip)** gives
  Music and Pictures folders, ready to copy onto an SD card.

### 3.4.1: updates work again, rolling news
- **Fixed: Check for updates rebooted the board straight away.** Update mode gave Bluetooth's memory back a second
  time; that fails on the S3 and aborts, so every update went round the boot menu and back. Boards on 3.4.0 or older
  need this version installed once from the flasher; after that, Check for updates works (with or without a card).
- **Rolling news headlines** on the desktop panel, in a line above the map: NPR (BBC News if NPR can't be reached),
  moving along a letter at a time, refreshed every 30 minutes while the browser is open.

### 3.4.0: shrink music and photos
- **Drop music on the desktop and it's shrunk on the way in**: MP3 / M4A / FLAC / WAV / OGG become **Opus 48 kbps**
  (`.opus`), about a third of the size of a 128 kbps MP3 and sounding about the same. Title, artist, album, track and the
  **cover picture** are kept. The browser does it (Chrome's WebCodecs on https pages; on the board's own http page,
  libopus from [opus-recorder](https://github.com/chris-rudmin/opus-recorder) via the jsDelivr CDN).
- **Photos too**: JPEG / PNG / iPhone **HEIC** become a 1280 px JPEG that TidyOS Pro's Pictures app can open on the
  board (HEIC is read with [heic-to](https://github.com/hoppergee/heic-to), loaded from the CDN only when needed).
  A 12-megapixel photo goes from ~3.5 MB to ~200 KB.
- Hold **Alt** while dropping to send files as they are.
- **Dropping works without an SD card** now: files land in the board's storage (`TidyPro/Incoming`).
- Music and Files play `.opus`.
- Web flasher: **Make room: shrink your music and photos**, with before/after, per-file savings, and a zip of the
  results. It can also make AVIF photos for The Shelf ([jsquash/avif](https://github.com/jamsinclair/jSquash)).
  The shrinkers are in `shrink/` (`shrink.js`, `photo.js`, `zip.js`).

### 3.3.6
- The browser page reconnects **every second** when the board restarts (boot menu, update, Restart), and says how long
  it's been, so it picks up the update screen and the new TidyOS Pro straight away.

### 3.3.5
- **Updates say what they're doing.** In update mode the OLED shows each step (joining Wi-Fi, checking GitHub,
  downloading with a progress bar, installing) and any error, and every step goes to the serial console as
  `[update] ...`. If the update stops, the reason appears on the desktop after the restart ("Update: ...").

### 3.3.4
- OLED Kp graph: a dotted line at each midnight with the day's name under it, "now" at the right end,
  and "5" beside the dotted storm line.

### 3.3.3
- Bottom bar logo much tighter: PRO right against "TidyOS", the blue just around the letters, a closer box.

### 3.3.2
- The update question fits its box again: "Restart and check GitHub? Needs Wi-Fi (no SD card needed)."

### 3.3.1
- OLED Sun + HF page: the Kp graph has a label ("Kp last 4 days, dots = storm").

### 3.3.0
- **One storage area, 7.4 MB instead of 1 MB.** With no SD card, TidyOS Pro now keeps its files in the board's big
  `ffat` area, the same one The Shelf uses, in its own **TidyPro** folder so the two never mix. It's set up the first
  time if it was never used. Anything in the old 1 MB area is copied over at the first start (once). The Files app,
  the bottom bar, the OLED, Task Manager and the Wi-Fi file sharing all use it. If that area ever won't open,
  TidyOS Pro falls back to the old 1 MB area.
- Web flasher: "Wipe the board's file storage" covers both systems' files, and the big number shows the shared 7.4 MB.

### 3.2.5
- Settings → Check for updates says the V4 needs no SD card (the old message was from the classic ESP32).
- Bottom bar logo: a snug box, with PRO right after "TidyOS" and its blue badge just around the letters.
- Web flasher: the big number is now **room for your files**, split by who can use it (TidyOS Pro 1 MB, The Shelf
  7.4 MB), instead of every unused part of the 16 MB, much of which can't hold files.

### 3.2.4
- When an app saves a file, its **title bar says so**, e.g. "Wi-Fi Scan  -  saved wifi-20261121-153000.csv":
  RF Sweep, Meter Traffic, Site Survey (CSV log and KML), GPS waypoints, Wi-Fi Scan and Nic Sweep.

### 3.2.3
- **Updating crashed** (it restarted, crashed and restarted): the update ran in Arduino's loop task, whose 8 KB stack
  is too small for HTTPS with certificate checks on the S3. It now runs in its own task with a 24 KB stack, and the
  loop task (where the desktop and every app run) has 16 KB instead of 8 KB.
- **Wi-Fi Scan: "Open in Tidy"** saves the scan as `rf/wifi-*.csv` (network, signal, channel, frequency, security,
  BSSID, time, and position if the GPS has a fix) and opens it in Tidy.
- **Desktop HF bands** in one row across the Kp graph (Good filled in, Poor in red), with a label under the graph
  saying what it shows.
- **Web flasher: Erase the whole board** (step 7), the same as esptool's erase_flash.

### 3.2.1
- Bottom bar: the Optimize button now sits in front of the RAM figures.

### 3.2.0
- **The crash is found**: FabGL's drawing task (it draws every window, line and letter of the desktop) had a
  1,200-byte stack. On the ESP32-S3, with the screen in PSRAM, that ran out a few seconds after the desktop appeared
  ("Stack canary watchpoint triggered" on core 0, with a blank task name). It now has 6 KB, and FabGL's tasks have names
  (`fabgl-draw`, `fabgl-ui`...) so a log would show them. The stack watch no longer warns about ESP-IDF's idle/ipc tasks.
- **The OLED shows rolling pages** of the desktop's live data, changing every 30 s: address, clock, weather (with the
  4-day temperature graph), wind + humidity, sun + HF bands (160 to 10 m, Kp graph), storms + NWS alerts, 900 MHz
  (a live mini spectrum when RF Sweep runs, plus GPS), system (RAM, PSRAM, flash, Wi-Fi, uptime) and a stopwatch.
  PRG: tap once to hold a page (again to roll), tap twice for the next page; on the stopwatch hold 1 s to start/stop,
  3 s to reset. The stopwatch keeps running on the other pages (shown top right). It dims after 30 minutes without a press.
- **Address page** first after start-up: the IP address and name on your Wi-Fi, or the hotspot's name, password and
  address in full (the old screen cut "10.1.1.250" off).
- **Bottom bar**: the TidyOS logo in a rounded box with a blue PRO badge, then the version and the board's last 4 ID digits;
  the clock shows seconds when there's room, and its colons blink every second.

### 3.1.4
- **HF bands now include 160 m and 80 m.** They're rated from whether it's dark where you are (the sun's height at
  the weather lookup's location) and Kp, since solar flux doesn't help them: 160 m opens after dark when Kp is low,
  80 m is *Local* (near-vertical, a few hundred miles) by day and *Good* at night when it's quiet.
- **Boot menu: 5 seconds**, with the system used last already picked.
- **Serial console on the web flasher page** (115200 baud): timestamps, crash lines in red, Restart board, Save log,
  and a line to send text. The flasher's buttons close it when they need the port, and it opens again after an install.

### 3.1.3
- The bottom bar also shows free storage: the SD card's when it's on, otherwise TidyOS Pro's own flash area.
- Web flasher: after installing, the board now restarts into its program by itself. (esptool-js's own reset left
  the V4 in download mode with a dark screen until the reset button was pressed.)

### 3.1.2
- **Updates without an SD card**: with no card, TidyOS Pro saves the download in the board's own storage (the 7.4 MB
  `ffat` area The Shelf uses; it's set up first if it was never used, and needs about 3 MB free). The boot menu now
  looks for `/tidyos/update.bin` there as well as on the card, checks its SHA-256 and installs it.
  Boards from before 3.1.2 need one install from the web flasher with *TidyOS Pro* and *Boot menu* ticked.
- **Boot menu**: 5 seconds instead of 30, with the system used last already picked.
- The bottom bar shows the two kinds of free RAM separately: the chip's own and the PSRAM.

### 3.1.1 fixes (first run on a real V4)
- **Crash when opening apps** ("Stack canary watchpoint triggered" on core 0): the background tasks kept the stack
  sizes from the classic ESP32, and the S3 needs more. Every core-0 task now has about twice the stack.
  A new *stackwatch* task prints a warning on the serial port (115200) if any task gets within 700 bytes of the end of
  its stack, and a table of every task's spare stack 30 seconds after start-up.
- **Status light**: it was driving GPIO 2, which on the V4 is the radio's front-end enable, not an LED. It now uses the
  V4's white LED (GPIO 35). The V4-R8 has no LED free for it (GPIO 35 is its PSRAM), so there it stays off.
- Task Manager shows the PSRAM.

[![The TidyOS Pro install page](docs/screenshot.png)](docs/screenshot-full.png)

*The install page ([full page](docs/screenshot-full.png)).*

TidyOS Pro turns a Heltec WiFi LoRa 32 V4 into a pocket field computer for 900 MHz and AMI work.
It's [TidyOS](https://github.com/hardwaremack-prog/TidyOS-ESP32) ported to the V4's ESP32-S3, with
tools for its SX1262 radio and GNSS module. The TidyOS desktop shows up in any browser on the same Wi-Fi.
It can also share the board with [The Shelf](https://github.com/hardwaremack-prog/The-Shelf-Heltec-V4900mhzMesh) (**dual boot**, with a 5-second boot menu on the OLED).

> Status: 3.0.0, first release. Everything builds for the V4 and V4-R8. It has **not been run on a board yet**,
> and the RF numbers haven't been compared with a calibrated instrument. Treat readings as indications until they have been.

## What's in it

| App | What it does |
|---|---|
| **RF Sweep** | Live 902–928 MHz spectrum: 129 channels of 200 kHz (902.2–927.8 MHz, the Wi-SUN/FAN plan Riva and most 900 MHz AMI meshes use). Average, peak and peak-hold traces, a waterfall, noise floor, busy threshold, an **OVERLOAD** warning, and a CSV export. Antenna gain, attenuator and front-end correction are set here. |
| **Meter Traffic** | *Whole band*: how busy each channel is (bar chart, busiest-channel list). *One channel*: a burst counter: bursts per minute, % of time on the air, burst-length histogram, latest bursts. It counts energy above the noise, so it works for OpenWay CENTRON (hw 2 / 3 / 3.1, ICM) and Riva alike. It doesn't decode or read anything. |
| **Site Survey** | GPS-tagged logging every N seconds to `SD:/rf/survey-*.csv` (time, fix, lat/lon, sats, noise floor, peak, busy %, band power density). Draws the points as a map coloured by noise / busy / peak, with "Mark here" notes, and exports **KML** for Google Earth. |
| **RF Exposure** | An **estimate** of power density against the FCC limits, from the band's readings: band average, the strongest moment, other units (µW/cm², W/m², V/m). Plus a calculator: transmitter power, duty cycle, gain, distance and count → mW/cm² and % of the limit. |
| **GPS** | Fix, position (decimal and DMS), altitude, satellites, HDOP, speed, UTC, and waypoints to `SD:/rf/waypoints.csv`. With no internet, the clock sets itself from GPS. |
| **Wi-Fi Scan** | 2.4 GHz networks: signal, channel, security, BSSID, and the quietest of channels 1/6/11. |
| **Clone** | Copies this board's firmware onto another V4 by wire (see below). |
| **The Shelf** | Restarts into The Shelf on a dual-boot board. |
| Nic Sweep, Ping, Tidy, Neat, Files, Calculator, Browser, Messages, Clock, Screenshot, Git-Store, Settings, Task Manager, Switches | From TidyOS. Tidy opens the CSV files the RF tools save. |
| Ready, Steady, Rugged, Latch, Games, Doodle, Pictures, Music, HTML Games, HTML Programs | From TidyOS, in the full build. Pick them in the flasher. |

**The desktop's right-hand panel** shows the time and weather, then **field conditions**, each with a 4-day graph (one point every 3 hours):
- **Wind and gusts** (mph, with direction) and **humidity** (with dew point)
- **Storms**: CAPE (how unstable the air is), with the hours the weather model had thunderstorms marked in red
- **Tropo ducting**: the refractivity gradient from the ground to the 925 hPa level, in N/km. About −40 is normal, −79 or lower is enhanced, and −157 or lower is a **duct**, when 900 MHz signals (meters, interference) carry much further than usual. Worked out from Open-Meteo's forecast model for your location.
- **Space weather**: solar flux (SFI), Kp with a 4-day bar graph, the GOES X-ray class, and an HF band estimate for 160, 80, 40, 20, 15 and 10 m (day/night and Kp for the low bands, the usual SFI/Kp rule of thumb for the high ones)
- **NWS alerts** for your location (US)
- **This board's own** 900 MHz noise floor, band occupancy and GPS status, live
- A day/night **grey-line map** with a you-are-here marker

There's no free live lightning-strike feed a browser can read, so the storm graph uses the weather model's thunderstorm hours and CAPE instead.
The page in your browser looks all of this up (Open-Meteo, NOAA SWPC, api.weather.gov), not the board.

The top bar of the browser page has **Restart** (back to the boot menu) and **☾ Deep Sleep**. Deep sleep switches off the radio, its amplifier, the GPS and the OLED too; press **PRG** to wake it.

The board's **OLED** shows the desktop's address (or the setup hotspot and its password), the noise floor and the GPS status. It sleeps after 5 minutes; press PRG to wake it.

## How the exposure estimate works

The SX1262 reports the power at its input in dBm. Corrected for the front-end gain and any attenuator, that's the
power *P* the antenna picked up. An antenna of gain *G* has an effective area *Ae = G·λ²/4π* (≈135 cm² for 2 dBi at
915 MHz), so the power density of the wave that delivered it is *S = P / Ae*. RF Exposure sums every channel's
time-averaged power, and compares it with **FCC 47 CFR 1.1310** at 915 MHz: *f*/1500 = **0.61 mW/cm²** for the general
public and *f*/300 = 3.05 mW/cm² for workers (averaged over 30 minutes).

It's an estimate, good to a few dB. The antenna's real gain, the V4's own front-end (LNA) gain, direction,
polarisation and reflections all move it. The receiver also tops out a little below 0 dBm, so right beside a meter it reads
**low**: the apps say *OVERLOAD* when they see that. To calibrate it, compare with a known signal or a reference instrument,
then enter the difference as *Front-end dB* in RF Sweep. For a number you can sign off, use a calibrated broadband RF
exposure meter. For scale: a 1 W radio transmitting constantly gives ≈0.013 mW/cm² at 1 m (2% of the public limit), and AMI
radios transmit in short bursts.

## Hardware

| | Heltec V4 pins | Notes |
|---|---|---|
| GNSS module | GNSS header: RX 39, TX 38, EN 34, RST 42, WAKE 40, PPS 41 | Detected at start-up, 9600 or 115200 baud, either way round. |
| microSD (3.3 V SPI module) | **CS 4, MISO 6, CLK 47, MOSI 48**, 3V3, GND | Moved off 39–42 because the GNSS module uses them. With no GNSS module fitted, The Shelf's in-line wiring (CS 42, MOSI 41, CLK 40, MISO 39) also works. FAT32. |
| 900 MHz antenna | LoRa connector | **Receive only**: TidyOS Pro never transmits. |
| Clone cable (optional) | 43 → other board's RX, 44 ← its TX, GND (+ 3 → RST, 33 → GPIO 0 for auto reset) | V4-R8: no auto reset (GPIO 33 is its PSRAM). |

The V4-R8 (8 MB PSRAM) has its own builds. On the R8, TidyOS Pro leaves GPIO 33–37 and 40 alone.

**Heltec V3**: the same OLED, PRG, LED (35), Vext (36), battery (1 / 37) and SX1262 pins as the V4. No GNSS header and no
front-end module. microSD on the in-line pins (CS 42, MOSI 41, CLK 40, MISO 39) or on CS 4, MISO 6, CLK 47, MOSI 48.

```
Heltec V3, 8 MB (partitions-v3.csv)
0x010000  menu    448 KB  TidyBoot: starts TidyOS Pro, installs updates
0x080000  tidyos    3 MB  TidyOS Pro
0x380000  ffat    4.4 MB  the board's file storage
```

## Dual boot

The flash is split so both systems fit (16 MB):

```
0x010000  menu    448 KB  TidyBoot: the boot menu, and the installer for updates
0x080000  tidyos    3 MB  TidyOS Pro
0x380000  shelf     2 MB  The Shelf (dual-boot build)
0x580000  www       2 MB  The Shelf's web app
0x780000  tidyfs    1 MB  TidyOS Pro's internal storage (when there's no card)
0x880000  ffat    7.4 MB  The Shelf's internal storage
```

TidyOS Pro on its own can use a second layout (`partitions-tidyonly.csv`, picked in the flasher with
*Give The Shelf's space to my files*):

```
0x010000  menu    448 KB  TidyBoot
0x080000  tidyos    3 MB  TidyOS Pro
0x380000  ffat   12.4 MB  the board's file storage (all TidyOS Pro's)
```

Every start goes through **TidyBoot**. It shows *Start which one?* on the OLED for 5 seconds: a short press of PRG
switches, holding PRG for a second starts the one shown, and if you do nothing it starts the last one used. With only one
system installed it starts that one straight away, and waking from deep sleep goes straight back to the last one.
Each system sends the next restart back to the menu.

TidyBoot also installs updates. TidyOS Pro downloads its update to the microSD card, and The Shelf saves its update on the
card or in its internal storage. On the next restart TidyBoot checks the file and writes it into the right slot.
The dual-boot Shelf build (`source/ShelfDual`) is The Shelf 1.10.12 with three small changes, all behind
`SHELF_DUALBOOT`: it returns to the menu on restart, saves updates for TidyBoot to install, and uses the 4/6/47/48 card wiring.

**Back up first.** The web flasher's *Back up board* reads the whole flash into one file, and *Restore* puts it back
exactly as it was. That includes a backup made while the board only had The Shelf.

## Clone

Clone writes this board's bootloader, partition table, boot menu, TidyOS Pro and (optionally) The Shelf and its web app
onto another V4 through that board's ROM bootloader (the same protocol esptool uses). It runs at 921600 baud and checks every
piece with MD5. It doesn't copy settings, Wi-Fi passwords or files: the other board starts fresh, at the boot menu.

## Building

```
./build.sh
```

You need **arduino-cli** with the **esp32 core 2.0.17** (FabGL doesn't build on 3.x), and these libraries:
- **RadioLib 7.1.2**, **U8g2** and **NimBLE-Arduino 1.4.3**
- **ESPAsyncWebServer 3.3.23** and **ArduinoJson 7** (for The Shelf)
- `libraries/FabGL`, included here: FabGL 1.0.9 with TidyOS's patches and the ESP32-S3 port (see its README)

`build.sh` compiles TidyOS Pro and TidyBoot for the V3, V4 and V4-R8, and the dual-boot Shelf for the V4 and V4-R8
(`ONLY="v3"` builds one board; `SKIP_SHELF=1` reuses The Shelf from the dual-boot images already in `firmware/`).
If downloads.arduino.cc can't be reached, the esp32 core can be cloned by hand into `~/Arduino/hardware/espressif/esp32`
(then `tools/get.py`); `build.sh` notices and uses it. It fetches The Shelf's web app
image from The Shelf's installer site, and `tools/package.py` writes `firmware/`, the install manifests, the updater manifests
(`manifest-pro*.json`) and `shelf/ota/` (what a dual-boot Shelf updates from).

Source map (in `source/TidyOSPro`):

| File | What's in it |
|---|---|
| `rf_engine.cpp` | SX1262 + front-end as a receiver, the sweep, one-channel listening, and the GNSS (NMEA) reader |
| `rf_apps.h` | RF Sweep, Meter Traffic, Site Survey, RF Exposure, GPS, Wi-Fi Scan, Clone UI, The Shelf switch |
| `clone.cpp` | The ESP32-S3 ROM-loader client for Clone |
| `oled.cpp` | The board's own screen |
| `desk_panel.h` | The desktop's right-hand panel, including the field conditions |
| `screen.cpp` | The browser page (and the JavaScript that fetches the weather, space weather and alerts) |
| `../TidyBoot/boot.cpp` | The boot menu and update installer |

Everything else is TidyOS 2.6.8. The rest of its notes (apps, printing, Git-Store and so on) are in the
[TidyOS README](https://github.com/hardwaremack-prog/TidyOS-ESP32#readme).

## Credits and licence

Based on TidyOS and The Shelf for Heltec. Uses [FabGL](https://github.com/fdivitto/FabGL) (GPL v3),
[RadioLib](https://github.com/jgromes/RadioLib) (MIT), [U8g2](https://github.com/olikraus/u8g2) (BSD-2),
ESPAsyncWebServer (LGPL-3) and ArduinoJson (MIT). The browser page uses data from Open-Meteo, NOAA SWPC and the US National Weather Service.
