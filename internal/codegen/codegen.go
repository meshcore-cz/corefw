// Package codegen turns a validated build plan into a concrete PlatformIO
// project: a platformio.ini env, a generated C++ entrypoint that constructs and
// registers the selected board/modules/policies with the kernel, and a build
// manifest. Only selected components are compiled in.
package codegen

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"text/template"

	"github.com/arnal/corefw/internal/manifest"
	"github.com/arnal/corefw/internal/resolve"
)

// Options configures generation.
type Options struct {
	// OutDir is where the generated project is written.
	OutDir string
	// FirmwareDir is the path to the corefw C++ kernel/component tree, made
	// available to the generated project via lib_extra_dirs.
	FirmwareDir string
}

// Result reports what was generated.
type Result struct {
	OutDir      string
	Files       []string
	Merged      MergedBuild
	EnvName     string
	Registrations []Registration
}

// Registration is one construct+register step in the generated entrypoint.
type Registration struct {
	Var      string
	Class    string
	Header   string
	Register string
	Setters  []setterCall
	Kind     manifest.Kind
	Category string
}

type setterCall struct {
	Method string
	Arg    string
}

// Generate writes the project and returns a summary.
func Generate(plan *resolve.Plan, opts Options) (*Result, error) {
	if opts.OutDir == "" {
		return nil, fmt.Errorf("codegen: OutDir is required")
	}
	env := envName(plan.Profile.Name)
	merged := merge(plan)
	regs := registrations(plan)

	if err := os.MkdirAll(filepath.Join(opts.OutDir, "src"), 0o755); err != nil {
		return nil, err
	}

	res := &Result{OutDir: opts.OutDir, Merged: merged, EnvName: env, Registrations: regs}

	pio, err := renderPlatformIO(plan, merged, env, opts.FirmwareDir)
	if err != nil {
		return nil, err
	}
	if err := write(opts.OutDir, "platformio.ini", pio, res); err != nil {
		return nil, err
	}

	main, err := renderEntrypoint(plan, regs)
	if err != nil {
		return nil, err
	}
	if err := write(opts.OutDir, filepath.Join("src", "corefw_main.generated.cpp"), main, res); err != nil {
		return nil, err
	}

	// Copy any board-support files (custom board JSON, linker scripts) the board
	// ships into the project's boards/ directory, where PlatformIO looks for
	// custom board definitions.
	if err := copyBoardSupport(plan, opts.OutDir, res); err != nil {
		return nil, err
	}
	return res, nil
}

func copyBoardSupport(plan *resolve.Plan, outDir string, res *Result) error {
	board := plan.Board.Component.Manifest.Board
	if board == nil {
		return nil
	}
	for _, rel := range board.PlatformIO.SupportFiles {
		data, err := plan.Board.Component.ReadFile(rel)
		if err != nil {
			return fmt.Errorf("board %s: reading support file %s: %w", plan.Board.ID(), rel, err)
		}
		dest := filepath.Join("boards", filepath.Base(rel))
		if err := write(outDir, dest, string(data), res); err != nil {
			return err
		}
	}
	for _, rel := range board.PlatformIO.VariantFiles {
		data, err := plan.Board.Component.ReadFile(rel)
		if err != nil {
			return fmt.Errorf("board %s: reading variant file %s: %w", plan.Board.ID(), rel, err)
		}
		dest := filepath.Join("variant", filepath.Base(rel))
		if err := write(outDir, dest, string(data), res); err != nil {
			return err
		}
	}
	return nil
}

func registrations(plan *resolve.Plan) []Registration {
	var out []Registration
	build := func(sel *resolve.Selected) {
		cg := sel.Component.Manifest.Codegen
		if cg.Class == "" {
			return // component contributes only build flags, no runtime object
		}
		reg := Registration{
			Var:      firstNonEmpty(cg.Var, slug(sel.ID())),
			Class:    cg.Class,
			Header:   cg.Header,
			Register: cg.Register,
			Kind:     sel.Component.Manifest.Type,
			Category: sel.Category,
		}
		for _, optKey := range sortedKeys(cg.Setters) {
			if v, ok := sel.Options[optKey]; ok {
				reg.Setters = append(reg.Setters, setterCall{Method: cg.Setters[optKey], Arg: cppArg(v)})
			}
		}
		out = append(out, reg)
	}
	build(plan.Board)
	for _, m := range plan.Modules {
		build(m)
	}
	for _, p := range plan.Policies {
		build(p)
	}
	return out
}

// corefwBaseDefines are the RadioLib configuration flags every corefw target
// build needs. COREFW_TARGET switches on the on-device code paths (e.g. the
// SX1262 driver). The RADIOLIB_EXCLUDE_* set trims unused radio families to
// keep the image small — matching the reference firmware's radio config.
var corefwBaseDefines = []string{
	"-D COREFW_TARGET=1",
	"-D RADIOLIB_STATIC_ONLY=1",
	"-D RADIOLIB_EXCLUDE_CC1101=1",
	"-D RADIOLIB_EXCLUDE_RF69=1",
	"-D RADIOLIB_EXCLUDE_SX1231=1",
	"-D RADIOLIB_EXCLUDE_SI443X=1",
	"-D RADIOLIB_EXCLUDE_RFM2X=1",
	"-D RADIOLIB_EXCLUDE_SX128X=1",
	"-D RADIOLIB_EXCLUDE_AFSK=1",
	"-D RADIOLIB_EXCLUDE_AX25=1",
	"-D RADIOLIB_EXCLUDE_HELLSCHREIBER=1",
	"-D RADIOLIB_EXCLUDE_MORSE=1",
	"-D RADIOLIB_EXCLUDE_APRS=1",
	"-D RADIOLIB_EXCLUDE_BELL=1",
	"-D RADIOLIB_EXCLUDE_RTTY=1",
	"-D RADIOLIB_EXCLUDE_SSTV=1",
}

// corefwBaseLibs are the libraries every corefw target build links. Crypto
// (Ed25519, SHA-256) is vendored in-tree, so only the radio + Arduino buses are
// external.
var corefwBaseLibs = []string{
	"SPI",
	"Wire",
	"jgromes/RadioLib @ ^7.6.0",
}

var pioTemplate = template.Must(template.New("pio").Parse(`; Generated by corefw — do not edit by hand.
; Profile: {{.Name}}
; Board:   {{.Board}}
[env:{{.Env}}]
platform = {{.Platform}}
framework = {{.Framework}}
board = {{.PioBoard}}
monitor_speed = 115200
{{- range .PlatformPackages}}
platform_packages = {{.}}
{{- end}}
{{- if .LdScript}}
board_build.ldscript = {{.LdScript}}
{{- end}}
{{- if .MaxSize}}
board_upload.maximum_size = {{.MaxSize}}
{{- end}}
build_flags =
{{- range .Defines}}
  {{.}}
{{- end}}
{{- range .Includes}}
  -I {{.}}
{{- end}}
build_src_filter =
{{- range .SrcFilter}}
  {{.}}
{{- end}}
lib_deps =
{{- range .LibDeps}}
  {{.}}
{{- end}}
`))

func renderPlatformIO(plan *resolve.Plan, mb MergedBuild, env, firmwareDir string) (string, error) {
	board := plan.Board.Component.Manifest.Board
	defines := append([]string{}, corefwBaseDefines...)
	// Emit the platform family define the target code guards on
	// (COREFW_TARGET && NRF52_PLATFORM, etc.).
	switch board.PlatformIO.BaseEnv {
	case "nrf52":
		defines = append(defines, "-D NRF52_PLATFORM=1")
	case "esp32":
		defines = append(defines, "-D ESP32_PLATFORM=1")
	}
	for _, k := range mb.sortedDefines() {
		v := mb.Defines[k]
		if v == "" {
			defines = append(defines, "-D "+k)
		} else {
			defines = append(defines, fmt.Sprintf("-D %s=%s", k, v))
		}
	}
	// Add the corefw firmware tree itself as a library (symlink:// references a
	// specific directory, unlike lib_extra_dirs which scans for sub-libraries).
	libDeps := append([]string{"symlink://" + firmwareDir}, corefwBaseLibs...)
	libDeps = append(libDeps, mb.LibDeps...)

	// Include paths: the kernel public headers (<corefw/...>) and the firmware
	// root (<drivers/...>, <boards/...>), then any board-specific include dirs.
	// All are rooted at the firmware tree so they resolve from build/<name>/.
	includes := []string{
		filepath.Join(firmwareDir, "kernel", "include"),
		firmwareDir,
		filepath.Join(firmwareDir, "drivers", "crypto", "ed25519"),
		filepath.Join(firmwareDir, "drivers", "crypto", "sha256"),
	}
	for _, inc := range mb.Includes {
		if filepath.IsAbs(inc) {
			includes = append(includes, inc)
		} else {
			includes = append(includes, filepath.Join(firmwareDir, inc))
		}
	}
	// The Arduino variant dir (if any) is project-relative so PlatformIO finds
	// variant.h during the core build.
	srcFilter := []string{"+<*.cpp>"}
	if len(board.PlatformIO.VariantFiles) > 0 {
		includes = append(includes, "variant")
		srcFilter = append(srcFilter, "+<../variant/variant.cpp>")
	}
	data := struct {
		Name, Board, Env, Platform, Framework, PioBoard, LdScript, FirmwareDir string
		PlatformPackages                                                       []string
		MaxSize                                                                int
		Defines, Includes, SrcFilter, LibDeps                                  []string
	}{
		Name:             plan.Profile.Name,
		Board:            plan.Board.ID(),
		Env:              env,
		Platform:         board.PlatformIO.Platform,
		Framework:        board.PlatformIO.Framework,
		PlatformPackages: board.PlatformIO.PlatformPackages,
		PioBoard:         board.PlatformIO.Board,
		LdScript:         board.PlatformIO.LdScript,
		MaxSize:          board.PlatformIO.MaxSize,
		FirmwareDir:      firmwareDir,
		Defines:          defines,
		Includes:         includes,
		// The env compiles the generated composition root (and the Arduino
		// variant, if the board ships one); the corefw kernel, drivers, boards
		// and modules build themselves as a library (firmware/library.json)
		// once their headers are referenced.
		SrcFilter: srcFilter,
		LibDeps:   libDeps,
	}
	var buf bytes.Buffer
	if err := pioTemplate.Execute(&buf, data); err != nil {
		return "", err
	}
	return buf.String(), nil
}

func write(outDir, rel, content string, res *Result) error {
	full := filepath.Join(outDir, rel)
	if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(full, []byte(content), 0o644); err != nil {
		return err
	}
	res.Files = append(res.Files, rel)
	return nil
}

func envName(name string) string {
	if name == "" {
		return "corefw"
	}
	return "corefw_" + slug(name)
}

func slug(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		switch {
		case r >= 'a' && r <= 'z', r >= '0' && r <= '9':
			b.WriteRune(r)
		default:
			b.WriteByte('_')
		}
	}
	return strings.Trim(b.String(), "_")
}

// cppArg renders a validated option value as a C++ literal.
func cppArg(v any) string {
	switch t := v.(type) {
	case string:
		return strconv.Quote(t)
	case bool:
		if t {
			return "true"
		}
		return "false"
	case int64:
		return strconv.FormatInt(t, 10)
	case int:
		return strconv.Itoa(t)
	case float64:
		return strconv.FormatFloat(t, 'g', -1, 64)
	default:
		return fmt.Sprintf("%v", t)
	}
}

// defineValue renders a value for a -D define (no C++ quoting).
func defineValue(v any) string {
	switch t := v.(type) {
	case string:
		return t
	case bool:
		if t {
			return "1"
		}
		return "0"
	case int64:
		return strconv.FormatInt(t, 10)
	case int:
		return strconv.Itoa(t)
	case float64:
		return strconv.FormatFloat(t, 'g', -1, 64)
	default:
		return fmt.Sprintf("%v", t)
	}
}

func quote(s string) string { return `'"` + s + `"'` }

func firstNonEmpty(a, b string) string {
	if a != "" {
		return a
	}
	return b
}

func sortedKeys(m map[string]string) []string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return ks
}
