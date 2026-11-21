- Power cap: uncapped now means full speed. Releasing the cap writes 100% maximum processor state and boost on into the active power plan and leaves them there, instead of restoring the plan's previous values.
- Power cap: switching power plans while capped leaves the old plan at full speed and caps the new one. Before, releasing the cap could write one plan's values into another.
- Power cap: the label shows the maximum processor state actually applied.
- CPU power and clock readings no longer inflate under heavy load. Each counter is now timed at the moment it is read, not at the start of the sampling pass.

Requires Windows 11 24H2 or later with sudo for Windows enabled in Inline mode (`sudo config --enable normal`). The installer checks both.
