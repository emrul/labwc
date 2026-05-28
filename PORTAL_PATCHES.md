# Portal Patches — `portal` branch

This fork carries a small set of patches on top of upstream labwc
to support [portal-agent](https://github.com/emrul/portal-agent),
a Wayland-native remote-desktop streaming agent.

The `portal` branch is maintained as a stack of focused commits on
top of an upstream tag (currently `0.8.3`), with `git rerere`
enabled so cross-tag rebases replay previously-resolved conflicts.

The fork is **buildable standalone** — the portal hooks add new
behavior but don't gate any upstream functionality, so a vanilla
labwc session against this branch should be indistinguishable from
upstream labwc until a portal-agent client connects.

---

## Commit topology

The branch is structured as three groups of commits on top of the
base tag. Group A and B never conflict on rebase (new files only).
Group C is where rerere earns its keep — those are the splices
into upstream code paths.

### Group A — sidecar feature modules (4 commits)

New `.c` / `.h` / `.xml` files only. None touch existing upstream
code, so no rebase conflicts are possible.

| Commit | Purpose |
|---|---|
| `feat(portal): content-type protocol manager` | wp_content_type_v1 (staging) so clients can hint surface content type — drives portal-agent's per-surface encoder selection. |
| `feat(portal): cursor-shape protocol manager` | Private `portal_cursor_shape_v1` — forwards cursor shape changes (client-side or labwc-internal) for client-side cursor paint. |
| `feat(portal): csd-margin helper module` | `portal_any_view_window_contains_cursor()` — used by the desktop.c hook to gate CSD-shadow resize matching against view occlusion. |
| `feat(portal): toplevel-positioning protocol manager` | Private `portal_toplevel_positioning_v1` — the load-bearing protocol carrying per-toplevel/popup/menu geometry, stacking, and visibility events. |

### Group B — upstream-quality feature commit (1 commit)

| Commit | Purpose |
|---|---|
| `Clip CSD shadows while preserving resize hitboxes` | Adds CSD shadow resize zones (`try_csd_margin_resize_context`) and visual content clipping (`apply_csd_content_clip`) in `desktop.c` + `xdg.c`. Candidate for proposing back to labwc upstream — independent of portal-agent in principle. |

### Group C — hook commits (12 commits)

Each commit splices a small amount of code into an upstream file
to make a portal feature observable. Order is dependency-respecting
so the tree builds at each commit.

| # | Commit | File(s) | Lines |
|---|---|---|---|
| 1 | `hook(portal): declare portal managers on struct server` | `include/labwc.h` | +3 |
| 2 | `hook(portal): declare view.events.new_geometry signal` | `include/view.h` | +1 |
| 3 | `hook(portal): register portal sources + XML in meson` | `protocols/meson.build`, `src/meson.build` | +7 |
| 4 | `hook(portal): create portal managers in server_init()` | `src/server.c` | +6 |
| 5 | `hook(portal): init + emit view.events.new_geometry` | `src/view.c` | +2 |
| 6 | `hook(portal): route foreign-toplevel map/unmap to positioning` | `src/foreign-toplevel/foreign.c` | +3 |
| 7 | `hook(portal): route xdg-popup map to positioning` | `src/xdg-popup.c` | +2 |
| 8 | `hook(portal): forward surface_damage from xdg + xwayland` | `src/xdg.c`, `src/xwayland.c` | +4 |
| 9 | `hook(portal): emit compositor-menu visibility events` | `src/menu/menu.c` | +5 |
| 10 | `hook(portal): forward client cursor shapes from cursor.c` | `src/input/cursor.c` | +4 |
| 11 | `hook(portal): PORTAL_HIDE_CURSOR env-gated cursor silencing` | `src/input/cursor.c` | +1 / -0 + 1 line modified |
| 12 | `hook(portal): guard CSD margin resize with occlusion check` | `src/desktop.c` | +1 / 1 line modified |

Each hook commit message is structured:

```
hook(portal): <subject>

<one-paragraph WHY — what would break or be wrong without this hook>

UPSTREAM TOUCHPOINT: <file>:<function>

SAFE-TO-DROP-IF: <condition under which the hook becomes obsolete>
```

The `SAFE-TO-DROP-IF` line is the lever for [Phase 5 upstream walk
audits](#upstream-audit-log) — when reading a new upstream release's
NEWS.md, search for anything that triggers any of the SAFE-TO-DROP-IF
conditions to identify hooks that have become obsolete.

---

## Rebase runbook

When upstream cuts a new tag (say `0.8.4`):

```sh
git fetch upstream
git checkout portal
git rebase 0.8.4                 # rerere replays prior resolutions
# Inspect any unresolved conflicts. The 4 sidecar commits + the 5
# pure-additive hook commits should need zero attention. Conflict
# attention typically lands on:
#   - meson.build registrations (commit 3) — list reorders
#   - server.c init (commit 4) — server_init() body shifts
#   - cursor.c hooks (commits 10, 11) — adjacent code changes

meson setup build --prefix=/usr --buildtype=release -Dxwayland=enabled
ninja -C build                   # quick smoke
./build/labwc -V                 # at least make sure it runs

# If clean and the smoke test runs:
git tag portal/0.8.4-1
git push origin portal portal/0.8.4-1

# If hooks were modified, bump to portal/0.8.4-2, etc.
```

Then in portal-agent:

```sh
cd container/labwc
git fetch && git checkout portal/0.8.4-1
cd ../..
git add container/labwc
git commit -m "bump labwc submodule to portal/0.8.4-1"
make build-labwc                 # end-to-end build verify
```

### Sharing rerere

`rerere` resolutions are stored in `.git/rr-cache/` per-clone, which
means each developer's machine accumulates its own resolution cache.
To share resolutions across machines, after a successful rebase:

```sh
# In the labwc fork clone:
git rerere gc                     # prune stale entries
tar czf /tmp/rr-cache.tgz .git/rr-cache
# Transfer to other machines and extract into their .git/
```

For a smaller team this is overkill — re-resolving 1-2 conflicts per
tag bump is acceptable.

---

## Upstream audit log

Walking forward from `0.8.3` through released tags (the next being
`0.8.4`, then `0.9.0` … `0.9.7`). For each tag, record:

- **Walked:** date of the rebase + smoke build
- **Relevant to portal-agent:** features or fixes worth leveraging
- **Hooks dropped/modified:** which SAFE-TO-DROP-IF lines fired
- **wlroots:** any wlroots minimum-version change (affects portal-
  agent's Containerfile.labwc apt deps too)

Entries follow:

### 0.8.4 — _pending_

_Walk not yet performed._

### 0.9.x series (0.9.0 … 0.9.7) — _pending_

_Walk not yet performed. Special attention to the wlroots-0.18 →
0.19 transition, which may invalidate hook anchors wholesale around
surface/state APIs (hook 8) and scene-tree APIs (hooks 9, abd415e2-
equivalent)._

---

## License

This fork is licensed under GPL-2.0-only, matching upstream labwc.
The portal protocol XML files (`protocols/portal-*.xml`) are
licensed under MIT/X11 to match the wayland-protocols convention
so they can be implemented by any compositor.
