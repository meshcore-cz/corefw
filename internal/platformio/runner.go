package platformio

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/arnal/corefw/internal/progress"
)

// ErrNotFound is returned when the pio executable is not in PATH.
var ErrNotFound = errors.New("platformio executable not found")

// Options controls one PlatformIO invocation.
type Options struct {
	Context    context.Context
	Dir        string
	Env        string
	Upload     bool
	Port       string
	RawLogPath string
	RawOutput  io.Writer
	Reporter   progress.Reporter
}

// Result summarizes a PlatformIO run.
type Result struct {
	RawLogPath string
}

// Run invokes PlatformIO and parses its output into progress events.
func Run(opts Options) (*Result, error) {
	reporter := progress.Safe(opts.Reporter)
	if _, err := exec.LookPath("pio"); err != nil {
		return nil, ErrNotFound
	}

	logPath := opts.RawLogPath
	if logPath == "" {
		logPath = filepath.Join(opts.Dir, "platformio.log")
	}
	if err := os.MkdirAll(filepath.Dir(logPath), 0o755); err != nil {
		return nil, err
	}
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return nil, err
	}
	defer logFile.Close()

	ctx := opts.Context
	if ctx == nil {
		ctx = context.Background()
	}
	args := Args(opts.Env, opts.Upload, opts.Port)
	fmt.Fprintf(logFile, "$ pio %s\n\n", strings.Join(args, " "))

	phase := progress.PhaseCompile
	action := "Building firmware"
	if opts.Upload {
		phase = progress.PhaseUpload
		action = "Flashing firmware"
	}

	cmd := exec.CommandContext(ctx, "pio", args...)
	cmd.Dir = opts.Dir

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		reporter.Report(event(progress.PhasePrepareToolchain, progress.StatusFailed, progress.LevelError, "Failed to prepare PlatformIO", err.Error()))
		return nil, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		reporter.Report(event(progress.PhasePrepareToolchain, progress.StatusFailed, progress.LevelError, "Failed to prepare PlatformIO", err.Error()))
		return nil, err
	}
	reporter.Report(event(progress.PhasePrepareToolchain, progress.StatusCompleted, progress.LevelInfo, "PlatformIO ready", logPath))
	reporter.Report(event(phase, progress.StatusStarted, progress.LevelInfo, action, ""))
	if err := cmd.Start(); err != nil {
		reporter.Report(event(progress.PhasePrepareToolchain, progress.StatusFailed, progress.LevelError, "Failed to start PlatformIO", err.Error()))
		return nil, err
	}

	parser := NewParser()
	var logMu sync.Mutex
	var wg sync.WaitGroup
	var scanErrMu sync.Mutex
	var scanErr error
	scan := func(stream string, r io.Reader) {
		defer wg.Done()
		if err := scanPlatformIO(stream, r, parser, reporter, logFile, opts.RawOutput, &logMu); err != nil {
			scanErrMu.Lock()
			if scanErr == nil {
				scanErr = err
			}
			scanErrMu.Unlock()
		}
	}

	wg.Add(2)
	go scan("stdout", stdout)
	go scan("stderr", stderr)
	waitErr := cmd.Wait()
	wg.Wait()

	if scanErr != nil && waitErr == nil {
		return &Result{RawLogPath: logPath}, scanErr
	}
	if waitErr != nil {
		reporter.Report(event(phase, progress.StatusFailed, progress.LevelError, "PlatformIO failed", logPath))
		return &Result{RawLogPath: logPath}, fmt.Errorf("PlatformIO failed (see %s): %w", logPath, waitErr)
	}

	reporter.Report(event(phase, progress.StatusCompleted, progress.LevelInfo, "PlatformIO completed", logPath))
	return &Result{RawLogPath: logPath}, nil
}

// Args returns the pio arguments for a build or upload.
func Args(env string, upload bool, port string) []string {
	args := []string{"run", "-e", env}
	if upload {
		args = append(args, "-t", "upload")
		if port != "" {
			args = append(args, "--upload-port", port)
		}
	}
	return args
}

// ManualCommand returns a shell command users can run directly.
func ManualCommand(outDir, env string, upload bool, port string) string {
	c := fmt.Sprintf("pio run -e %s -d %s", env, outDir)
	if upload {
		c += " -t upload"
		if port != "" {
			c += " --upload-port " + port
		}
	}
	return c
}

func scanPlatformIO(stream string, r io.Reader, parser *Parser, reporter progress.Reporter, logFile io.Writer, rawOutput io.Writer, logMu *sync.Mutex) error {
	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for scanner.Scan() {
		line := scanner.Text()
		logMu.Lock()
		fmt.Fprintln(logFile, line)
		if rawOutput != nil {
			fmt.Fprintln(rawOutput, line)
		}
		logMu.Unlock()
		for _, event := range parser.ParseLine(stream, line) {
			reporter.Report(event)
		}
	}
	return scanner.Err()
}

func event(phase progress.Phase, status progress.Status, level progress.Level, message, detail string) progress.Event {
	return progress.Event{
		Time:    time.Now(),
		Phase:   phase,
		Status:  status,
		Level:   level,
		Message: message,
		Detail:  detail,
	}
}
