# Vendored sources

The third-party code this project builds, committed as plain files. This project's changes are
made in place.

```
ZettaBridge  d6066b9b9e61de27041ba6e1f3c499c080c9e2b7  https://github.com/ZailoxTT/ZettaBridge
Dynarmic     86458a0bd369d63ba4c2ef812cacbb6c9080c065  https://github.com/Vita3K/dynarmic
```

- `ZettaBridge/` is ZettaBridge at that commit. Its licence, PolyForm Noncommercial 1.0.0 +
  PolyForm Perimeter 1.0.1, is in `ZettaBridge/LICENSE` and stays with the code.
- `ZettaBridge/third_party/dynarmic/` is Dynarmic at that commit, with ZettaBridge's
  `third_party/patches/dynarmic-*.patch` applied, which is how ZettaBridge builds it. Upstream it
  is a git submodule. Here it is plain files, and ZettaBridge's `.gitmodules` is removed.

The commit "Vendor ZettaBridge and Dynarmic, unchanged" holds both exactly as upstream ships them.
Everything that differs from that commit is this project's change:

```sh
git diff "$(git log --format=%H -1 --grep='^Vendor .*, unchanged$')" -- upstream/
```

To update to a newer upstream, branch from that commit and replace the files there with the new
upstream, again unchanged. Keep `sysroot/` and `build/`, which are ignored. Commit with a subject
of the same form, merge the branch, and update the commits above. Git then merges upstream's
changes three-way with this project's.
