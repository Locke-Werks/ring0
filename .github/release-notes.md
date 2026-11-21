- Power cap on the cpu view: a slider from 40 W to uncapped that holds CPU package power near the target by stepping the power plan's boost mode and maximum processor state. The original plan is restored on exit, or at the next launch after a crash. Needs the PawnIO driver for the power reading.
- Core evacuation on the cpu view (added in 0.4.0): select cores with click, Ctrl+click, Shift+click or a drag box, then right-click to steer every process off them. Restored on exit.

Requires Windows 11 24H2 or later with sudo for Windows enabled in Inline mode (`sudo config --enable normal`). The installer checks both.
