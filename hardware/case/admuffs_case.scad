// SPDX-License-Identifier: MIT
// ============================================================================
// Admuffs case - Raspberry Pi 4B + ANAVI Infrared pHAT (with sensor modules)
// ============================================================================
// Two printed parts:
//   part = "base"  - tray: Pi drops in from the top, ports exit through
//                    open-top notches (no support material, easy insertion)
//   part = "lid"   - plate with inner lip; LARGE open aperture above the
//                    pHAT so (a) the IR LED and receiver keep line-of-sight,
//                    (b) plug-in I2C sensors (HTU21D / BMP180 / BH1750) stand
//                    proud of the case and read AMBIENT air, not box air.
//
// All connector positions follow the official Raspberry Pi 4B mechanical
// drawing; every cutout carries >=1.5 mm margin per side, so small printer
// or drawing tolerances don't matter. Print a base first and test-fit
// before printing the lid; adjust `fit` if your printer runs tight/loose.
//
// Coordinates: origin at the Pi PCB corner where the microSD (x=0) edge
// meets the USB-C/HDMI (y=0) edge. GPIO runs along y=56; USB/Ethernet at
// x=85. The pHAT overhangs x in [0..65], y in [26..56], above the Pi.
// ============================================================================

part = "preview";   // "base" | "lid" | "preview" (assembled view)

// ---- tunables --------------------------------------------------------------
fit        = 0.25;  // printer fit clearance for the lid lip (raise if tight)
wall       = 2.4;   // wall thickness
floor_t    = 2.4;   // floor thickness
clr        = 0.75;  // gap between PCB edge and inner wall, each side
post_h     = 4.0;   // standoff under the Pi (leaves room for the microSD)
wall_top   = 22.5;  // inner wall height above cavity floor (clears USB stack)
lid_t      = 2.4;   // lid plate thickness
lip_h      = 5.0;   // lid alignment lip depth
emboss     = true;  // "ADMUFFS" text on the lid

// ---- Raspberry Pi 4B geometry (official mechanical drawing) ---------------
pcb_l = 85;  pcb_w = 56;  pcb_t = 1.4;
hole_x = [3.5, 61.5];        // mounting holes: 58 x 49 mm grid, 3.5 in
hole_y = [3.5, 52.5];
pcb_top = post_h + pcb_t;    // z of PCB top above cavity floor

// y=0 edge connector centers (x) and generous cutout widths
usbc_x   = 11.2;  usbc_w  = 13;
hdmi0_x  = 26.0;  hdmi_w  = 12;
hdmi1_x  = 39.5;
av_x     = 54.0;  av_w    = 10;
// x=85 edge connector centers (y): Ethernet nearest GPIO edge on the Pi 4.
eth_y    = 45.75; eth_w   = 19;
usb3_y   = 27.0;  usb_w   = 17;
usb2_y   = 9.0;
// microSD (x=0 edge): card sits under the PCB
sd_w     = 16;

// ---- derived ---------------------------------------------------------------
in_l  = pcb_l + 2*clr;                 // cavity 86.5 x 57.5
in_w  = pcb_w + 2*clr;
out_l = in_l + 2*wall;
out_w = in_w + 2*wall;
base_h = floor_t + wall_top;
corner_r = 3.5;

$fn = 48;

// rounded-rectangle prism
module rbox(l, w, h, r) {
    hull() for (x = [r, l-r], y = [r, w-r])
        translate([x, y, 0]) cylinder(h = h, r = r);
}

// place children in PCB coordinates (cavity floor = z0)
module pcbspace() { translate([wall + clr, wall + clr, floor_t]) children(); }

// ---- base ------------------------------------------------------------------
module base() {
    difference() {
        rbox(out_l, out_w, base_h, corner_r);
        // cavity
        translate([wall, wall, floor_t]) rbox(in_l, in_w, base_h, 2);

        // ---- port notches: open to the wall top so the Pi drops straight in
        pcbspace() {
            // y=0 edge: USB-C, 2x micro HDMI, AV jack
            for (c = [[usbc_x, usbc_w], [hdmi0_x, hdmi_w],
                      [hdmi1_x, hdmi_w], [av_x, av_w]])
                translate([c[0] - c[1]/2, -clr - wall - 1, pcb_top - 1])
                    cube([c[1], wall + 2, wall_top]);
            // x=85 edge: Ethernet + two USB stacks
            for (c = [[eth_y, eth_w], [usb3_y, usb_w], [usb2_y, usb_w]])
                translate([pcb_l - 0.5, c[0] - c[1]/2, pcb_top - 1])
                    cube([clr + wall + 2, c[1], wall_top]);
            // x=0 edge: microSD slot (card lives under the PCB)
            translate([-clr - wall - 1, pcb_w/2 - sd_w/2, 0.2])
                cube([wall + 2, sd_w, post_h + 1.2]);
        }

        // floor vents (also convection intake for the sensors above)
        for (i = [0:5])
            translate([out_l/2 - 33 + i*11, out_w/2 - 16, -1])
                rbox(5, 32, floor_t + 2, 2.4);
    }

    // mounting posts, M2.5 self-tapping pilot holes
    pcbspace() for (x = hole_x, y = hole_y)
        translate([x, y, 0]) difference() {
            cylinder(d = 6.5, h = post_h);
            translate([0, 0, -floor_t + 1.2]) cylinder(d = 2.3, h = post_h + floor_t);
        }
}

// ---- lid -------------------------------------------------------------------
module lid() {
    difference() {
        union() {
            rbox(out_l, out_w, lid_t, corner_r);
            // alignment lip inside the two port-free walls (SD edge + GPIO
            // edge), so the lid can't cover any connector notch.
            translate([wall + fit, wall + fit, -lip_h]) {
                cube([3, in_w - 2*fit, lip_h + 1]);                        // x=0 wall
                translate([0, in_w - 2*fit - 3, 0])
                    cube([in_l - 2*fit, 3, lip_h + 1]);                    // GPIO wall
            }
        }

        // ---- the big idea: open sky above the pHAT ----
        // IR LED + receiver keep line-of-sight to the room, and the plug-in
        // I2C sensors stand through the opening into ambient air. Covers the
        // pHAT's component area (its rear strip is the 40-pin header, which
        // nothing pokes through), bounded so the lid keeps a stiff rim.
        translate([wall + clr - 2, wall + clr + 23, -lip_h - 1])
            rbox(70, 29.5, lid_t + lip_h + 2, 3);

        // vent slots over the remaining covered areas
        for (i = [0:3])                                    // over USB/Ethernet
            translate([out_l - 16, 6 + i*7, -1]) rbox(9, 4, lid_t + 2, 1.8);
        for (i = [0:4])                                    // over the Pi front
            translate([8 + i*12, 6, -1]) rbox(8, 4, lid_t + 2, 1.8);

        // embossed name
        if (emboss)
            translate([out_l/2 - 14, 14, lid_t - 0.8])
                linear_extrude(1) text("ADMUFFS", size = 5.5,
                                       font = "DejaVu Sans:style=Bold");
    }
}

// ---- output ----------------------------------------------------------------
if (part == "base") base();
if (part == "lid")  lid();
if (part == "preview") {
    base();
    color("SteelBlue", 0.85) translate([0, 0, base_h + lip_h]) lid();
    // ghost of the Pi + pHAT for sanity checking
    %pcbspace() {
        translate([0, 0, post_h]) cube([pcb_l, pcb_w, pcb_t]);          // Pi
        translate([0, 26, post_h + 12.6]) cube([65, 30, 1.6]);          // pHAT
    }
}
