step 1 unmount the usb drive ( changhe sdx whit ur own usb drive )

sudo umount /dev/sdX*

step 2 flash it ( again chage sdx whit your own usb)

sudo dd if=VeRsitileOS.iso of=/dev/sdX bs=4M status=progress oflag=sync


( do lsblk to see what usb label you got for your usb)
( for example sdb) anyway hardware run resulted in a .. well not a failure nor a win. it booted on a old toshiba satelite sh. btu on my asus fx503vd keyboard dosent work. so. yh.