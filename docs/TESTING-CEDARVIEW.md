# Testing the new GPU driver on a Cedarview computer

Thank you for helping! This test checks whether our new open-source graphics driver
recognizes the graphics chip in your computer. You don't need to know anything about
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

On the [Releases page](https://github.com/The1TrueJoe/sgx545-ce/releases), find the newest
release named **Cedarview test kit**. Download the file that ends in **`.img.gz`**, for
example `sgx545-cedarview-kit-v1.img.gz`. You don't need to unzip it.

## 2. Put it on the USB stick

Use one of these free programs:

- **balenaEtcher** (Windows, Mac, Linux), from [etcher.balena.io](https://etcher.balena.io):
  1. Click **Flash from file** and pick the `.img.gz` file.
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
4. A menu titled **SGX545 GPU test** appears and starts by itself after 5 seconds.

If the computer starts its normal system instead, restart it and try another key. On some
computers you have to turn on "Boot from USB" in the BIOS setup first (often **F2** or
**Del**).

## 4. Wait about two minutes

The screen shows five steps as they run. You don't have to do anything. At the end you
see **DONE. Thank you!** and the name of the results file.

Then type **p** and press **Enter** to turn the computer off, and unplug the stick.

If the screen stays black or the computer stops responding for more than five minutes,
hold the power button to turn it off. That's useful to know too: tell us about it, and
send the results file if there is one on the stick.

## 5. Send us the results

1. Plug the USB stick into your other computer. It shows up as a drive named
   **SGXKIT**.
2. On it, find the file that starts with **`results-`** and ends with **`.tar.gz`**.
3. Attach it to a comment on the GitHub issue where you were asked to test: drag the file
   into the comment box. Please also say which computer model you used.

The file contains only technical information about your computer's hardware and what the
driver reported. Serial numbers and network addresses are removed. No personal files are
ever read.

## What happens next

This first test only asks the graphics chip what exactly it is. The driver doesn't draw
anything yet. With your results we add support for your chip, and the next test kit
will draw on it.
