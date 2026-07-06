// Package cli implements the corefw command-line interface.
package cli

import (
	"flag"
	"fmt"
	"os"
	"text/tabwriter"

	"github.com/arnal/corefw/internal/build"
	"github.com/arnal/corefw/internal/lock"
	"github.com/arnal/corefw/internal/manifest"
	"github.com/arnal/corefw/internal/profile"
	"github.com/arnal/corefw/internal/registry"
	"github.com/arnal/corefw/internal/resolve"
)

const usage = `corefw — an ESPHome-style composition & build platform for MeshCore firmware.

Usage:
  corefw <command> [flags]

Commands:
  build <profile.yaml>      Resolve, generate and (optionally) compile a firmware image
  validate <profile.yaml>   Validate a profile and its component graph without generating
  components                List available components (boards, modules, policies)
  boards                    List available board packages
  lock <profile.yaml>       Print the resolved lockfile for a profile
  version                   Print the corefw platform version

Run 'corefw <command> -h' for command-specific flags.
`

// Main is the entrypoint; returns a process exit code.
func Main(args []string) int {
	if len(args) < 1 {
		fmt.Print(usage)
		return 2
	}
	cmd, rest := args[0], args[1:]
	switch cmd {
	case "build":
		return cmdBuild(rest)
	case "validate":
		return cmdValidate(rest)
	case "components":
		return cmdComponents(rest, "")
	case "boards":
		return cmdComponents(rest, manifest.KindBoard)
	case "lock":
		return cmdLock(rest)
	case "version":
		fmt.Printf("corefw %s\n", lock.PlatformVersion)
		return 0
	case "-h", "--help", "help":
		fmt.Print(usage)
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n%s", cmd, usage)
		return 2
	}
}

func cmdBuild(args []string) int {
	fs := flag.NewFlagSet("build", flag.ContinueOnError)
	out := fs.String("out", "", "output directory (default build/<name>)")
	firmware := fs.String("firmware", "firmware", "path to the corefw C++ firmware tree")
	compile := fs.Bool("compile", false, "run PlatformIO to produce the firmware binary")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if fs.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "build requires exactly one profile path")
		return 2
	}
	res, err := build.Run(build.Options{
		ProfilePath: fs.Arg(0),
		OutDir:      *out,
		FirmwareDir: *firmware,
		Compile:     *compile,
		Logf:        logln,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "\nerror: %v\n", err)
		return 1
	}
	fmt.Printf("\nGenerated %s\n", res.OutDir)
	for _, f := range res.Gen.Files {
		fmt.Printf("  %s\n", f)
	}
	fmt.Printf("  %s\n", "corefw.lock")
	fmt.Printf("\nBuild with: pio run -e %s -d %s\n", res.Gen.EnvName, res.OutDir)
	return 0
}

func cmdValidate(args []string) int {
	if len(args) != 1 {
		fmt.Fprintln(os.Stderr, "validate requires exactly one profile path")
		return 2
	}
	plan, err := loadPlan(args[0])
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 1
	}
	fmt.Printf("✓ %s is valid\n", args[0])
	fmt.Printf("  board:    %s\n", plan.Board.ID())
	fmt.Printf("  modules:  %s\n", ids(plan.Modules))
	fmt.Printf("  policies: %s\n", policyIDs(plan.Policies))
	for _, w := range plan.Warnings {
		fmt.Printf("  warning:  %s\n", w)
	}
	return 0
}

func cmdComponents(args []string, only manifest.Kind) int {
	reg, err := registry.New()
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 1
	}
	w := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
	fmt.Fprintln(w, "ID\tTYPE\tVERSION\tSTATUS\tDESCRIPTION")
	for _, c := range reg.All() {
		if only != "" && c.Manifest.Type != only {
			continue
		}
		m := c.Manifest
		fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\n", m.ID, m.Type, m.Version, m.Status, m.Description)
	}
	w.Flush()
	return 0
}

func cmdLock(args []string) int {
	if len(args) != 1 {
		fmt.Fprintln(os.Stderr, "lock requires exactly one profile path")
		return 2
	}
	plan, err := loadPlan(args[0])
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 1
	}
	lf := lock.Build(plan)
	data, _ := jsonIndent(lf)
	fmt.Println(string(data))
	return 0
}

// loadPlan runs the front half of the pipeline (parse + resolve) without
// generating, for validate/lock.
func loadPlan(path string) (*resolve.Plan, error) {
	p, err := profile.Load(path)
	if err != nil {
		return nil, err
	}
	if p.Name == "" {
		p.Name = baseName(path)
	}
	reg, err := registry.New()
	if err != nil {
		return nil, err
	}
	// Note: validate/lock intentionally do not fetch git sources; they operate
	// on built-in + already-cached components. `build` performs fetching.
	return resolve.Resolve(p, reg)
}

func logln(format string, args ...any) {
	fmt.Printf(format+"\n", args...)
}
