# Testing the new GPU driver on a Cedarview computer

Thank you for helping! This test checks whether our new open-source graphics driver
recognizes and runs the graphics chip in your computer. You don't need to know anything about
Linux, and you don't need to install anything.

## Will this work on my computer?

It's for computers with an **Intel Atom D2500, D2550, D2700, N2600 or N2800** processor
("Cedarview", Intel GMA 3600 or 3650 graphics). Many 2012-era netbooks and small desktop
boards have one.

## Is it safe?

Yes. The test runs entirely from a USB stick:

- It doesn't install anything and doesn't change your computer's settings.
- It sets your computer's internal disk to read-only before it does anything, and never
  writes to it.
- When you unplug the stick and restart, your computer is exactly as it was.

The only thing it writes is one small results file, onto the USB stick itself.

## What you need

- The Cedarview computer, plugged into power.
- A USB stick of 1 GB or more. **Everything on it will be erased.**
- Another computer (Windows, Mac or Linux) to prepare the stick and to send us the
  results.

## 1. Download the test

1. Open the download link from the GitHub issue where you were asked to test. You need to
   be signed in to GitHub for the download to start.
2. It downloads a **`.zip`** file. Open it (on Windows, double-click it, or right-click it
   and choose **Extract All**) and take out the file that ends in **`.img.gz`**, for
   example `sgx545-cedarview-kit-97baf09.img.gz`.
3. Don't unzip the `.img.gz` file itself. The flashing program reads it as it is.

## 2. Put it on the USB stick

Use one of these free programs:

- **balenaEtcher** (Windows, Mac, Linux), from [etcher.balena.io](https://etcher.balena.io):
  1. Click **Flash from file** and pick the `.img.gz` file from step 1.
  2. Click **Select target** and pick your USB stick. Make sure it's the stick, not
     another drive.
  3. Click **Flash!** and wait until it says it's done.
- **Rufus** (Windows), from [rufus.ie](https://rufus.ie):
  1. Under **Device**, pick your USB stick.
  2. Click **SELECT** and pick the `.img.gz` file.
  3. Click **START**. If Rufus asks how to write the image, choose **DD Image mode**.

## 3. Start the Cedarview computer from the stick

1. Turn the Cedarview computer off. Plug in the USB stick.
2. Turn it on, and right away press the key for the **boot menu** a few times. It's
   usually **F12**, **Esc**, **F10** or **F11**. The start-up screen often shows which one.
3. In the menu, pick the USB stick.
4. A menu with four entries appears. Pick **1** (it starts by itself after 10 seconds).
   Entries 2 to 4 run the same test with settings for screens that go dark with 1.

If the computer starts its normal system instead, restart it and try another key. On some
computers you have to turn on "Boot from USB" in the BIOS setup first (often **F2** or
**Del**).

## 4. Wait a few minutes

The screen shows five steps as they run. You don't have to do anything. At the end you
see **DONE. Thank you!** and the name of the results file.

Then type **p** and press **Enter** to turn the computer off, and unplug the stick.

At the end the screen also asks whether you could see it. Please answer that in your
comment.

**If the screen goes dark** (some Cedarview panels do with Linux):

1. Wait about five minutes. The test keeps running and saves its results.
2. Hold the power button to turn the computer off.
3. Start from the stick again (step 3) and pick the next menu entry: 2, then 3, then 4.

Each try saves its own results file. Send all of them, and tell us which entries left the
screen dark, and whether it was fully dark (no backlight) or lit but black.

## 5. Send us the results

1. Plug the USB stick into your other computer.
2. Find the drive named **SGXKIT**. It is the whole USB stick: the test system and the
   results are both on it.
   On Windows, if no SGXKIT drive shows up, unplug the stick and plug it in again.
3. On SGXKIT, find the files whose names start with **`results-`** and end with
   **`.tar.gz`**: one per try (the `-e1-`, `-e2-`, ... in the name is the menu entry).
4. Attach them to a comment on the GitHub issue where you were asked to test. You can drag
   them straight from the SGXKIT drive into the comment box. You don't need to copy or unzip
   them first. Please also say which computer model you used.

The files contain only technical information about your computer's hardware and what the
driver reported. Serial numbers and network addresses are removed. No personal files are
ever read.

## What happens next

The test first asks the graphics chip what it is. If our driver knows the chip, it then
starts it and draws a few small test images, and checks that it recovers from a
deliberate error. Nothing is shown on the screen while it does this. Each kit you test
lets us support more of your computer. The next ones will run desktop graphics.
