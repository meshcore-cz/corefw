package cli

import (
	"context"
	"io"
	"os"
	"os/exec"
	"strings"

	"github.com/arnal/corefw/internal/build"
	"github.com/arnal/corefw/internal/platformio"
)

// Dependencies contains injectable edges for command tests and production IO.
type Dependencies struct {
	Stdout io.Writer
	Stderr io.Writer
	Stdin  io.Reader

	Getenv   func(string) string
	LookPath func(string) (string, error)

	BuildRunner   BuildRunner
	PlanLoader    PlanLoader
	DeviceLister  DeviceLister
	MonitorRunner MonitorRunner

	WorkingDirectory func() (string, error)
	IsStdoutTerminal func() bool
}

type BuildRunner interface {
	Run(context.Context, build.Options) (*build.Result, error)
}

type BuildRunnerFunc func(context.Context, build.Options) (*build.Result, error)

func (f BuildRunnerFunc) Run(ctx context.Context, opts build.Options) (*build.Result, error) {
	return f(ctx, opts)
}

type PlanLoader interface {
	Load(context.Context, PlanOptions) (*PlanResult, error)
}

type PlanLoaderFunc func(context.Context, PlanOptions) (*PlanResult, error)

func (f PlanLoaderFunc) Load(ctx context.Context, opts PlanOptions) (*PlanResult, error) {
	return f(ctx, opts)
}

type DeviceLister interface {
	List(context.Context, DeviceListOptions) ([]DeviceOutput, error)
}

type DeviceListerFunc func(context.Context, DeviceListOptions) ([]DeviceOutput, error)

func (f DeviceListerFunc) List(ctx context.Context, opts DeviceListOptions) ([]DeviceOutput, error) {
	return f(ctx, opts)
}

type MonitorRunner interface {
	Run(context.Context, MonitorOptions) error
}

type MonitorRunnerFunc func(context.Context, MonitorOptions) error

func (f MonitorRunnerFunc) Run(ctx context.Context, opts MonitorOptions) error {
	return f(ctx, opts)
}

// DefaultDependencies returns production command dependencies.
func DefaultDependencies() Dependencies {
	deps := Dependencies{
		Stdout:           os.Stdout,
		Stderr:           os.Stderr,
		Stdin:            os.Stdin,
		Getenv:           os.Getenv,
		LookPath:         exec.LookPath,
		WorkingDirectory: os.Getwd,
		IsStdoutTerminal: func() bool { return isTerminalWriter(os.Stdout) },
	}
	deps.BuildRunner = BuildRunnerFunc(func(ctx context.Context, opts build.Options) (*build.Result, error) {
		opts.Context = ctx
		return build.Run(opts)
	})
	deps.PlanLoader = PlanLoaderFunc(defaultLoadPlan)
	deps.DeviceLister = DeviceListerFunc(func(ctx context.Context, opts DeviceListOptions) ([]DeviceOutput, error) {
		devices, err := platformio.ListDevices(ctx, platformio.DeviceListOptions{All: opts.All, LookPath: deps.LookPath})
		if err != nil {
			return nil, err
		}
		out := make([]DeviceOutput, len(devices))
		for i, d := range devices {
			out[i] = DeviceOutput{Port: d.Port, Description: d.Description, HardwareID: d.HardwareID, BoardHint: d.BoardHint}
		}
		return out, nil
	})
	deps.MonitorRunner = MonitorRunnerFunc(func(ctx context.Context, opts MonitorOptions) error {
		return platformio.Monitor(ctx, platformio.MonitorOptions{
			Port:     opts.Port,
			Baud:     opts.Baud,
			Filters:  opts.Filters,
			Project:  opts.Project,
			Env:      opts.Env,
			Stdin:    deps.Stdin,
			Stdout:   deps.Stdout,
			Stderr:   deps.Stderr,
			LookPath: deps.LookPath,
		})
	})
	return deps.withDefaults()
}

func (d Dependencies) withDefaults() Dependencies {
	if d.Stdout == nil {
		d.Stdout = io.Discard
	}
	if d.Stderr == nil {
		d.Stderr = io.Discard
	}
	if d.Stdin == nil {
		d.Stdin = stringsReader("")
	}
	if d.Getenv == nil {
		d.Getenv = func(string) string { return "" }
	}
	if d.LookPath == nil {
		d.LookPath = exec.LookPath
	}
	if d.WorkingDirectory == nil {
		d.WorkingDirectory = os.Getwd
	}
	if d.IsStdoutTerminal == nil {
		d.IsStdoutTerminal = func() bool { return false }
	}
	if d.BuildRunner == nil {
		d.BuildRunner = BuildRunnerFunc(func(ctx context.Context, opts build.Options) (*build.Result, error) {
			opts.Context = ctx
			return build.Run(opts)
		})
	}
	if d.PlanLoader == nil {
		d.PlanLoader = PlanLoaderFunc(defaultLoadPlan)
	}
	if d.DeviceLister == nil {
		d.DeviceLister = DeviceListerFunc(func(ctx context.Context, opts DeviceListOptions) ([]DeviceOutput, error) {
			devices, err := platformio.ListDevices(ctx, platformio.DeviceListOptions{All: opts.All, LookPath: d.LookPath})
			if err != nil {
				return nil, err
			}
			out := make([]DeviceOutput, len(devices))
			for i, dev := range devices {
				out[i] = DeviceOutput{Port: dev.Port, Description: dev.Description, HardwareID: dev.HardwareID, BoardHint: dev.BoardHint}
			}
			return out, nil
		})
	}
	if d.MonitorRunner == nil {
		d.MonitorRunner = MonitorRunnerFunc(func(ctx context.Context, opts MonitorOptions) error {
			return platformio.Monitor(ctx, platformio.MonitorOptions{
				Port:     opts.Port,
				Baud:     opts.Baud,
				Filters:  opts.Filters,
				Project:  opts.Project,
				Env:      opts.Env,
				Stdin:    d.Stdin,
				Stdout:   d.Stdout,
				Stderr:   d.Stderr,
				LookPath: d.LookPath,
			})
		})
	}
	return d
}

func stringsReader(s string) io.Reader {
	return strings.NewReader(s)
}
