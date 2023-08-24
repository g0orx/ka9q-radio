This package is designed for Debian Linux, including the Raspberry Pi
OS. Since I use a Macbook Pro as my desktop, some of it (e.g., the
interactive components 'control' and 'monitor') will also compile and
run on MacOS -- but not all of it. However, I'm interested in fixing
any unnecessary non-portabilities.

Prerequisites

Building this package on Debian requires the following packages be installed with 'apt install':

build-essential
make
gcc
libairspy-dev
libairspyhf-dev
libavahi-client-dev
libbsd-dev
libfftw3-dev
libhackrf-dev
libiniparser-dev
libncurses5-dev
libopus-dev
libpigpio-dev (Raspberry Pi only)
librtlsdr-dev
libusb-1.0-0-dev
libusb-dev
portaudio19-dev
libasound2-dev
uuid-dev

Although not needed to build ka9q-radio, I find these additional
packages useful for working with multicast audio:

sox
libsox-fmt-all
opus-tools
flac
avahi-utils
avahi-browse
molly-guard (if you run your system remotely)
tcpdump


Then:

$ ln -s Makefile.[linux|pi] Makefile
$ make
$ sudo make install

This will write into the following directories:

/usr/local/sbin	     	 	   daemon binaries (e.g., 'radiod')
/usr/local/bin		 	   application programs (e.g., 'control')
/usr/local/share/ka9q-radio	   support files (e.g., 'modes.conf')
/var/lib/ka9q-radio		   application state files (e.g., tune-*)
/etc/systemd/system  		   systemd unit files (e.g., radio@.service)
/etc/sysctl.d	    		   system configuration files (e.g., 98-sockbuf.conf)
/etc/udev/rules.d		   device daemon rule files (e.g., 52-airspy.rules)
/etc/fftw			   FFTW "wisdom" files (i.e., wisdomf)
/etc/radio			   program config files (e.g., radio@2m.conf)

It will also create several special system users and groups so that
the daemons don't have to run with root permissions. I recommend that
you add your own user ID to the "radio" group, which is applied to
most of the relevant installed directories and files.

Read the file FFTW3.md on pre-computing efficient transforms for the FFTs in 'radio'.