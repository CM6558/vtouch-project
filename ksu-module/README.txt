vtouchmerge KernelSU module

Files:
- system/bin/vtouchmerge: merged worker
- system/bin/vtouchsupervise: watchdog/supervisor
- service.sh: delayed startup
- post-fs-data.sh: runtime directory setup

The service uses /data/adb/vtouch-merge/merge.sock and does not use /data/local/tmp.
It dynamically discovers a Type-B touchscreen and only grabs it after creating the merged uinput device.

Important: this build has not yet been run on the phone. First boot should be treated as a staged test. If touch stops responding, disable this module from KernelSU recovery/manager and reboot.
