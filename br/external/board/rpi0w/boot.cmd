# Slot selection (RAUC U-Boot scheme): BOOT_ORDER lists the slots to try,
# BOOT_A_LEFT/BOOT_B_LEFT the remaining attempts.  Each boot uses one
# attempt; the booted system resets them once it is up (mark good).

test -n "${BOOT_ORDER}" || setenv BOOT_ORDER "A B"
test -n "${BOOT_A_LEFT}" || setenv BOOT_A_LEFT 3
test -n "${BOOT_B_LEFT}" || setenv BOOT_B_LEFT 3

setenv bootpart
setenv raucslot
for BOOT_SLOT in ${BOOT_ORDER}; do
  if test -z "${bootpart}"; then
    if test "${BOOT_SLOT}" = "A" && test ${BOOT_A_LEFT} -gt 0; then
      setexpr BOOT_A_LEFT ${BOOT_A_LEFT} - 1
      setenv bootpart 2
      setenv raucslot A
    elif test "${BOOT_SLOT}" = "B" && test ${BOOT_B_LEFT} -gt 0; then
      setexpr BOOT_B_LEFT ${BOOT_B_LEFT} - 1
      setenv bootpart 3
      setenv raucslot B
    fi
  fi
done

if test -z "${bootpart}"; then
  echo "No attempts left on any slot, resetting them"
  setenv BOOT_A_LEFT 3
  setenv BOOT_B_LEFT 3
  saveenv
  reset
fi

echo "Booting slot ${raucslot} (attempts left: A ${BOOT_A_LEFT}, B ${BOOT_B_LEFT})"
saveenv

setenv bootargs "root=/dev/mmcblk0p${bootpart} rootfstype=squashfs ro rootwait console=ttyGS0 panic=10 watchdog.open_timeout=60 rauc.slot=${raucslot}"
load mmc 0:${bootpart} ${kernel_addr_r} /boot/zImage && bootz ${kernel_addr_r} - ${fdt_addr}

# kernel missing or not bootable: try again (next attempt or slot)
reset
