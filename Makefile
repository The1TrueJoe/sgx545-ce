# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for iteration. openHC builds this in-tree as obj-y via the
# Buildroot package in buildroot/ -- see README.md.
#
#   make                         build against $(KERNELDIR)
#   make KERNELDIR=/path/to/src  build against a specific kernel tree
#   make shell                   drop into the dev container
#   make clean

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

.PHONY: all clean image shell errors

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	find . -name '*.o' -o -name '*.ko' -o -name '.*.cmd' | xargs -r rm -f

# Compile and summarise: which files failed, and the distinct error kinds.
# The whole port is a loop over this target.
errors:
	@$(MAKE) -C $(KERNELDIR) M=$(PWD) modules 2>&1 | tee /tmp/build.log | tail -5 || true
	@echo "--- files with errors:"
	@grep -oE '^[^ :]+\.c(:[0-9]+)?' /tmp/build.log | cut -d: -f1 | sort -u || true
	@echo "--- distinct errors:"
	@grep -oE 'error: .*' /tmp/build.log | sed 's/[0-9]\+/N/g' | sort | uniq -c | sort -rn || true

image:
	DOCKER_CONFIG=$(DOCKER_CONFIG) docker build -f dev/Dockerfile -t sgx545-dev .

shell:
	docker run --rm -it -v "$(PWD):/src" sgx545-dev sh
