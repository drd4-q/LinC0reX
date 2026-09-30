### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers

### AnyKernel setup
# global properties
properties() { '
kernel.string=Darkmoon-Reborn
do.devicecheck=1
do.cleanup=1
device.name1=stone
device.name2=moonstone
device.name3=sunstone
'; } # end properties

### AnyKernel install
# boot shell variables
block=boot;
is_slot_device=auto;
no_block_display=1;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

# boot install
#
# Kernel only.  The stock boot.img carries no appended DTB, and ak3-core.sh
# appends any dtb/dtbo found in the zip root, which changes how the panel
# initialises.  Keep the layout stock-shaped.
split_boot;
flash_boot;
## end boot install
