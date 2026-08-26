# Matter attestation: CD, PAI/DAC and factory partition

Test (development) attestation material for the Matterize Labs "WS2812 Animation
Light" (VID `0x1618`, PID `0x0001`). Everything here uses the Matter **test**
credentials shipped with the connectedhomeip checkout - do not use for
production.

Generated binaries and private keys are **not** committed: they land in
`tools/certs/out/` (see `.gitignore`). The only committed artifact in this
directory is this README.

## Layout

| Path | Purpose |
|------|---------|
| `firmware/main/certification_declaration/certification_declaration.der` | Test Certification Declaration (CD), signed with `Chip-Test-CD-Signing-Key.pem`, embedded into the firmware |
| `tools/certs/out/<vid>_<pid>/<uuid>/<uuid>-partition.bin` | Factory (NVS `fctry`) partition containing DAC + PAI + CD + discriminator/passcode |
| `tools/certs/out/<vid>_<pid>/<uuid>/<uuid>-onb_codes.csv` | Onboarding codes (QR / manual code / discriminator / passcode) |

## 1. Build `chip-cert` (one time)

The Matter host build is already configured by `esp-matter` bootstrap. Build the
tool with ninja instead of a fresh `gn gen`:

```bash
CHIP=~/.espressif/versions/esp-matter/release-v1.6/connectedhomeip/connectedhomeip
cd "$CHIP"
source ./scripts/activate.sh
ninja -C .environment/gn_out chip-cert
```

`chip-cert` is produced at `$CHIP/.environment/gn_out/chip-cert`.

> Note (Fedora 44 host): the configured GCC host toolchain hits a glibc
> 2.41+ `memchr` const-correctness warning in boringssl `bcm.c`. Worked around
> by adding `treat_warnings_as_errors = false` to
> `.environment/gn_out/args.gn` and re-running `gn gen .environment/gn_out`.
> The Linux DBus wrappers also need `glib2-devel` (`sudo dnf install glib2-devel`).

## 2. Generate the test CD

```bash
CHIP=~/.espressif/versions/esp-matter/release-v1.6/connectedhomeip/connectedhomeip
"$CHIP/.environment/gn_out/chip-cert" gen-cd \
  -f 1 -V 0x1618 -p 0x0001 -d 0x0016 \
  -c "CSA00000SWC00000-01" -l 0 -i 0 -n 1 -t 0 \
  -K "$CHIP/credentials/test/certification-declaration/Chip-Test-CD-Signing-Key.pem" \
  -C "$CHIP/credentials/test/certification-declaration/Chip-Test-CD-Signing-Cert.pem" \
  -O firmware/main/certification_declaration/certification_declaration.der
```

## 3. Generate the factory partition (DAC + PAI + CD + discriminator/passcode)

```bash
pip install esp-matter-mfg-tool   # or: uv tool install esp-matter-mfg-tool
CHIP=~/.espressif/versions/esp-matter/release-v1.6/connectedhomeip/connectedhomeip

esp-matter-mfg-tool \
  --passcode 89674523 --discriminator 2245 \
  -cd firmware/main/certification_declaration/certification_declaration.der \
  -v 0x1618 --vendor-name "Matterize Labs" \
  -p 0x0001 --product-name "WS2812 Animation Light" \
  --hw-ver 1 --hw-ver-str v1.0 \
  --paa \
  -k "$CHIP/credentials/test/attestation/Chip-Test-PAA-NoVID-Key.pem" \
  -c "$CHIP/credentials/test/attestation/Chip-Test-PAA-NoVID-Cert.pem" \
  --outdir tools/certs/out
```

The PAI is generated from the test PAA (NoVID), and the DAC from that PAI. DAC,
PAI, CD and commissionable data are written into the `fctry` NVS partition.
Output (one `<uuid>` directory per device) is under
`tools/certs/out/<vid>_<pid>/`.

## 4. Flash the factory partition

```bash
cd firmware
idf.py -p /dev/ttyUSB0 flash
parttool.py -p /dev/ttyUSB0 write_partition --partition-name fctry \
  ../tools/certs/out/1618_1/<uuid>/<uuid>-partition.bin
```

## 5. Verify VID/PID (chip-tool)

Commission with chip-tool and read `Basic Information` - it must report
VID `0x1618` and PID `0x0001`.
