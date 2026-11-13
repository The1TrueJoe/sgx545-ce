SGX545 GPU test stick for Intel Cedarview (GMA 3600/3650), live Debian 13

1. Start the Cedarview computer from this stick (pick the USB stick in the boot menu).
2. Walk away. The test runs by itself for up to 15 minutes and never writes to the
   computer's own disks. The screen may change or go dark along the way: that's fine,
   nothing needs to be seen or pressed.
3. When it is done, the screen says "Done. Thank you!". Then the computer either
   turns itself off or opens a desktop that worked, for you to try. Either way the
   results are saved, and you can switch it off whenever you like.
4. Attach the one file named results-kitv5-....tar.gz on this drive to the GitHub
   issue, as the guide says:
   https://github.com/The1TrueJoe/powervr_sgx/blob/dev/docs/TESTING-CEDARVIEW.md

If after 20 minutes there is neither "Done. Thank you!" nor a desktop, or the
computer restarted into its own system, start it from the stick once more: the
test carries on where it stopped, in the same results file.
