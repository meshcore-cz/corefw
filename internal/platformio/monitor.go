package platformio

import (
	"context"
	"io"
	"os/exec"
	"strconv"
)

type MonitorOptions struct {
	Port    string
	Baud    int
	Filters []string
	Project string
	Env     string

	Stdin    io.Reader
	Stdout   io.Writer
	Stderr   io.Writer
	LookPath func(string) (string, error)
}

func Monitor(ctx context.Context, opts MonitorOptions) error {
	lookPath := opts.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if _, err := lookPath("pio"); err != nil {
		return ErrNotFound
	}
	args := MonitorArgs(opts)
	cmd := exec.CommandContext(ctx, "pio", args...)
	cmd.Stdin = opts.Stdin
	cmd.Stdout = opts.Stdout
	cmd.Stderr = opts.Stderr
	if opts.Project != "" {
		cmd.Dir = opts.Project
	}
	return cmd.Run()
}

func MonitorArgs(opts MonitorOptions) []string {
	args := []string{"device", "monitor"}
	if opts.Port != "" {
		args = append(args, "--port", opts.Port)
	}
	if opts.Baud > 0 {
		args = append(args, "--baud", strconv.Itoa(opts.Baud))
	}
	for _, filter := range opts.Filters {
		args = append(args, "--filter", filter)
	}
	if opts.Project != "" {
		args = append(args, "-d", opts.Project)
	}
	if opts.Env != "" {
		args = append(args, "-e", opts.Env)
	}
	return args
}
