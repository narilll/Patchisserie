# Patchisserie

## User manual

`docs/` is the user manual for Rack users: `README.md` is the front page, and each module has its own file (`filo.md`, `icing.md`, `lattice.md`).

- Every change a Rack user can notice (a port, button, menu item, key, default, behaviour, limitation or saved setting) updates the module's manual in the same commit, and the front page if needed.
- `test/docs.test.mjs` (part of `make test-js`) fails when a panel or menu label is missing from the manual. It checks names only. It does not check tooltips, slider labels, preset names or defaults, so keep those right by hand, and prose changes are on you.
- Style: English, second person, full sentences, plain words. Define a term on first use. Write panel labels in CAPS as printed, menu items in **bold** exactly as shown, and keys as `Cmd/Ctrl+…`. Use tables only for reference lists. No telegraphic fragments or shorthand.
- A task that changes user-visible behaviour is not done until the manual is updated and `make test-js` passes.
