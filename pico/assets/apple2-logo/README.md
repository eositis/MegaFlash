# MegaFlash logo — Apple IIc graphics modes

Rendered from `megaflash_logo_source.png` for display on an Apple IIc.

## Modes

| Mode | Resolution | Colors | Preview | Native PNG | Machine data |
|------|------------|--------|---------|------------|--------------|
| **Double Low-Res (DLGR)** | 80×48 | 16 | `megaflash_dlgr_preview.png` | `megaflash_dlgr_80x48.png` | `megaflash_dlgr_packed.bin` (1920 B, 2 pixels/byte), `megaflash_dlgr_80x48.idx` |
| **Hi-Res (HGR)** | 280×192 | 6 artifact | `megaflash_hgr_preview.png` | `megaflash_hgr_280x192.png` | `megaflash_hgr.bin` (8192 B screen dump @ `$2000`) |
| **Double Hi-Res (DHGR)** | 560×192 (140×192 color cells) | 16 | `megaflash_dhgr_preview.png` | `megaflash_dhgr_560x192.png` | `megaflash_dhgr_140x192.idx` |

Comparison sheet: `megaflash_apple2_modes_sheet.png`  
Palette card: `apple2_palette_reference.png`

## Palette notes

- **HGR:** Black, White, Green, Violet, Orange, Blue. Conversion picks Green/Violet vs Orange/Blue per 7-pixel HGR byte (high bit).
- **DHGR / DLGR:** Standard Apple IIe/IIc 16-color set (Magenta, Dark Blue, Purple, Dark Green, Greys, Browns, Orange, Pink, Light Green, Yellow, Aquamarine, White, …).

## Regenerate

```bash
python3 -m venv /tmp/a2logo && source /tmp/a2logo/bin/activate
pip install Pillow
python3 render_apple2_logo.py          # uses megaflash_logo_source.png
# or: python3 render_apple2_logo.py /path/to/other.png
```

## Loading on real hardware / emulator

- **HGR:** `BLOAD MEGAFLASH.HGR,A$2000` then soft-switch into HGR (approx. `megaflash_hgr.bin`).
- **DLGR / DHGR:** Index/packed files are palette maps for a display routine; full aux/main interleave encoding for DHGR memory is not included yet.
