# corefw CLI

`corefw` composes, validates, generates, builds and flashes modular MeshCore
firmware from YAML profiles.

```console
$ corefw --help
$ corefw --version
```

Profile arguments may be explicit paths or built-in profile names. For example,
these are equivalent when `profiles/heltec-v3-repeater.yaml` exists:

```console
$ corefw build profiles/heltec-v3-repeater.yaml
$ corefw build heltec-v3-repeater
```

## Global Flags

These flags work before or after the command name:

```console
$ corefw --plain build profiles/heltec-v3-repeater.yaml
$ corefw build profiles/heltec-v3-repeater.yaml --plain
```

| Flag | Purpose |
| --- | --- |
| `--plain` | Disable Bubble Tea, spinners, cursor control and decorative output. Useful for CI and logs. |
| `--verbose` | Show more detailed build and PlatformIO output. |
| `--no-color` | Disable color while keeping normal formatting. |
| `--version`, `-v` | Print the corefw version. |

corefw automatically uses plain output when stdout is not a terminal or when
`CI` is set.

## Common Workflows

Validate a profile without generating firmware:

```console
$ corefw validate profiles/heltec-v3-repeater.yaml
$ corefw validate profiles/heltec-v3-repeater.yaml --quiet
```

Inspect the resolved composition before generating anything:

```console
$ corefw plan profiles/heltec-v3-repeater.yaml
$ corefw plan profiles/heltec-v3-repeater.yaml --json
$ corefw plan profiles/heltec-v3-repeater.yaml --offline
```

Generate a PlatformIO project and compile it:

```console
$ corefw build profiles/heltec-v3-repeater.yaml
$ corefw build heltec-v3-repeater
$ corefw build profiles/heltec-v3-repeater.yaml --out build/custom
$ corefw build profiles/heltec-v3-repeater.yaml --firmware firmware
```

Generate only, without running PlatformIO:

```console
$ corefw build profiles/heltec-v3-repeater.yaml --no-compile
```

Flash a connected device:

```console
$ corefw flash profiles/heltec-v3-repeater.yaml
$ corefw flash heltec-v3-repeater
$ corefw flash profiles/heltec-v3-repeater.yaml --port /dev/ttyUSB0
$ corefw flash profiles/heltec-v3-repeater.yaml --monitor
```

Open a serial monitor:

```console
$ corefw monitor
$ corefw monitor --port /dev/ttyACM0
$ corefw monitor profiles/heltec-v3-repeater.yaml --baud 9600
$ corefw monitor --filter default --filter time
```

List detected devices:

```console
$ corefw devices
$ corefw devices --json
$ corefw devices --all
```

Check the local environment:

```console
$ corefw doctor
$ corefw doctor profiles/heltec-v3-repeater.yaml
$ corefw doctor --json
```

Clean generated output:

```console
$ corefw clean profiles/heltec-v3-repeater.yaml
$ corefw clean profiles/heltec-v3-repeater.yaml --dry-run
$ corefw clean --all
```

`clean` removes generated corefw output only. It refuses unsafe paths such as
the repository root, current working directory, filesystem root, home directory
or directories that do not look like generated corefw/PlatformIO output.

## Command Tree

```text
corefw
├── build <profile>
├── flash <profile>
├── validate <profile>
├── plan <profile>
├── doctor [profile]
├── devices
├── monitor [profile]
├── clean [profile]
├── component
│   ├── list
│   ├── show <component-id>
│   └── validate <component-path>
├── lock
│   └── show <profile>
├── completion
│   ├── bash
│   ├── zsh
│   ├── fish
│   └── powershell
└── version
```

## Component Commands

List built-in and available components:

```console
$ corefw component list
$ corefw component list --type board
$ corefw component list --status official
$ corefw component list --json
```

Show component details:

```console
$ corefw component show heltec-v3
$ corefw component show wio-tracker-l1 --json
```

Validate a component package or manifest without installing it:

```console
$ corefw component validate ./my-component
$ corefw component validate ./my-component/component.yaml
```

## Lockfiles

Print the resolved lockfile JSON:

```console
$ corefw lock show profiles/heltec-v3-repeater.yaml
$ corefw lock show profiles/heltec-v3-repeater.yaml --offline
```

The legacy form still works:

```console
$ corefw lock profiles/heltec-v3-repeater.yaml
```

## Shell Completion

Generate shell completion scripts:

```console
$ corefw completion bash
$ corefw completion zsh
$ corefw completion fish
$ corefw completion powershell
```

Install the generated script according to your shell's normal completion setup.

## Compatibility Aliases

These legacy commands are hidden from the main help output but still work:

| Legacy command | Preferred command |
| --- | --- |
| `corefw components` | `corefw component list` |
| `corefw boards` | `corefw component list --type board` |
| `corefw lock <profile>` | `corefw lock show <profile>` |

## Exit Codes

| Code | Meaning |
| --- | --- |
| `0` | Success, help or version output. |
| `1` | Runtime, build, environment or validation failure. |
| `2` | Invalid command usage or invalid flags. |
| `130` | Interrupted by Ctrl+C where practical. |
