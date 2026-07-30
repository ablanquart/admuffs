# 3D-printed case — Raspberry Pi 4B + ANAVI Infrared pHAT

A two-part, screw-together case designed for exactly this project's hardware
stack, including the sensor-equipped pHAT variant. Files live in
`hardware/case/`:

| File | What |
|---|---|
| `admuffs_case.scad` | Parametric OpenSCAD source — every dimension is a named variable |
| `admuffs_case_base.stl` | Printable base tray (91.3 × 62.3 × 24.9 mm) |
| `admuffs_case_lid.stl` | Printable lid (91.3 × 62.3 × 7.4 mm incl. lip) |

## Design decisions (why it looks like this)

- **Open skylight over the pHAT.** This is deliberate, not laziness: the IR
  LED and receiver need unobstructed line-of-sight to your room, and the
  plug-in I²C sensors (HTU21D temperature/humidity, BMP180 pressure, BH1750
  light) must read *ambient* air and light — sealed inside a box, a
  temperature sensor mostly measures the Pi's own heat. The aperture spans
  the pHAT's entire component area; sensor modules stand up through it into
  free air, and the IR LED can be gently bent toward the TV. The covered
  strip at the rear sits over the 40-pin header, where nothing protrudes.
- **Drop-in port notches.** All connector openings run to the top of the
  base wall, so the Pi lowers straight in (its USB/Ethernet blocks overhang
  the PCB and can't pass closed holes). The lid closes the notches from
  above. Bonus: the base prints with **zero bridges and zero supports**.
- **Generous tolerances.** Cutouts follow the official Pi 4B mechanical
  drawing with ≥1.5 mm margin per side, so printer calibration and cable
  plug bodies aren't a fight. USB-C, 2× micro-HDMI, AV jack, Gigabit
  Ethernet, both USB stacks, and the under-board microSD slot are all
  reachable with the case closed.
- **Convection path.** Floor vent slots under the Pi + vent slots in the lid
  over the SoC/USB region + the skylight = a passive chimney. The Pi 4 runs
  this workload (audio DSP + an occasional IR blast) far below throttling.

## Printing

- **Material:** PETG or PLA. PETG preferred if the case sits in a warm media
  cabinet.
- **Layer height:** 0.2 mm. **Perimeters:** 3. **Infill:** 20 %.
- **Supports:** none needed, either part. **Brim:** optional for the base.
- **Orientation:** both parts flat, as exported.
- If the lid lip is too tight/loose in your printer's dialect, adjust the
  `fit` parameter in the `.scad` (default 0.25 mm) and re-export —
  `openscad -o lid.stl -D 'part="lid"' admuffs_case.scad`.

## Assembly

1. Drop the Pi 4 into the base, connectors into their notches.
2. Fasten with **4 × M2.5 self-tapping screws, 5–6 mm** into the posts
   (pilot holes are 2.3 mm). Nylon M2.5 standoff kits work too.
3. Mount the ANAVI Infrared pHAT on the GPIO header as usual (its holes line
   up above the Pi's; ANAVI's standoffs fit within the case height).
4. Plug your sensor modules into the pHAT slots — they stand through the
   lid's aperture.
5. Seat the lid: the inner lip registers against the microSD-side and
   GPIO-side walls; the plate rests on the wall tops and closes the port
   notches.
6. Point the IR LED at the TV. If your print sits below TV level, bend the
   LED's legs gently upward/outward through the aperture — line-of-sight
   matters more than looks.

## Test-fit note

Print the **base first** and dry-fit your Pi before printing the lid. All
port positions carry wide margins, but board revisions and printer shrinkage
vary — a one-part test costs 40 minutes; discovering it after both parts
costs an evening. If any cutout needs a nudge, every connector center and
width is a named constant at the top of the `.scad`.

## Customizing

`admuffs_case.scad` is fully parametric: wall thickness, post height, fit
clearance, vent layout, emboss text, and all connector geometry are
variables. Set `part="preview"` in OpenSCAD to see the assembled case with a
ghosted Pi + pHAT for sanity-checking changes.
