SGX545 GPU test stick for Intel Cedarview (GMA 3600/3650), live Debian 13

Boot a Cedarview computer from this stick and pick a menu entry. The test runs by
itself for about ten minutes, shows a desktop for part of it, and never writes to
the computer's own disks.

Please run it six times: entry 1 three times, then entry 2 three times. Some
panels go dark only some of the time, so we need counts, not one try. Then tell
us how many of the three were dark with each entry (for example "entry 1: 0 of 3
dark, entry 2: 2 of 3 dark").

Each run puts its own file named results-kitv3.4-....tar.gz on this drive (the
-e1-, -e2- in the name is the entry). Attach all of them to the GitHub issue, as
the guide says:
https://github.com/The1TrueJoe/sgx545-ce/blob/main/docs/TESTING-CEDARVIEW.md

If the screen goes dark during a run:
  1. Wait about 5 minutes. The test keeps running and saves its results.
     If it is the desktop that stays dark, wait at least 2 minutes there:
     the test saves its results to the stick every 15 seconds.
  2. Turn the computer off (hold the power button), then start the next run.
Say whether it was fully dark (no backlight) or lit but black. If entries 1 and 2
both go dark every time, try 3 and 4 once each as well.
