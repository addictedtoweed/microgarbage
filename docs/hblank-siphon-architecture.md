# SNES H-Blank DMA Siphon Architecture Notes

## Goal

This document describes a proposed rendering pipeline for a dedicated
**60-color framebuffer mode** on the SNES.

The objective is to maximize VRAM bandwidth by using **small normal DMA
transfers during every H-Blank**, synchronized by the SNES H-counter
IRQ.

This mode is intended for the custom cartridge coprocessor ("Colonel")
and is separate from the normal sprite-based game renderer.

------------------------------------------------------------------------

# Overall Rendering Pipeline

At the beginning of a new framebuffer transfer:

1.  Enter framebuffer mode.
2.  Disable HDMA.
3.  Disable the sprite engine.
4.  Configure VRAM increment mode.
5.  Set the initial VRAM destination once.
6.  Configure one normal DMA channel.
7.  Set the initial DMA source once.
8.  Program the H-counter interrupt once.
9.  Enable H-IRQ.

Every visible scanline:

1.  H-counter IRQ occurs.
2.  Acknowledge the IRQ.
3.  Read the next DMA transfer length from a table.
4.  If the length is zero:
    -   Disable H-IRQ.
    -   End the siphon for this frame.
5.  Otherwise:
    -   Reload the DMA byte count.
    -   Optionally assert forced blank (experimental).
    -   Start DMA.
    -   Optionally restore rendering.
    -   Advance to the next table entry.
6.  RTI.

At the next NMI:

-   Begin transferring the next completed framebuffer.

------------------------------------------------------------------------

# Why Source and Destination Are Only Initialized Once

Normal DMA automatically advances the source address after each
transfer.

The VRAM address also advances automatically according to VMAIN
(\$2115).

Therefore the IRQ does **not** rewrite:

-   VRAM address
-   DMA source address
-   DMA mode
-   DMA destination register

Only the DMA transfer count is rewritten each scanline because the SNES
decrements it to zero after every DMA burst.

This keeps the IRQ extremely small and minimizes timing jitter.

------------------------------------------------------------------------

# Per-Scanline Transfer Table

Each scanline has one entry describing how many bytes should be
transferred.

Example:

    8
    8
    8
    8
    6
    6
    6
    6
    4
    4
    4
    4
    0

A value of zero means:

-   Disable H-IRQ.
-   Stop transferring for the remainder of the frame.

This allows:

-   Different transfer sizes per scanline.
-   Automatic early termination.
-   Easy experimentation on real hardware.

Transfer lengths should be **even** when using DMA mode 1
(\$2118/\$2119).

------------------------------------------------------------------------

# Dedicated Framebuffer Mode

This rendering mode intentionally simplifies the PPU configuration.

Enabled:

-   BG layers required by the framebuffer.

Disabled:

-   OBJ (sprites)
-   OAM DMA
-   HDMA

The Colonel treats this as a dedicated rendering mode rather than
attempting to coexist with the normal sprite renderer.

Advantages:

-   Reduced PPU activity.
-   Less contention during H-Blank.
-   Simpler timing.
-   Easier debugging.
-   Potentially fewer sources of graphical corruption.

When returning to the normal game renderer:

-   Restore sprite enable.
-   Restore HDMA if required.
-   Resume normal OAM updates.

------------------------------------------------------------------------

# H-IRQ Timing

The horizontal timer (HTIME) is programmed **once**.

The SNES automatically generates an H-counter IRQ at the programmed
horizontal position on every scanline.

The IRQ handler does not need to rewrite HTIME every line.

The interrupt position can be moved earlier or later experimentally
until the DMA burst consistently fits inside the safe H-Blank window.

------------------------------------------------------------------------

# Suggested IRQ Flow

    IRQ

    Read TIMEUP

    Read next transfer length

    If zero:
        Disable HIRQ
        RTI

    Reload DMA count

    Optional:
        Enter forced blank

    Write MDMAEN

    Optional:
        Restore display

    Advance table pointer

    RTI

------------------------------------------------------------------------

# Suggested Development Strategy

1.  Disable HDMA.
2.  Disable sprites.
3.  Use H-IRQ only.
4.  Initialize DMA source once.
5.  Initialize VRAM destination once.
6.  Begin with a constant 4-byte transfer.
7.  Increase transfer length gradually.
8.  Tune HTIME experimentally.
9.  Introduce variable transfer tables.
10. Experiment with per-line forced blank only after the basic mechanism
    is stable.

This minimizes variables during debugging and should make it easier to
determine the practical H-Blank bandwidth available on real hardware.
