# Adding a component

A component is a directory containing a `component.yaml` manifest and (usually) a
`schema.yaml`. Built-in components live under `components/`; external ones live
in any repo and are loaded via `external_components` in a profile — **no fork of
the platform required**.

## Anatomy

```
adaptive-solar/
├── component.yaml     # metadata, compatibility, deps, build fragment, codegen
├── schema.yaml        # option types, ranges, defaults, cross-field rules
├── AdaptiveSolarPolicy.h / .cpp
├── tests/
└── README.md
```

## Manifest (`component.yaml`)

```yaml
id: adaptive-solar
type: policy            # board | module | policy | driver | app
version: 0.3.0
status: community-maintained
license: MIT
description: Adaptive solar power policy

compatibility:
  platform: ">=0.1 <0.2"
  policy_api: "^0.1"

requires:
  capabilities: [battery]     # must be provided by the board
  components: [scheduler]      # must be selected
conflicts: [simple-power]      # cannot coexist
auto_load: [telemetry-core]    # pulled in automatically

resources:                      # early footprint estimate (linker is final word)
  flash: 12000
  ram_static: 1200

# How selecting this component affects the generated build.
build:
  src_filter: ["+<policies/adaptive_solar/*.cpp>"]
  define_templates:             # option -> compile-time -D (strings auto-quoted)
    something: SOMETHING

# How the generated composition root constructs & wires the object.
codegen:
  class: AdaptiveSolarPolicy
  header: adaptive_solar/AdaptiveSolarPolicy.h
  register: setPowerPolicy      # kernel API call
  setters:                       # validated option -> C++ setter
    low_battery: setLowBatteryThreshold
    critical_battery: setCriticalBatteryThreshold
```

## Schema (`schema.yaml`)

```yaml
properties:
  low_battery:
    type: integer      # string | integer | number | boolean | duration | enum
    minimum: 1
    maximum: 99
    unit: percent
    default: 30
  critical_battery:
    type: integer
    minimum: 1
    maximum: 99
    default: 15
required: [low_battery, critical_battery]
rules:
  - less_than: [critical_battery, low_battery]
    message: critical_battery must be lower than low_battery
```

Options are validated and defaulted **before** any code generation, so mistakes
surface as clear config errors:

```
simple-power: critical_battery must be lower than low_battery

    critical_battery: 35
    low_battery: 30
```

## Boards

Boards additionally carry a `board:` block: architecture, framework, PlatformIO
board id + base env, `capabilities`, `limits` (e.g. `max_tx_power_dbm`), and an
optional `display:` (class + sources) so modules can request a display without
knowing the controller. Board `defines` encode the fixed pin map. See
[`components/boards/heltec-v3/component.yaml`](../components/boards/heltec-v3/component.yaml).

## Using an external component

```yaml
external_components:
  - source: github://vendor/corefw-radioboard-x1@v1.0.0
    components: [radioboard-x1]
  - source: ./my-local-components            # local path
    components: [experimental-policy]

board: radioboard-x1
```

Shorthands: `github://owner/repo@ref`, `github://pr#418` (test a PR directly),
`local://./path`. Every git source is pinned to a resolved commit in
`corefw.lock`.

## Testing your component

- **Schema tests** — valid + invalid config fixtures (`corefw validate`).
- **Compile tests** — the generated composition root must compile against the
  kernel headers (`make verify-gen` demonstrates the pattern).
- Boards should be tested against the Board API; modules against mocked services.
