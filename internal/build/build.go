// Package build orchestrates the full corefw pipeline: load & substitute the
// profile, fetch external components, resolve and validate the component graph,
// generate the PlatformIO project, write the lockfile, and (optionally) invoke
// PlatformIO to produce the firmware binary.
package build

import (
	"context"
	"errors"
	"fmt"
	"io"
	"path/filepath"
	"time"

	"github.com/arnal/corefw/internal/codegen"
	"github.com/arnal/corefw/internal/lock"
	"github.com/arnal/corefw/internal/platformio"
	"github.com/arnal/corefw/internal/profile"
	"github.com/arnal/corefw/internal/progress"
	"github.com/arnal/corefw/internal/registry"
	"github.com/arnal/corefw/internal/resolve"
	"github.com/arnal/corefw/internal/source"
)

// Options controls a build.
type Options struct {
	Context     context.Context
	ProfilePath string
	OutDir      string // default: build/<name>
	FirmwareDir string // path to corefw C++ tree (default: ./firmware)
	Compile     bool   // run `pio run` after generation
	Upload      bool   // run `pio run -t upload` (flash) after generation
	Port        string // optional upload port for flashing
	PIOOutput   io.Writer
	Reporter    progress.Reporter
	Logf        func(format string, args ...any)
}

// Result summarises a completed build.
type Result struct {
	Plan       *resolve.Plan
	Gen        *codegen.Result
	Lockfile   *lock.Lockfile
	LockPath   string
	OutDir     string
	PIOLog     string
	UploadPort string
}

func (o *Options) logf(format string, args ...any) {
	if o.Logf != nil {
		o.Logf(format, args...)
	}
}

// Run executes the pipeline up to (and optionally including) compilation.
func Run(opts Options) (*Result, error) {
	reporter := progress.Safe(opts.Reporter)
	report := func(phase progress.Phase, status progress.Status, level progress.Level, message, detail string) {
		event := progress.Event{
			Time:    time.Now(),
			Phase:   phase,
			Status:  status,
			Level:   level,
			Message: message,
			Detail:  detail,
		}
		reporter.Report(event)
		opts.logEvent(event)
	}

	report(progress.PhaseLoadProfile, progress.StatusStarted, progress.LevelInfo, "Loading profile", opts.ProfilePath)
	p, err := profile.Load(opts.ProfilePath)
	if err != nil {
		report(progress.PhaseLoadProfile, progress.StatusFailed, progress.LevelError, "Failed to load profile", err.Error())
		return nil, err
	}
	if p.Name == "" {
		p.Name = baseName(opts.ProfilePath)
	}
	report(progress.PhaseLoadProfile, progress.StatusCompleted, progress.LevelInfo, "Loaded profile", p.Name)

	reg, err := registry.New()
	if err != nil {
		return nil, err
	}

	if len(p.External) > 0 {
		report(progress.PhaseFetchComponents, progress.StatusStarted, progress.LevelInfo, "Resolving external components", "")
		fetcher := &source.Fetcher{
			BaseDir: filepath.Dir(mustAbs(opts.ProfilePath)),
			Logf: func(format string, args ...any) {
				msg := fmt.Sprintf(format, args...)
				report(progress.PhaseFetchComponents, progress.StatusProgress, progress.LevelInfo, msg, "")
			},
		}
		if _, err := fetcher.Resolve(reg, p.External); err != nil {
			report(progress.PhaseFetchComponents, progress.StatusFailed, progress.LevelError, "Failed to resolve external components", err.Error())
			return nil, err
		}
		report(progress.PhaseFetchComponents, progress.StatusCompleted, progress.LevelInfo, "Resolved external components", "")
	} else {
		report(progress.PhaseFetchComponents, progress.StatusSkipped, progress.LevelInfo, "No external components", "")
	}

	report(progress.PhaseResolveComponents, progress.StatusStarted, progress.LevelInfo, "Resolving component graph", "")
	plan, err := resolve.Resolve(p, reg)
	if err != nil {
		report(progress.PhaseResolveComponents, progress.StatusFailed, progress.LevelError, "Failed to resolve component graph", err.Error())
		return nil, err
	}
	for _, w := range plan.Warnings {
		report(progress.PhaseResolveComponents, progress.StatusWarning, progress.LevelWarning, "Component graph warning", w)
	}
	report(progress.PhaseResolveComponents, progress.StatusCompleted, progress.LevelInfo, "Resolved component graph", "")

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

	report(progress.PhaseGenerateProject, progress.StatusStarted, progress.LevelInfo, "Generating PlatformIO project", outDir)
	gen, err := codegen.Generate(plan, codegen.Options{OutDir: outDir, FirmwareDir: firmwareDir})
	if err != nil {
		report(progress.PhaseGenerateProject, progress.StatusFailed, progress.LevelError, "Failed to generate PlatformIO project", err.Error())
		return nil, err
	}
	report(progress.PhaseGenerateProject, progress.StatusCompleted, progress.LevelInfo, "Generated PlatformIO project", outDir)

	report(progress.PhaseWriteLockfile, progress.StatusStarted, progress.LevelInfo, "Writing lockfile", "")
	lf := lock.Build(plan)
	lockPath, err := lock.WriteLock(outDir, lf)
	if err != nil {
		report(progress.PhaseWriteLockfile, progress.StatusFailed, progress.LevelError, "Failed to write lockfile", err.Error())
		return nil, err
	}
	report(progress.PhaseWriteLockfile, progress.StatusCompleted, progress.LevelInfo, "Wrote lockfile", fmt.Sprintf("%s (config %s)", lockPath, short(lf.ConfigHash)))

	res := &Result{Plan: plan, Gen: gen, Lockfile: lf, LockPath: lockPath, OutDir: outDir}

	// PlatformIO isn't available in every environment (CI, containers without
	// the toolchain). Rather than fail the whole build, degrade to generation
	// and tell the user how to compile/flash by hand.
	if opts.Compile || opts.Upload {
		report(progress.PhasePrepareToolchain, progress.StatusStarted, progress.LevelInfo, "Preparing PlatformIO", "")
		pioRes, err := platformio.Run(platformio.Options{
			Context:   opts.Context,
			Dir:       outDir,
			Env:       gen.EnvName,
			Upload:    opts.Upload,
			Port:      opts.Port,
			RawOutput: opts.PIOOutput,
			Reporter:  reporter,
		})
		if errors.Is(err, platformio.ErrNotFound) {
			report(progress.PhasePrepareToolchain, progress.StatusSkipped, progress.LevelInfo, "PlatformIO not found; skipping "+verb(opts.Upload), platformio.ManualCommand(outDir, gen.EnvName, opts.Upload, opts.Port))
			return res, nil
		}
		if pioRes != nil {
			res.PIOLog = pioRes.RawLogPath
			res.UploadPort = pioRes.UploadPort
		}
		if err != nil {
			return res, err
		}
	}
	return res, nil
}

func verb(upload bool) string {
	if upload {
		return "flash"
	}
	return "compile"
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

func (o *Options) logEvent(event progress.Event) {
	if o.Logf == nil {
		return
	}
	switch event.Status {
	case progress.StatusStarted, progress.StatusCompleted, progress.StatusSkipped, progress.StatusWarning, progress.StatusFailed:
		if event.Detail != "" {
			o.logf("%s: %s", event.Message, event.Detail)
			return
		}
		o.logf("%s", event.Message)
	}
}
