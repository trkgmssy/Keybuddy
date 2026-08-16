# Getting Started (No Coding Experience Needed)

This guide walks you through everything, click by click, to get this project running on your board. You don't need to know how to code — just follow the steps in order.

If you want to understand the general concepts (what a "sketch" is, what a microcontroller is, etc.) in more depth than this guide covers, [Random Nerd Tutorials' ESP32 getting-started guide](https://randomnerdtutorials.com/getting-started-with-esp32/) is a well-regarded, beginner-friendly place to learn more. You don't need to read it first — this guide is self-contained.

## What you'll need

- Your Waveshare ESP32-S3-Touch-AMOLED-1.64 board
- A USB-C cable (must be a real **data** cable, not a charge-only one — if you're not sure, any cable that came with a phone or that you've used to transfer files before will work)
- A Windows, Mac, or Linux computer
- About 20-30 minutes

---

## Step 1 — Install Arduino IDE

This is the free program you'll use to load code onto your board.

1. Go to **[arduino.cc/en/software](https://www.arduino.cc/en/software)**
2. Download the version for your computer (Windows/Mac/Linux) and install it like any other program.
3. Open it once to make sure it launches.

## Step 2 — Add ESP32 support

Arduino IDE doesn't know about ESP32 boards by default — this teaches it how.

1. Open Arduino IDE. Go to **File → Preferences** (on Mac: **Arduino IDE → Settings**).
2. Find the box labeled **"Additional boards manager URLs"** and paste this into it:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click **OK**.
4. On the left sidebar, click the **Boards Manager** icon (it looks like a stack of squares).
5. Type **esp32** into the search box.
6. Find **"esp32 by Espressif Systems"** and click **Install**. This takes a few minutes — just wait for it to finish.

## Step 3 — Install two required libraries

"Libraries" are pre-made pieces of code this project depends on.

1. On the left sidebar, click the **Library Manager** icon (looks like an open book).
2. Search for **TFT_eSPI**, find the one by **Bodmer**, and click **Install**.
3. Search for **TJpg_Decoder**, find the one by **Bodmer**, and click **Install**.

## Step 4 — Download this project

1. On this project's GitHub page, click the green **Code** button, then **Download ZIP**.
2. Find the downloaded ZIP file on your computer and unzip it (double-click it, or right-click → Extract).
3. Inside, you'll find a `Keychain2` folder. That's the whole project.

## Step 5 — Open the project and check the board settings

1. Open Arduino IDE, then **File → Open**, and select `Keychain2.ino` inside the folder from Step 4.
2. Plug your board into your computer with the USB-C cable.
3. At the top of the window there's a board selector dropdown. Click it, then **Select other board and port**.
4. Type **AMOLED-1.64** into the search box and pick the Waveshare ESP32-S3-Touch-AMOLED-1.64 entry. Then pick the port that appeared on the right (it'll be the only new one that showed up when you plugged in the board).
5. Now go to the **Tools** menu at the top and set these three things exactly:
   - **Flash Size → 4MB**
   - **Partition Scheme →** any option with **"spiffs"** in its name (for example *"Default 4MB with spiffs"*)
   - **PSRAM → OPI PSRAM**

   These three matter — the project won't run correctly if they're set differently.

## Step 6 — Install the image upload tool

Your sword picture gets uploaded separately from the code, using a small add-on tool.

1. Go to **[this tool's release page](https://github.com/earlephilhower/arduino-littlefs-upload/releases)** and download the latest file ending in **.vsix**.
2. Find the `.arduinoIDE` folder on your computer:
   - Windows: `C:\Users\<your name>\.arduinoIDE`
   - Mac/Linux: `~/.arduinoIDE`
3. Inside it, create a new folder called `plugins` if one doesn't already exist.
4. Move the downloaded `.vsix` file into that `plugins` folder.
5. Completely close Arduino IDE (not just the window — quit the app) and reopen it.


## Step 7 — Upload your image to the board

1. Make sure the Serial Monitor tab (if open anywhere in Arduino IDE) is closed.
2. Press **Ctrl+Shift+P** (Mac: **Cmd+Shift+P**) to open the Command Palette.
3. Type **"Upload LittleFS"** and click the option that appears (**"Upload LittleFS to Pico/ESP8266/ESP32"**).
4. Wait for it to finish — you'll see progress text at the bottom of the window.

## Step 8 — Upload the code

1. Click the **→ (Upload)** button at the top-left of the Arduino IDE window (it looks like a right-pointing arrow).
2. Wait — this takes a minute or two the first time. You'll see "Done uploading" when it's finished.

## Step 9 — You're done!

Your board should now show the animated sword. Try:
- **Tap the BOOT button once** — wakes the screen if it's gone to sleep
- **Tap it twice quickly** — shows a charging animation
- **Hold it down for 1-2 seconds** — turns the screen off (tap once to turn back on)

---

## Something not working?

- **Screen stays black / nothing happens:** double-check all three Tools settings in Step 5 are exactly right — this is the single most common cause of problems.
- **Upload fails with a port/connection error:** close Serial Monitor if it's open, unplug and replug the USB cable, then try again.
- **Sword doesn't appear but everything else works:** re-check Step 7 — the image must be named exactly `sword0.jpg` and be exactly 102×249 pixels.
- Still stuck? Check the **Known limitations** and settings reference in the main [README](README.md) — or open an Issue on this repo describing exactly what you see (including anything shown in Serial Monitor).
