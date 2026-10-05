# DroidNet Command Library entry for v4.0

`r2uppityspinner-alt.json` is the periscope's board file for Travis Cook's DroidNet Command
Library (https://github.com/travisccook/Droidnet-Command-Library, `libraries/boards/`), updated
for firmware 4.0.0. It starts from the community v3.5.3 entry upstream (commit `17a89ec`,
2026-10-02, already in the current schema: `category`, not `group`) and changes only this:

- firmware `v4.0.0`, `generatedFrom` and a routing note describing v4 (the same `:P`/`#P` grammar
  on the bus, USB and the optional mesh; the latching E-STOP and how it is released; `&PERI`
  telemetry; what the mesh and the bus refuse);
- `uppity.cfg.rc` (`#PRC`) removed: v4 dropped the RC/PPM input;
- clearer names for `:PX` (latches) and `:PH` (lowers safely, homing the head first);
- six v4 commands added: `#PRESUME` (release the E-STOP), `#PSTATE`, `#PCLEARFAULT`,
  `#PROTARYTEST<turns>`, `#PRANDOMLIGHT<mode>` (with a new `uppity.randomLight` enum) and
  `#PMESHHB<seconds>`.

Deliberately left out: anything that unlocks the board, overrides the safety rules or changes a
measured clearance (`#PUNLOCK`, `#PCOMMISSION…`, `#POVERRIDE`, `#PSETHOME`, `#PSAFESPIN`,
`#PTRAVEL`, the jog and nudge commands). Those belong at the droid, on the USB console or the
board's own web page.

Checked with the library's own validator (`node scripts/validate.js` after `npm install ajv@8`,
in a scratch copy of the upstream repo): valid. Its two warnings (`Lifter` and `Rotary` are not
standard category names) were already there in the v3.5.3 entry.

Sending it upstream is a pull request to Travis's repository: a public action that Todd approves
separately. Nothing has been sent.
