# fwupdate

Shell command that writes a firmware image to a bank of the internal flash and tells the platform which bank to start.

```
fwupdate status                                  # state of the banks
fwupdate write <image> [bank] [--select|--trial] # default bank: the one that does not run
fwupdate verify <bank>
fwupdate select <bank> [--trial]
```

The banks, their verification and the choice belong to the platform (`Dmod_Firmware*` of the dmod SAL, implemented by
dmboot with its selector); the bytes go through the dmflash node of the bank, `/dev/dmflashx/bank_a`, `bank_b`, ...
(a section of the dmflash `.ini` for each bank). The running bank is never written, and the header of the image is
written last, so an interrupted write leaves an empty bank, not a half image.
`--trial`: the platform goes back to the previous bank when the new firmware does not confirm (`Dmod_FirmwareReady`).

Build against a dmflash checkout until it is released: `-DDMFLASH_INCLUDE_DIR=<dmflash>/include`.

## Confirming a start

`fwupdate confirm` calls `Dmod_FirmwareReady()`. What "the firmware works" means is the image's to say: a oneshot unit
that runs it after what the image needs (`after=` / `requires=`), e.g. in `cfg/services/bootok.ini`:

```
description=Confirm the boot of the firmware
exec=fwupdate
args=confirm
type=oneshot
restart=no
after=ext
requires=ext
```

A firmware started on trial (`--trial`) that does not confirm in the number of starts the selector allows is dropped, and the
previous bank starts.
