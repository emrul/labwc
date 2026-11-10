# Portal patch ledger: labwc

This repository is Portal Desktop's fork of [labwc/labwc](https://github.com/labwc/labwc). Portal
carries the patches listed below, which let a remote-desktop host running inside a container
session observe and drive the compositor: cursor shapes, window geometry, damage, content type
and virtual touch. Everything else tracks upstream verbatim. Upstream maintainers are welcome to
incorporate any of them, in whole or in part, at their discretion. The fork is meant to shrink:
patches are expected to graduate upstream, not accumulate.

This follows the patch-ledger model of Aster's forks (salvo, iroh, fory), with the names changed
for Portal.

## Branch and tag model

- **`master`** is upstream `master` plus one commit: the README preamble and this ledger.
  - Nothing is built from `master`.
  - It is refreshed with `git merge upstream/master` and is never reset or force-pushed.
  - Ledger edits are commits on `master`, never on `upgrade/*`.
- **`upgrade/labwc-v<upstream version>`** is the patched integration line: the upstream release
  plus the patches in this ledger.
  - A new upstream base always gets a new branch, cut from the upstream release tag.
  - Portal-only fixes and re-pins append to the current branch.
  - Consumers pin commits on it.
  - Published `upgrade/*` branches and tags are never rewritten. A failed attempt is superseded
    by new commits and a new version.
- **Topic branches** carry one change each and are named for the change.
  - `feat/` is for new modules and protocols, `hook/` for splices into upstream code paths, and
    `fix/` for fixes.
  - A topic is based on the upstream release tag. A patch that builds on others (every hook, and
    P19) is based instead on the assembled integration line just below it.
  - Each is cherry-picked with `git cherry-pick -x` onto the upgrade branch, so every
    upgrade-branch commit names its source.
  - Check a topic with `git cherry -v <its base> <topic>`, which should list exactly one `+`.
  - A topic that must be re-cut for a new upstream base gets a new name,
    `<topic>-v<upstream version>`. The old branch stays as it was.
- **Version:** `major.minor.P`, with `P = 1000 * (upstream_patch + 1) + n`, where `n` (1 to
  999) counts Portal's releases on that upstream version. Upstream 0.20.3 plus the first Portal
  release is `0.20.4001`. A re-pin without a code change keeps the version and takes the tag
  suffix `-p<n>`.
- **Tags:** `portal-labwc-v<version>`, created only after the Portal consumer trial passes against
  that exact commit, because tags cannot be moved. The trial is the container image build and its
  live checks.
- **Remotes:** `origin` is `https://github.com/emrul/labwc`; `upstream` is
  `https://github.com/labwc/labwc`, read-only.
- **Earlier branches:** `portal` (on 0.9.7) and `portal-0.20` (on 0.20.3, from portal-agent) are
  historical and kept unchanged. Their `PORTAL_PATCHES.md` described the same patches; this
  ledger replaces it.
- Portal does not submit patches upstream.

## Current release

| Field | Value |
|---|---|
| Upstream base | `0.20.3` |
| Integration branch | `upgrade/labwc-v0.20.3` |
| Pinned commit | `78f21457352a23c5165d235611fe46bfeab8f3bb` (the packaging commit) |
| Version | `0.20.4001` |
| Tag | `portal-labwc-v0.20.4001` (annotated) → `78f2145735`. Consumer trial passed on 10 November 2026: the Ubuntu 26.04 session image, with German keys, cursor shapes and native touch |
| Consumer | `container/flake.nix` in Portal Desktop (`labwc-portal`, built against wlroots 0.20 from the catalog's nixpkgs pin) |

The integration line is upstream `0.20.3`, then LABWC-P01..P19 in ID order, each a `cherry-pick -x`
of its topic. Last comes the packaging commit `78f2145735`, which sets labwc's meson version to
`0.20.4001` and is kept separate from the patches, as fory does. nixpkgs' version check and
`labwc --version` then name the Portal build.

## Patches

| ID | Topic branch | Base | Topic commit | Upgrade commit | What and why |
|---|---|---|---|---|---|
| LABWC-P01 | `feat/portal-content-type` | `0.20.3` | `9e186ad23e` | `d400c1416f` | `wp_content_type_v1` (staging), so clients can hint a surface's content type; drives the host's per-surface encoder choice. |
| LABWC-P02 | `feat/portal-cursor-shape` | `0.20.3` | `1d2200b89a` | `908c6b4f0e` | Private `portal_cursor_shape_v1`: forwards the current cursor shape (client `wp_cursor_shape_v1` requests and labwc's own) so the viewer paints the cursor. |
| LABWC-P03 | `feat/portal-csd-margin` | `0.20.3` | `4e5370bc04` | `92c668e85f` | `portal_any_view_window_contains_cursor()`, used by the CSD resize guard (LABWC-P17). |
| LABWC-P04 | `feat/portal-toplevel-positioning` | `0.20.3` | `002b391b5d` | `9173f614ec` | Private `portal_toplevel_positioning_v1`: per-toplevel, popup and menu geometry, stacking and visibility. |
| LABWC-P05 | `feat/csd-shadow-clip` | `0.20.3` | `1b68ce4fac` | `ff5ee7bb9d` | Clips client-side-decoration shadows while keeping their resize hit zones (`desktop.c`, `xdg.c`). Upstream quality. |
| LABWC-P06 | `hook/portal-managers-on-server` | the line below it | `0e1e9a02ec` | `bc2be76319` | Hook: the portal managers on `struct server` (`include/labwc.h`). |
| LABWC-P07 | `hook/view-new-geometry-signal` | the line below it | `ddb4e25d03` | `793a37a539` | Hook: `view.events.new_geometry` signal (`include/view.h`). |
| LABWC-P08 | `hook/portal-meson-sources` | the line below it | `9b298c5037` | `801d5d20e6` | Hook: portal sources and protocol XML in meson. |
| LABWC-P09 | `hook/portal-managers-init` | the line below it | `8fda9f3450` | `9606738b9c` | Hook: create the portal managers in `server_init()`. |
| LABWC-P10 | `hook/emit-new-geometry` | the line below it | `c82a5de625` | `b8f5eca278` | Hook: initialise and emit `new_geometry`. |
| LABWC-P11 | `hook/foreign-toplevel-positioning` | the line below it | `02d811a87a` | `bc34c9dc51` | Hook: foreign-toplevel map and unmap reach toplevel positioning. |
| LABWC-P12 | `hook/surface-damage-forwarding` | the line below it | `e88eb59cd4` | `12c1120b2d` | Hook: forward `surface_damage` from xdg and Xwayland surfaces. |
| LABWC-P13 | `hook/menu-visibility-events` | the line below it | `dbbb1eb9eb` | `b346471faa` | Hook: compositor menu visibility events. |
| LABWC-P14 | `hook/xdg-popup-positioning` | the line below it | `b2aad0bcad` | `86890fcf0c` | Hook: xdg popup map reaches positioning. |
| LABWC-P15 | `hook/client-cursor-shapes` | the line below it | `25facd8d7f` | `b0c2cbfcdd` | Hook: forward client cursor shapes from `cursor.c` to P02. |
| LABWC-P16 | `hook/hide-cursor` | the line below it | `12d318f708` | `9a02c34f30` | Hook: `PORTAL_HIDE_CURSOR=1` silences the compositor's cursor sprite (the viewer paints it from P02). |
| LABWC-P17 | `hook/csd-margin-occlusion-guard` | the line below it | `97b8374bc9` | `0a591514f2` | Hook: guard CSD-margin resize with an occlusion check (uses P03). |
| LABWC-P18 | `hook/csd-resize-grip-hitbox` | the line below it | `db642a45d4` | `bc5bf6b7ec` | Hook: keep the CSD resize-grip hit box when shadows are clipped (with P05). |
| LABWC-P19 | `feat/portal-virtual-touch` | the line below it | `3001b40c78` | `9394c236a6` | Private `portal_virtual_touch_v1`: a client-made `wlr_touch` on the seat, driven by down/motion/up/cancel/frame, so a viewer's touchscreen reaches clients as `wl_touch`. wlroots has no virtual-touch protocol. |

Three earlier commits on `portal-0.20` only edited `PORTAL_PATCHES.md`. They are not patches and
are not carried.

## Moving to a new upstream release

1. Merge `upstream/master` into `master`.
2. Cut `upgrade/labwc-v<new>` from the new upstream tag.
3. For each patch, in ID order, cherry-pick its topic onto the new line. Where it does not apply,
   re-cut it as `<topic>-v<new>` on the right base, then cherry-pick that with `-x`.
4. Update this ledger on `master`: the table above and the current release.
5. Pin the new commit in Portal's `container/flake.nix`, then run the consumer trial.
6. Tag only after the trial passes.
