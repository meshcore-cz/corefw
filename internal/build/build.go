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
	Upload      bool   // run `pio run -t upload` (flash) after generation
	Port        string // optional upload port for flashing
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

	// PlatformIO isn't available in every environment (CI, containers without
	// the toolchain). Rather than fail the whole build, degrade to generation
	// and tell the user how to compile/flash by hand.
	if opts.Compile || opts.Upload {
		if _, err := exec.LookPath("pio"); err != nil {
			opts.logf("PlatformIO (pio) not found in PATH — skipping %s.", verb(opts.Upload))
			opts.logf("  Install it (https://platformio.org) then run: %s", manualCmd(outDir, gen.EnvName, opts.Upload, opts.Port))
			return res, nil
		}
		opts.logf("%s firmware with PlatformIO", title(opts.Upload))
		if err := runPIO(outDir, gen.EnvName, opts.Upload, opts.Port, opts.Logf); err != nil {
			return res, err
		}
	}
	return res, nil
}

// runPIO invokes PlatformIO to build (or, when upload is true, build+flash) the
// generated env in outDir.
func runPIO(outDir, env string, upload bool, port string, logf func(string, ...any)) error {
	args := []string{"run", "-e", env}
	if upload {
		args = append(args, "-t", "upload")
		if port != "" {
			args = append(args, "--upload-port", port)
		}
	}
	cmd := exec.Command("pio", args...)
	cmd.Dir = outDir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func verb(upload bool) string {
	if upload {
		return "flash"
	}
	return "compile"
}

func title(upload bool) string {
	if upload {
		return "Flashing"
	}
	return "Compiling"
}

func manualCmd(outDir, env string, upload bool, port string) string {
	c := fmt.Sprintf("pio run -e %s -d %s", env, outDir)
	if upload {
		c += " -t upload"
		if port != "" {
			c += " --upload-port " + port
		}
	}
	return c
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
