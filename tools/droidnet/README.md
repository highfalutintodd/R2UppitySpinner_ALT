# DroidNet Command Library entry for v4.0

`r2uppityspinner-alt.json` is the periscope's board file for Travis Cook's DroidNet Command
Library (https://github.com/travisccook/Droidnet-Command-Library, `libraries/boards/`), updated
for firmware 4.0.0-beta.1. It starts from the community v3.5.3 entry upstream (commit `17a89ec`,
2026-10-02, already in the current schema: `category`, not `group`) and changes only this:

- firmware `v4.0.0-beta.1`, `generatedFrom` and a routing note describing v4 (the same `:P`/`#P`
  grammar on the bus, USB and the optional mesh; the latching E-STOP and how it is released;
  `&PERI` telemetry; what the mesh and the bus refuse), which also says the entry drives v3.5.3+
  boards;
- six v4 commands added and marked "(v4)": `#PRESUME` (release the E-STOP), `#PSTATE`,
  `#PCLEARFAULT`, `#PROTARYTEST<turns>`, `#PRANDOMLIGHT<mode>` (with a new `uppity.randomLight`
  enum) and `#PMESHHB<seconds>`;
- clearer names for `:PX` (on v4 it latches) and `:PH` (lowers safely, homing the head first);
- `uppity.cfg.rc` (`#PRC`) **kept and marked v3 only** (v4 has no RC input and ignores it), so no
  command id is removed: the catalog bump is minor (4.6.0 to 4.7.0), not major, and v3 builders
  keep their button.

Deliberately left out: anything that unlocks the board, overrides the safety rules or changes a
measured clearance (`#PUNLOCK`, `#PCOMMISSION…`, `#POVERRIDE`, `#PSETHOME`, `#PSAFESPIN`,
`#PTRAVEL`, the jog and nudge commands). Those belong at the droid, on the USB console or the
board's own web page.

Checked with the library's own checks in a scratch copy of the upstream repo, with the catalog
bumped to 4.7.0 (`libraries/manifest.json`, `releases.json`, the version pins in `package.json`,
`package-lock.json` and two tests): `npm run validate` valid, `npm test` 498 passed. The two
warnings (`Lifter` and `Rotary` are not standard category names) were already there in the v3.5.3
entry.

Sending it upstream is a pull request to Travis's repository from Todd's fork: a public action
that Todd approves separately.
