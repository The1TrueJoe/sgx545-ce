# Defines and include path of the kernel driver build, for libpvr/abi/*.sh. Needs $R.
DEFS="-DLINUX -DSGX545 -DSUPPORT_SGX545 -DSGX_CORE_REV=1013 -DSERVICES4 -DSUPPORT_SRVINIT -DSUPPORT_SGX
 -DTRANSFER_QUEUE -DSYS_USING_INTERRUPTS -DSUPPORT_HW_RECOVERY -DPVR_SECURE_HANDLES -DUSE_PTHREADS
 -DSUPPORT_SGX_EVENT_OBJECT -DSUPPORT_SGX_HWPERF -DSUPPORT_LINUX_X86_PAT -DSUPPORT_CACHE_LINE_FLUSH
 -DSUPPORT_CPU_CACHED_BUFFERS -DDISABLE_PM -DSUPPORT_SGX_NEW_STATUS_VALS -DSUPPORT_PERCONTEXT_PB -DRELEASE
 -DSUPPORT_DRI_DRM -DSUPPORT_LINUX_X86_WRITECOMBINE"
INC="-I$R/kernel/src/include4 -I$R/kernel/src/services4/include -I$R/kernel/src/services4/include/env/linux
 -I$R/kernel/src/services4/srvkm/hwdefs -I$R/kernel/src/services4/srvkm/devices/sgx -I$R/kernel/src/services4/srvkm/include"
for d in "$R"/kernel/src/services4/system/*/; do INC="$INC -I$d"; done
