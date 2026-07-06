# Extensions

Extensions are **optional add-on components** that enrich an existing role
(repeater, companion, …) instead of replacing it. They exist so that the kind of
"small modification" people normally fork the whole firmware for becomes a
self-contained component you drop into a profile — or pull from its own git repo
— with **no fork of the platform and no edits to the core modules**.

An extension is just a `module`-type component that overrides one of the
kernel's extension hooks. It gets registered like any other module, sits in the
module list, and the kernel calls its hook at the right moment.

## Available hooks

| Hook | Called when | Use it to |
|------|-------------|-----------|
| `Module::decorateAdvert(proto::AdvertData&)` | just before every self-advert is signed (`Kernel::applyAdvertDecorators`) | add fields to the advert app_data (feature flags, extra location policy, …) |

More hooks are added to `Module` as needed; because they default to a no-op,
adding one never breaks existing components.

## Anatomy

Identical to any component (see [`docs/adding-a-component.md`](../../docs/adding-a-component.md)) —
and **fully self-encapsulated in one directory**:

```
cz-advert-features/
├── component.yaml       # id, type: module, codegen (header + sources + setters)
├── schema.yaml          # option types/ranges/defaults
├── CzAdvertFeatures.h   # the C++ — lives right here, not in firmware/
└── README.md
```

The C++ lives in the component directory. `codegen.sources` lists the files to
copy into the generated project, and `codegen.header` is given relative to that
directory. The copy goes through the component's own filesystem, so the exact
same directory works whether it is built-in, a local path, or fetched from git.
That is what makes an extension a single, git-linkable unit with no files
scattered across the platform tree.

## Using one

In a profile, list it alongside the role it extends:

```yaml
modules:
  - type: repeater
    advert_name: "CoreFW CZ Repeater"
  - type: cz-advert-features        # the extension
    feat1: 0x0001
    feat2: 0x00A0
```

## Shipping one from its own repo (the "no fork" flex)

An extension doesn't have to live in this tree. Put the same
`component.yaml` + `schema.yaml` + header in your own repo and pull it in:

```yaml
external_components:
  - source: github://youruser/corefw-cz-advert-features@v0.1.0
    components: [cz-advert-features]

modules:
  - type: repeater
  - type: cz-advert-features
    feat1: 0x0001
```

Every git source is pinned to a resolved commit in `corefw.lock`.

## Example: `cz-advert-features`

The corefw-native replacement for the
[meshcore-cz `cz-advert-features` fork](https://github.com/meshcore-dev/MeshCore/compare/main...meshcore-cz:MeshCoreCZ:cz-advert-features).
See [`cz-advert-features/`](cz-advert-features/) and its README for the full
mapping of what the fork changed vs. what the extension does.
