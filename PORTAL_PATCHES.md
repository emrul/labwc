# Portal Patches — `portal` branch

This fork carries a small set of patches on top of upstream labwc
to support [portal-agent](https://github.com/emrul/portal-agent),
a Wayland-native remote-desktop streaming agent.

The `portal` branch is maintained as a stack of focused commits on
top of an upstream tag (currently `0.9.7`), with `git rerere`
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
| 6 | `hook(portal): route foreign-toplevel map/unmap to positioning` | `src/foreign-toplevel/foreign.{c,h}` | +3 hook / +1 accessor decl / +5 accessor body †|
| 7 | `hook(portal): route xdg-popup map to positioning` | `src/xdg-popup.c` | +2 |
| 8 | `hook(portal): forward surface_damage from xdg + xwayland` | `src/xdg.c`, `src/xwayland.c` | +4 |
| 9 | `hook(portal): emit compositor-menu visibility events` | `src/menu/menu.c` | +5 |
| 10 | `hook(portal): forward client cursor shapes from cursor.c` | `src/input/cursor.c` | +4 |
| 11 | `hook(portal): PORTAL_HIDE_CURSOR env-gated cursor silencing` | `src/input/cursor.c` | +1 / -0 + 1 line modified |
| 12 | `hook(portal): guard CSD margin resize with occlusion check` | `src/desktop.c` | +1 / 1 line modified |

† **0.9.x accessor.** As of labwc 0.9.0 `struct foreign_toplevel` is
opaque outside `foreign.c`. The positioning manager needs the backing
`struct view` to read geometry/state, so hook 6 also adds a
`foreign_toplevel_get_view()` accessor (declared in `foreign.h`, body in
`foreign.c`). This keeps the handle's `foreign_toplevel *` identity and
all public signatures unchanged — it's the lowest-risk adaptation to the
struct going opaque. SAFE-TO-DROP-IF: upstream re-exposes the view
pointer publicly, or the positioning manager is rewritten to track views
by another handle.

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

When upstream cuts a new tag we want (say `0.9.8`):

```sh
git fetch upstream
git checkout portal
git rebase 0.9.8                 # rerere replays prior resolutions
# Inspect any unresolved conflicts. The 4 sidecar commits + the 5
# pure-additive hook commits should need zero attention. Conflict
# attention typically lands on:
#   - meson.build registrations (commit 3) — list reorders
#   - server.c init (commit 4) — server_init() body shifts
#   - cursor.c hooks (commits 10, 11) — adjacent code changes
# A wlroots major bump (as in the 0.8.3→0.9.7 walk) is the heavy
# case — see the audit log for the kinds of API edits to expect.

meson setup build --prefix=/usr --buildtype=release -Dxwayland=enabled
ninja -C build                   # quick smoke
./build/labwc -V                 # at least make sure it runs

# If clean and the smoke test runs:
git tag portal/0.9.8-1
git push origin portal portal/0.9.8-1

# If hooks were modified, bump to portal/0.9.8-2, etc.
```

Then in portal-agent:

```sh
cd container/labwc
git fetch && git checkout portal/0.9.8-1
cd ../..
git add container/labwc
git commit -m "bump labwc submodule to portal/0.9.8-1"
# If the new tag changes the wlroots/ffmpeg deps, bump
# container/Containerfile.labwc + container/Containerfile + the
# build-labwc image tag in container/Makefile to match (see the
# 0.9.7 walk for the SONAME-rename checklist).
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

### 0.8.3 → 0.9.7 — walked 2026-05-28

Done as a single forward rebase (`0.8.3` → `0.9.0` → `0.9.7`) rather
than tag-by-tag; `rerere` captured conflict resolutions at the `0.9.0`
and `0.9.7` legs. Smoke build is green in the container
(`make build-labwc`, `[211/211] Linking target labwc`).

**wlroots:** **0.18 → 0.19** (this is the headline). Upstream
recommends ≥ 0.19.1 ([#2943]); Ubuntu 26.04 LTS ships `0.19.2-1`, so
`Containerfile.labwc` now build-deps `libwlroots-0.19-dev` and the
runtime image installs `libwlroots-0.19`. labwc `master` has already
moved on to wlroots-0.20, which is why we pin the **0.9.x** line (the
maintained wlroots-0.19 branch) rather than chasing `master`. See the
[self-built wlroots escape hatch](#self-built-wlroots-escape-hatch)
for the 0.20+ path.

**Hooks / features modified during the port** (topology unchanged —
still 4 + 1 + 12 commits — but these commits' *contents* were adapted
to the wlroots-0.19 / labwc-0.9 API):

| Commit | API change that forced the edit |
|---|---|
| `feat: toplevel-positioning` | `struct foreign_toplevel` went opaque → all view access routes through the new `foreign_toplevel_get_view()` accessor (17 sites). `view_get_string_prop(v,"app_id"/"title")` removed → `v->app_id` / `v->title`. Destroy listener moved from `toplevel->events.toplevel_destroy` to `view->events.destroy`. |
| `feat: csd-margin helper` | Node-descriptor enum unified: `LAB_NODE_DESC_VIEW` → `LAB_NODE_VIEW`, `desc->data` → `desc->view`. |
| `Clip CSD shadows` (Group B) | `wlr_xdg_surface_get_geometry()` removed → `xdg_surface->geometry` field. SSD part enum folded into the node-type enum: `csd_margin_resize_part()` now returns `enum lab_node_type`; `view->ssd_enabled` → `view->ssd_mode != LAB_SSD_MODE_NONE`; `LAB_SSD_{CORNER,EDGE}_*` → `LAB_NODE_{CORNER,BORDER}_*`. `get_cursor_context()` restructured (0.9.x). |
| hook 6 `route foreign-toplevel map/unmap` | Adapted to the new sub-struct API (`wlr_foreign_toplevel_init(&t->wlr_toplevel, view)`); carries the `foreign_toplevel_get_view()` accessor (see † above). |
| hook 8 `forward surface_damage` | `xdg_surface->geometry` field access (same removal as Clip CSD shadows). |
| hook 9 `emit compositor-menu visibility` | Consolidated the broadcast into the `open_menu()` choke point back at the 0.9.0 leg, so it sailed through 0.9.2/0.9.3's large `menu.c` rewrites untouched. |
| hook 12 `guard CSD margin resize` | Node-type enum migration (as Clip CSD shadows). |

The remaining 9 commits (content-type, cursor-shape sidecars; hooks
1–5, 7, 10, 11) replayed with zero edits.

**Features worth leveraging** (not yet adopted by portal-agent; logged
for follow-up):

- **`ext-image-copy-capture` protocol** (added 0.9.0, [#2740]). Our
  capture path is wlr-screencopy v3 today (`ext-image-copy-capture-v1`
  was *not* packaged on the 0.8.3 stack — see portal-agent memory
  `project_labwc_protocols`). labwc now implements it; it's the
  modern, damage-aware capture protocol and a candidate replacement
  for the screencopy path. Worth a spike.
- **`<core><primarySelection>` config** (added 0.9.0, [#2832]). Lets
  Chromium / Electron middle-click autoscroll work *without*
  inadvertently pasting the primary selection — directly relevant to
  the streamed-Chromium UX. Consider enabling in `labwc-rc.xml`.
- **Client buffer size bumped to 1 MiB** (0.9.4, [#3400]). Reduces
  client disconnects under high event volume (e.g. high-refresh
  pointer streams). Relevant because portal-agent injects synthetic
  input via wdotool/virtual-pointer; less likely to overrun now.
- `xdg_shell` v3→v6 and `wl_compositor` v5→v6 (0.9.0). No portal-agent
  action; noted for protocol-version awareness.

**Per-release portal relevance** (0.8.4–0.9.7): `0.8.4` window-switcher
icons + bug fixes (none portal-relevant). `0.9.1`–`0.9.3` bug fixes +
config/menu features (none load-bearing for our hooks). `0.9.4`
requires **wayland ≥ 1.22.90** (26.04 ships 1.24 — fine) + the buffer
bump above. `0.9.5`/`0.9.6` XWayland always-on-top handling +
bug fixes. `0.9.7` small fixes (window-switcher thumbnail texture,
tablet tilt). None fired a `SAFE-TO-DROP-IF` condition: every portal
hook is still required.

**Headless note:** the wlroots-0.19 DRM-backend regressions called out
in upstream's NEWS.md "Notes on wlroots-0.19" (outputs destroyed/
recreated on VT switch, layer-shell reappearance) do **not** affect
portal-agent — we run the headless backend (`WLR_BACKENDS=headless`),
so there is no DRM/VT path.

---

## Self-built wlroots escape hatch

We deliberately do **not** build wlroots from source today: Ubuntu
26.04 LTS packages `libwlroots-0.19-dev`, which is exactly what the
`0.9.x` labwc line needs, so we get it for free from apt. This keeps
`Containerfile.labwc` a thin "apt build-deps + meson + ninja" image.

That free ride ends when we want a labwc release that requires a
wlroots newer than what the base Ubuntu LTS packages — i.e. moving to
labwc `master` / `0.20.x`, which builds against **wlroots-0.20**. At
that point the options are, in increasing order of burden:

1. **Wait for the next Ubuntu LTS** to package the newer wlroots and
   bump `BASE_IMAGE`. Zero maintenance; we just stay one wlroots
   release behind. This is the default — only leave it if a specific
   `0.20+` feature is load-bearing for portal-agent.
2. **Pin a PPA / newer interim Ubuntu** that packages the wlroots we
   need. Low burden, but interim releases have ~9-month support
   windows (the Plucky EOL trap we just escaped — do not re-anchor on
   a short-support release).
3. **Build wlroots from source** in a dedicated stage and install it
   into `Containerfile.labwc` before the labwc build. Highest burden:
   we then own wlroots's own meson deps (libdrm, libinput, Pixman,
   Wayland, hwdata, seatd, libdisplay-info versions) and must track
   its API churn ourselves. Sketch:

   ```dockerfile
   FROM ubuntu:<base> AS wlroots_builder
   RUN apt-get build-dep -y wlroots   # or hand-list meson deps
   RUN git clone --branch 0.20.0 https://gitlab.freedesktop.org/wlroots/wlroots
   RUN meson setup build wlroots -Dprefix=/usr -Dexamples=false \
       && ninja -C build install      # DESTDIR for a copyable tree
   # then COPY the install tree into the labwc build stage
   ```

   **Do not take on (3) until the labwc port itself is green against
   the source-built wlroots** — prove the labwc build first with a
   throwaway source-wlroots stage, then decide whether the feature is
   worth the standing maintenance cost.

The trigger to revisit this is a labwc `0.20.x` (or later) release
carrying something portal-agent actually needs; until then `0.9.x` on
distro wlroots-0.19 is the supported configuration.

---

## License

This fork is licensed under GPL-2.0-only, matching upstream labwc.
The portal protocol XML files (`protocols/portal-*.xml`) are
licensed under MIT/X11 to match the wayland-protocols convention
so they can be implemented by any compositor.
