SGX545 GPU test stick for Intel Cedarview (GMA 3600/3650), live Debian 13

Boot a Cedarview computer from this stick and pick menu entry 1. The test runs by
itself for about ten minutes, shows a desktop for part of it, and never writes to
the computer's own disks.

When it finishes, a file named results-kitv3.2-....tar.gz appears on this drive.
Attach that file to the GitHub issue, as the guide says:
https://github.com/The1TrueJoe/sgx545-ce/blob/main/docs/TESTING-CEDARVIEW.md

If the screen goes dark (some Cedarview panels do with Linux):
  1. Wait about 5 minutes. The test keeps running and saves its results.
  2. Turn the computer off (hold the power button).
  3. Boot from the stick again and pick the next menu entry (2, then 3, then 4).
Each try saves its own results file. Attach all of them, and say in your comment
which entries left the screen dark, and whether it was fully dark (no backlight)
or lit but black.
