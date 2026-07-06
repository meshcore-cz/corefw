// Package build orchestrates the full corefw pipeline: load & substitute the
// profile, fetch external components, resolve and validate the component graph,
// generate the PlatformIO project, write the lockfile, and (optionally) invoke
// PlatformIO to produce the firmware binary.
package build

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/arnal/corefw/internal/codegen"
	"github.com/arnal/corefw/internal/lock"
	"github.com/arnal/corefw/internal/profile"
	"github.com/arnal/corefw/internal/registry"
	"github.com/arnal/corefw/internal/resolve"
	"github.com/arnal/corefw/internal/source"
)

// Options controls a build.
type Options struct {
	ProfilePath string
	OutDir      string // default: build/<name>
	FirmwareDir string // path to corefw C++ tree (default: ./firmware)
	Compile     bool   // run `pio run` after generation
	Logf        func(format string, args ...any)
}

// Result summarises a completed build.
type Result struct {
	Plan     *resolve.Plan
	Gen      *codegen.Result
	Lockfile *lock.Lockfile
	LockPath string
	OutDir   string
}

func (o *Options) logf(format string, args ...any) {
	if o.Logf != nil {
		o.Logf(format, args...)
	}
}

// Run executes the pipeline up to (and optionally including) compilation.
func Run(opts Options) (*Result, error) {
	opts.logf("Loading profile %s", opts.ProfilePath)
	p, err := profile.Load(opts.ProfilePath)
	if err != nil {
		return nil, err
	}
	if p.Name == "" {
		p.Name = baseName(opts.ProfilePath)
	}

	reg, err := registry.New()
	if err != nil {
		return nil, err
	}

	if len(p.External) > 0 {
		opts.logf("Resolving external components")
		fetcher := &source.Fetcher{
			BaseDir: filepath.Dir(mustAbs(opts.ProfilePath)),
			Logf:    opts.Logf,
		}
		if _, err := fetcher.Resolve(reg, p.External); err != nil {
			return nil, err
		}
	}

	opts.logf("Resolving component graph")
	plan, err := resolve.Resolve(p, reg)
	if err != nil {
		return nil, err
	}
	for _, w := range plan.Warnings {
		opts.logf("  warning: %s", w)
	}

	outDir := opts.OutDir
	if outDir == "" {
		outDir = filepath.Join("build", p.Name)
	}
	firmwareDir := opts.FirmwareDir
	if firmwareDir == "" {
		firmwareDir = "firmware"
	}
	if abs, err := filepath.Abs(firmwareDir); err == nil {
		firmwareDir = abs
	}

	opts.logf("Generating project in %s", outDir)
	gen, err := codegen.Generate(plan, codegen.Options{OutDir: outDir, FirmwareDir: firmwareDir})
	if err != nil {
		return nil, err
	}

	lf := lock.Build(plan)
	lockPath, err := lock.WriteLock(outDir, lf)
	if err != nil {
		return nil, err
	}
	opts.logf("Wrote %s (config %s)", lockPath, short(lf.ConfigHash))

	res := &Result{Plan: plan, Gen: gen, Lockfile: lf, LockPath: lockPath, OutDir: outDir}

	if opts.Compile {
		opts.logf("Compiling firmware with PlatformIO")
		if err := compile(outDir, gen.EnvName, opts.Logf); err != nil {
			return res, err
		}
	}
	return res, nil
}

func compile(outDir, env string, logf func(string, ...any)) error {
	if _, err := exec.LookPath("pio"); err != nil {
		return fmt.Errorf("pio (PlatformIO) not found in PATH; generated project is in %s", outDir)
	}
	cmd := exec.Command("pio", "run", "-e", env)
	cmd.Dir = outDir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func baseName(path string) string {
	b := filepath.Base(path)
	return b[:len(b)-len(filepath.Ext(b))]
}

func mustAbs(p string) string {
	if abs, err := filepath.Abs(p); err == nil {
		return abs
	}
	return p
}

func short(s string) string {
	if len(s) > 12 {
		return s[:12]
	}
	return s
}
