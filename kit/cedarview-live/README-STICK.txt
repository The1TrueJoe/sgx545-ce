SGX545 GPU test stick for Intel Cedarview (GMA 3600/3650), live Debian 13

1. Start the Cedarview computer from this stick (pick the USB stick in the boot menu).
2. Walk away. The test runs by itself for up to 15 minutes and never writes to the
   computer's own disks. The screen may change or go dark along the way: that's fine,
   nothing needs to be seen or pressed.
3. The computer turns itself off when the test is done.
4. Attach the one file named results-kitv4-....tar.gz on this drive to the GitHub
   issue, as the guide says:
   https://github.com/The1TrueJoe/sgx545-ce/blob/main/docs/TESTING-CEDARVIEW.md

If the computer stops for more than 20 minutes, or restarts into its own system,
start it from the stick once more: the test carries on where it stopped, in the
same results file.
