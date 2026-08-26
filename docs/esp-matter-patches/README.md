# esp-matter v1.6 patches

Local fixes applied to `~/.espressif/versions/esp-matter/release-v1.6` that the project depends on.
These live **outside** this repo, so they are recorded here and must be re-applied if esp-matter is re-cloned/updated.

## 0001-fix-long-octet-string-ember-size.patch

**File:** `components/esp_matter/data_model_provider/private/esp_matter_attr_val_ember_buffer.cpp`

**Problem:** `get_ember_attr_size_from_val()` sizes long octet/char strings as `s + 1`, but the ember buffer layout for long strings uses a 2-byte length header (`val.a.t = s + 2`). Every write to a **long octet string** attribute therefore allocated a buffer 1 byte too small, and `build_ember_buffer_from_attr_val()` returned `Status::ResourceExhausted` (0x89). Regular octet strings (`s + 1`, 1-byte header) are unaffected.

This blocked `ATTR_FRAME_CHUNK` (>255-byte animation frame chunks) in the custom cluster `0x1618FC01`.

**Fix:** size long strings as `s + 2`.

**Apply:**
```bash
cd ~/.espressif/versions/esp-matter/release-v1.6
git apply /path/to/0001-fix-long-octet-string-ember-size.patch
```
(Or make the one-line edit manually: `+ 1` → `+ 2` in the `LONG_OCTET_STRING` / `LONG_CHAR_STRING` case.)
